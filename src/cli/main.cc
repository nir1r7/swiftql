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
#include "planner/binder.h"
#include "planner/plan_nodes.h"
#include "planner/vec_plan_node.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "common/schema.h"
#include "common/value.h"
#include "execution/vec_scan_node.h"
#include "execution/vec_filter_node.h"
#include "execution/vec_project_node.h"
#include "execution/vec_limit_node.h"
#include "execution/vec_sort_node.h"
#include "execution/vec_distinct_node.h"
#include "execution/vec_hash_aggregate_node.h"
#include "execution/vec_hash_join_node.h"

// result cache key: query text plus every flag
struct CacheKey {
    std::string query;
    std::string storage;
    std::string execution;
    bool no_optimize;

    bool operator==(const CacheKey& other) const {
        return query == other.query && storage == other.storage &&
               execution == other.execution && no_optimize == other.no_optimize;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const {
        size_t h = std::hash<std::string>{}(k.query);
        h ^= std::hash<std::string>{}(k.storage) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.execution) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.no_optimize) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<CacheKey, std::pair<Schema, std::vector<Row>>, CacheKeyHash> result_cache;

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
        line.rows_in = "rows_in=" + std::to_string(node->stats.rows_in);
        line.rows_out = "rows_out=" + std::to_string(node->stats.rows_out);
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
        // analyze mode implies full execution, so print rows_in/rows_out
        // unconditionally — a real 0 must be visible.
        line.rows_in = "rows_in=" + std::to_string(node->stats.rows_in);
        line.rows_out = "rows_out=" + std::to_string(node->stats.rows_out);
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
            Binder::bind(stmt, catalog);
            double parse_us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - parse_start).count();

            CacheKey cache_key{query, args.storage, args.execution, args.no_optimize};

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

                // Fix #3: validate before plan construction (same guarantee as Volcano path via Planner::plan)
                Validator::validate(stmt, catalog);

                // Capture row counts before columnar tables are moved into scan nodes
                int from_row_count = columnar_tables.at(stmt.from_table).num_rows;
                int join_row_count = (stmt.join.has_value() && columnar_tables.count(stmt.join->join_table))
                    ? columnar_tables.at(stmt.join->join_table).num_rows : 0;

                auto plan_start = std::chrono::high_resolution_clock::now();

                // Fix #2: narrow scan schema to only required columns (mirrors Planner::plan narrowSchema logic)
                Schema from_scan_schema = Planner::buildScanSchema(stmt, meta.schema);

                // Fix #1: pass WHERE predicate for zone-map chunk pruning (non-owning; VecFilterNode takes
                // ownership of the same expr at step 3, and is an ancestor of VecScanNode in the tree)
                std::unique_ptr<VecPlanNode> vec_node = std::make_unique<VecScanNode>(
                    stmt.from_table,
                    std::move(columnar_tables.at(stmt.from_table)),
                    from_scan_schema,
                    stmt.where.get());

                // Step 2: Join
                if (stmt.join.has_value()) {
                    const TableMetadata& join_meta = catalog.getTable(stmt.join->join_table);
                    Schema join_scan_schema = Planner::buildScanSchema(stmt, join_meta.schema);
                    auto join_scan = std::make_unique<VecScanNode>(
                        stmt.join->join_table,
                        std::move(columnar_tables.at(stmt.join->join_table)),
                        join_scan_schema);

                    std::string left_col, right_col, left_table, right_table;
                    if (auto* bin = dynamic_cast<BinaryExpr*>(stmt.join->condition.get())) {
                        if (auto* lc = dynamic_cast<ColumnRef*>(bin->left.get())) {
                            left_col   = lc->column_name;
                            left_table = lc->table_name;
                        }
                        if (auto* rc = dynamic_cast<ColumnRef*>(bin->right.get())) {
                            right_col   = rc->column_name;
                            right_table = rc->table_name;
                        }
                    }
                    // If table qualifiers are present, route by table name so that
                    // ON join_table.col = from_table.col is handled correctly.
                    // Without qualifiers, fall back to positional assumption (left=FROM, right=JOIN).
                    std::string from_col = left_col, join_col = right_col;
                    if (!left_table.empty() && !right_table.empty()) {
                        if (left_table == stmt.join->join_table)
                            std::swap(from_col, join_col);
                    }

                    bool swap = (from_row_count < join_row_count);
                    if (swap) {
                        // FROM is smaller: FROM becomes build, JOIN becomes probe
                        std::vector<ColumnDef> merged_cols = join_scan->outputSchema().columns();
                        for (const auto& col : vec_node->outputSchema().columns())
                            merged_cols.push_back(col);
                        vec_node = std::make_unique<VecHashJoinNode>(
                            std::move(join_scan), std::move(vec_node),
                            join_col, from_col, Schema(merged_cols));
                    } else {
                        // JOIN is smaller or equal: JOIN stays build, FROM stays probe
                        std::vector<ColumnDef> merged_cols = vec_node->outputSchema().columns();
                        for (const auto& col : join_scan->outputSchema().columns())
                            merged_cols.push_back(col);
                        vec_node = std::make_unique<VecHashJoinNode>(
                            std::move(vec_node), std::move(join_scan),
                            from_col, join_col, Schema(merged_cols));
                    }
                }

                // Step 3: Filter (WHERE)
                if (stmt.where) {
                    vec_node = std::make_unique<VecFilterNode>(
                        std::move(vec_node), std::move(stmt.where));
                }

                // Step 4: Aggregate (GROUP BY / aggregates in SELECT)
                bool has_aggs = false;
                for (const auto& expr : stmt.select_list) {
                    if (dynamic_cast<AggregateExpr*>(expr.get())) { has_aggs = true; break; }
                }
                if (!stmt.group_by.empty() || has_aggs) {
                    Schema agg_schema = Planner::buildAggregateSchema(stmt, vec_node->outputSchema());
                    auto specs = Planner::extractAggregates(stmt);
                    vec_node = std::make_unique<VecHashAggregateNode>(
                        std::move(vec_node), stmt.group_by, std::move(specs), agg_schema);
                }

                // Step 5: Having
                if (stmt.having) {
                    vec_node = std::make_unique<VecFilterNode>(
                        std::move(vec_node), std::move(stmt.having));
                }

                // Step 6: Sort (ORDER BY)
                if (!stmt.order_by.empty()) {
                    vec_node = std::make_unique<VecSortNode>(
                        std::move(vec_node), std::move(stmt.order_by));
                }

                // Step 7: Project — schema is final after agg/sort, so outputSchema() is correct here
                {
                    if (stmt.select_star) {
                        Schema star_schema = vec_node->outputSchema();
                        std::vector<std::unique_ptr<Expr>> star_exprs;
                        for (const auto& col : star_schema.columns()) {
                            auto ref = std::make_unique<ColumnRef>();
                            ref->column_name = col.name;
                            star_exprs.push_back(std::move(ref));
                        }
                        vec_node = std::make_unique<VecProjectNode>(
                            std::move(vec_node), std::move(star_exprs), star_schema);
                    } else {
                        Schema proj_schema = Planner::buildProjectSchema(stmt, vec_node->outputSchema());
                        vec_node = std::make_unique<VecProjectNode>(
                            std::move(vec_node), std::move(stmt.select_list), proj_schema);
                    }
                }

                // Step 8: Distinct
                if (stmt.distinct) {
                    vec_node = std::make_unique<VecDistinctNode>(std::move(vec_node));
                }

                // Step 9: Limit
                if (stmt.limit.has_value()) {
                    vec_node = std::make_unique<VecLimitNode>(
                        std::move(vec_node), stmt.limit.value());
                }

                double plan_us = std::chrono::duration<double, std::micro>(
                    std::chrono::high_resolution_clock::now() - plan_start).count();

                if (args.explain) {
                    std::vector<NodeLine> lines;
                    collectVecNodes(vec_node.get(), 0, false, 0.0, lines);
                    printAligned(lines);
                    continue;
                }

                if (!args.no_cache && !args.explain_analyze) {
                    auto it = result_cache.find(cache_key);
                    if (it != result_cache.end()) {
                        std::cout << "[cache hit]\n";
                        printResults(it->second.second, it->second.first);
                        continue;
                    }
                }

                auto exec_start = std::chrono::high_resolution_clock::now();
                vec_node->open();
                std::vector<Row> rows;
                while (DataChunk* chunk = vec_node->nextChunk()) {
                    int n = chunk->filter_applied
                        ? static_cast<int>(chunk->sel.indices.size())
                        : chunk->num_rows;
                    for (int i = 0; i < n; ++i) {
                        int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
                        Row row;
                        row.reserve(chunk->columns.size());
                        for (const auto& cv : chunk->columns) {
                            std::visit([&](const auto& vec) {
                                row.push_back(Value(vec[r]));
                            }, cv.data);
                        }
                        rows.push_back(std::move(row));
                    }
                }
                vec_node->close();
                double total_us = std::chrono::duration<double, std::micro>(
                    std::chrono::high_resolution_clock::now() - exec_start).count();

                if (args.explain_analyze) {
                    std::vector<NodeLine> lines;
                    collectVecNodes(vec_node.get(), 0, true, total_us, lines);
                    printAligned(lines);
                    std::cout << "\n";
                    std::cout << "Rows returned: " << rows.size() << "\n\n";
                    std::cout << "Parse:     " << std::fixed << std::setprecision(1) << parse_us  << "µs\n";
                    std::cout << "Plan:      " << std::fixed << std::setprecision(1) << plan_us   << "µs\n";
                    std::cout << "Execution: " << std::fixed << std::setprecision(1) << total_us  << "µs\n";
                    continue;
                }

                const Schema& out_schema = vec_node->outputSchema();
                if (!args.no_cache) {
                    result_cache.emplace(cache_key, std::make_pair(out_schema, rows));
                }
                printResults(rows, out_schema);
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
                auto it = result_cache.find(cache_key);
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
                result_cache.emplace(cache_key, std::make_pair(plan->outputSchema(), rows));
            }

            printResults(rows, plan->outputSchema());
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
