#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <iomanip>
#include <chrono>
#include <cmath>

#include "catalog/catalog.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "planner/plan_nodes.h"
#include "planner/vec_plan_node.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "common/schema.h"
#include "common/value.h"
#include "execution/vec_scan_node.h"

// result cache: raw SQL to {schema, rows}
static std::unordered_map<std::string, std::pair<Schema, std::vector<Row>>> result_cache;

// argument parsing
struct Args {
    std::string catalog_path;
    std::vector<std::string> queries;
    bool explain = false;
    bool explain_analyze = false;
    bool no_cache = false;
    bool no_optimize = false; // accepted, ignored in Phase 1
    bool storage_stats = false;  // print columnar byte sizes and exit
    std::string storage = "row"; // default to row
    std::string execution = "volcano";
};

Args parseArgs(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string flag(argv[i]);
        if (flag == "--catalog" && i+1 < argc) args.catalog_path = argv[++i];
        else if (flag == "--query" && i+1 < argc) args.queries.push_back(argv[++i]);
        else if (flag == "--storage" && i+1 < argc) args.storage = argv[++i];
        else if (flag == "--execution" && i+1 < argc) args.execution = argv[++i];
        else if (flag == "--explain") args.explain = true;
        else if (flag == "--explain-analyze") args.explain_analyze = true;
        else if (flag == "--no-cache") args.no_cache = true;
        else if (flag == "--no-optimize") args.no_optimize = true;
        else if (flag == "--storage-stats") args.storage_stats = true;
        else std::cerr << "Unknown argument: " << flag << "\n";
    }
    return args;
}

void printResults(const std::vector<Row>& rows, const Schema& schema) {
    const auto& cols = schema.columns();
    int ncols = static_cast<int>(cols.size());

    std::vector<size_t> widths(ncols);
    for (int i = 0; i < ncols; ++i){
        widths[i] = cols[i].name.size();
    }
    for (const auto& row : rows){
        for (int i = 0; i < ncols; ++i){
            widths[i] = std::max(widths[i], row[i].toString().size());
        }
    }

    // header
    for (int i = 0; i < ncols; ++i){
        std::cout << std::left << std::setw(static_cast<int>(widths[i]) + 2) << cols[i].name;
    }
    std::cout << "\n";

    // separator
    size_t total_width = 0;
    for (auto w : widths) total_width += w + 2;
    std::cout << std::string(total_width, '-') << "\n";

    // rows
    for (const auto& row : rows) {
        for (int i = 0; i < ncols; ++i){
            std::cout << std::left << std::setw(static_cast<int>(widths[i]) + 2) << row[i].toString();
        }
        std::cout << "\n";
    }
}

struct NodeLine {
    std::string label;
    std::string rows_in;
    std::string rows_out;
    std::string time;
    std::string pct;
};

// Format microseconds with ~4 significant digits, no scientific notation.
static std::string formatMicros(double us) {
    if (us == 0.0) return "0";
    int exp = us >= 1.0 ? static_cast<int>(std::log10(us)) : 0;
    std::ostringstream t;
    t << std::fixed << std::setprecision(std::max(0, 3 - exp)) << us;
    return t.str();
}

void collectNodes(PlanNode* node, int depth, bool analyze,
                  double exec_total_us, std::vector<NodeLine>& out) {
    NodeLine line;
    line.label = std::string(depth * 2, ' ') + node->explain();
    if (analyze) {
        if (node->stats.rows_in > 0){
            line.rows_in = "rows_in=" + std::to_string(node->stats.rows_in);
        }
        if (node->stats.rows_out > 0){
            line.rows_out = "rows_out=" + std::to_string(node->stats.rows_out);
        }
        line.time = "time=" + formatMicros(node->stats.elapsed_us) + "µs";
        if (exec_total_us > 0.0) {
            std::ostringstream p;
            p << std::fixed << std::setprecision(1) << (node->stats.elapsed_us / exec_total_us * 100.0);
            line.pct = "(" + p.str() + "%)";
        }
    }
    out.push_back(std::move(line));
    for (PlanNode* child : node->children()){
        collectNodes(child, depth + 1, analyze, exec_total_us, out);
    }
}

void collectVecNodes(VecPlanNode* node, int depth, bool analyze, double exec_total_us, std::vector<NodeLine>& out) {
    NodeLine line;
    line.label = std::string(depth * 2, ' ') + node->explain();
    if (analyze) {
        if (node->stats.rows_in > 0){
            line.rows_in = "rows_in=" + std::to_string(node->stats.rows_in);
        }
        if (node->stats.rows_out > 0){
            line.rows_out = "rows_out=" + std::to_string(node->stats.rows_out);
        }
        line.time = "time=" + formatMicros(node->stats.elapsed_us) + "µs";
        if (exec_total_us > 0.0) {
            std::ostringstream p;
            p << std::fixed << std::setprecision(1) << (node->stats.elapsed_us / exec_total_us * 100.0);
            line.pct = "(" + p.str() + "%)";
        }
    }
    out.push_back(std::move(line));
    for (VecPlanNode* child : node->children()){
        collectVecNodes(child, depth + 1, analyze, exec_total_us, out);
    }
}

void printAligned(const std::vector<NodeLine>& lines) {
    auto dispLen = [](const std::string& s) {
        size_t extra = 0;
        for (unsigned char c : s) if ((c & 0xC0) == 0x80) ++extra;
        return s.size() - extra;
    };

    size_t w_label = 0, w_ri = 0, w_ro = 0, w_time = 0;
    for (const auto& l : lines) {
        w_label = std::max(w_label, dispLen(l.label));
        w_ri    = std::max(w_ri,    l.rows_in.size());
        w_ro    = std::max(w_ro,    l.rows_out.size());
        w_time  = std::max(w_time,  dispLen(l.time));
    }
    const size_t gap = 3;

    for (const auto& l : lines) {
        std::cout << l.label << std::string(w_label - dispLen(l.label) + gap, ' ');
        if (w_ri > 0){
            std::cout << std::left << std::setw(static_cast<int>(w_ri + gap)) << l.rows_in;
        }
        if (w_ro > 0){
            std::cout << std::setw(static_cast<int>(w_ro + gap)) << l.rows_out;
        }
        if (!l.time.empty()) {
            std::cout << l.time << std::string(w_time - dispLen(l.time) + gap, ' ');
        }
        std::cout << l.pct << "\n";
    }
}

// main
int main(int argc, char* argv[]) {
    Args args = parseArgs(argc, argv);

    if (args.catalog_path.empty() || args.queries.empty()) {
        std::cerr << "Usage: swiftql --catalog <path> --query \"<sql>\" [--query \"<sql>\" ...]\n";
        return 1;
    }

    try {
        Catalog catalog(args.catalog_path);
        bool multi = args.queries.size() > 1;

        for (const auto& query : args.queries) {
            if (multi){
                std::cout << "\n-- " << query << "\n";
            }

            // time parsing
            auto parse_start = std::chrono::high_resolution_clock::now();
            Parser parser(query);
            auto stmt = parser.parse();
            double parse_us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - parse_start).count();

            // load CSV data (excluded from timing, per benchmark methodology)
            std::unordered_map<std::string, std::vector<Row>> table_rows;
            const TableMetadata& meta = catalog.getTable(stmt.from_table);
            table_rows[stmt.from_table] = CSVLoader::load(meta.filepath, meta.schema);
            if (stmt.join.has_value()) {
                const TableMetadata& jmeta = catalog.getTable(stmt.join->join_table);
                table_rows[stmt.join->join_table] = CSVLoader::load(jmeta.filepath, jmeta.schema);
            }

            // build columnar tables if --storage columnar
            // part of loading section
            std::unordered_map<std::string, ColumnarTable> columnar_tables;
            if (args.storage == "columnar") {
                for (const auto& [name, rows] : table_rows) {
                    const Schema& s = catalog.getTable(name).schema;
                    columnar_tables.emplace(name, CSVToColumnar::convert(rows, s));
                }
                table_rows.clear(); // row data no longer needed; free before plan timer starts
            
                if (args.storage_stats) {
                    for (const auto& [name, ct] : columnar_tables) {
                        size_t mb = columnarTableByteSize(ct) / (1024 * 1024);
                        std::cout << name << " (columnar): " << mb << " MB\n";
                    }
                    return 0;  // exit after first query; storage size doesn't change per query
                }
            }

            if (args.execution == "vectorized"){
                if (args.storage != "columnar"){
                    std::cerr << "Error: --execution vectorized requires --storage columnar\n";
                    return 1;
                }

                const TableMetadata& meta = catalog.getTable(stmt.from_table);
                auto vec_scan = std::make_unique<VecScanNode>(stmt.from_table, columnar_tables.at(stmt.from_table), meta.schema);

                vec_scan->open();
                std::vector<Row> rows;

                // temporary, will be reaplced in week 14
                while (DataChunk* chunk = vec_scan->nextChunk()){
                    for (int r = 0; r < chunk->num_rows; ++r){
                        Row row;
                        row.reserve(chunk->columns.size());
                        for (const auto& cv : chunk->columns){
                            std::visit([&](const auto& vec){
                                row.push_back(Value(vec[r]));
                            }, cv.data);
                        }
                        rows.push_back(std::move(row));
                    }
                }
                vec_scan->close();

                printResults(rows, meta.schema);
                continue;
            }
            

            // time planning
            auto plan_start = std::chrono::high_resolution_clock::now();
            auto plan = Planner::plan(std::move(stmt), catalog, std::move(table_rows), std::move(columnar_tables));
            double plan_us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - plan_start).count();

            if (args.explain) {
                std::vector<NodeLine> lines;
                collectNodes(plan.get(), 0, false, 0.0, lines);
                printAligned(lines);
                continue;
            }

            // cache lookup (skip for --explain-analyze and --no-cache)
            if (!args.no_cache && !args.explain_analyze) {
                auto it = result_cache.find(query);
                if (it != result_cache.end()) {
                    std::cout << "[cache hit]\n";
                    printResults(it->second.second, it->second.first);
                    continue;
                }
            }

            // execute
            auto exec_start = std::chrono::high_resolution_clock::now();
            plan->open();
            std::vector<Row> rows;
            while (Row* r = plan->next()){
                rows.push_back(*r);
            }
            plan->close();
            double total_us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - exec_start).count();

            if (args.explain_analyze) {
                std::vector<NodeLine> lines;
                collectNodes(plan.get(), 0, true, total_us, lines);
                printAligned(lines);
                std::cout << "\n";
                std::cout << "Rows returned: " << rows.size() << "\n\n";
                std::cout << "Parse:     " << std::fixed << std::setprecision(1) << parse_us     << "µs\n";
                std::cout << "Plan:      " << std::fixed << std::setprecision(1) << plan_us      << "µs\n";
                std::cout << "Execution: " << std::fixed << std::setprecision(1) << total_us     << "µs\n";
                continue;
            }

            if (!args.no_cache){
                result_cache.emplace(query, std::make_pair(plan->outputSchema(), rows));
            }

            printResults(rows, plan->outputSchema());
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
