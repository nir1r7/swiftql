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
#include "planner/logical_plan.h"
#include "planner/cardinality_estimator.h"
#include "planner/predicate_pushdown.h"
#include "planner/join_enumeration.h"
#include "planner/vec_plan_node.h"
#include "planner/vectorized_plan_builder.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "common/schema.h"
#include "common/value.h"

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
    bool no_optimize = false; // gates the vectorized optimizer (Week 21 pushdown + Week 20 estimates)
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
    std::string est;      // "est=N" when the Week 20 estimator ran, else empty
    std::string time;
    std::string pct;
};

// Render an estimated row count as a whole number.
//
// NOT std::llround: estimates are doubles and a join tree multiplies them, so a
// wide self-join overflows int64_t long before anything is wrong with the plan.
// llround outside int64_t range is undefined and yields INT64_MIN on x86-64,
// which printed `est=-9223372036854775808` on a ten-way join — inside the DP
// limit, so it is a configuration Week 28 advertises rather than an exotic one. A
// negative row estimate on the checkpoint surface is exactly what --explain
// exists to prevent. Uses the same fixed/setprecision(0) rendering the cost=
// string already relies on, which carries large magnitudes correctly.
static std::string formatEstimate(double rows) {
    std::ostringstream e;
    e << std::fixed << std::setprecision(0) << rows;
    return e.str();
}

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
    // estimates print in both --explain and --explain-analyze; the -1 sentinel
    // (--no-optimize, Volcano) leaves the field empty and printAligned hides
    // the whole column
    if (node->estimated_rows >= 0.0) {
        line.est = "est=" + formatEstimate(node->estimated_rows);
    }
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

// Week 23: logical-plan walker for the --explain sections. The logical tree is
// consumed by lowering, so callers must capture lines while it still exists.
void collectLogicalNodes(const LogicalPlanNode* node, int depth, std::vector<NodeLine>& out) {
    NodeLine line;
    line.label = std::string(depth * 2, ' ') + node->explain();
    if (node->estimated_rows >= 0.0) {
        line.est = "est=" + formatEstimate(node->estimated_rows);
    }
    out.push_back(std::move(line));
    for (const auto& child : node->children) {
        collectLogicalNodes(child.get(), depth + 1, out);
    }
}

void printAligned(const std::vector<NodeLine>& lines) {
    auto dispLen = [](const std::string& s) {
        size_t extra = 0;
        for (unsigned char c : s) if ((c & 0xC0) == 0x80) ++extra;
        return s.size() - extra;
    };

    size_t w_label = 0, w_ri = 0, w_ro = 0, w_est = 0, w_time = 0;
    for (const auto& l : lines) {
        w_label = std::max(w_label, dispLen(l.label));
        w_ri    = std::max(w_ri,    l.rows_in.size());
        w_ro    = std::max(w_ro,    l.rows_out.size());
        w_est   = std::max(w_est,   l.est.size());
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
        // est= sits directly after rows_out= — benchmark.py's q-error regex
        // anchors on that adjacency. Explicit std::left: in plain --explain
        // rows_in is empty, so the manipulator above never runs.
        if (w_est > 0){
            std::cout << std::left << std::setw(static_cast<int>(w_est + gap)) << l.est;
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
            // Week 19: statistics are table-scoped, not query-scoped — compute once
            // per process, before columnar conversion frees the row data
            if (!catalog.hasStats(stmt.from_table)) {
                catalog.setStats(stmt.from_table, TableStats::compute(table_rows[stmt.from_table], meta.schema));
            }
            // one load per joined table; a self-join names the same table twice,
            // and table_rows is keyed by table name, so the guard matters
            for (const auto& j : stmt.joins) {
                if (table_rows.count(j.join_table)) continue;
                const TableMetadata& jmeta = catalog.getTable(j.join_table);
                table_rows[j.join_table] = CSVLoader::load(jmeta.filepath, jmeta.schema);
                if (!catalog.hasStats(j.join_table)) {
                    catalog.setStats(j.join_table, TableStats::compute(table_rows[j.join_table], jmeta.schema));
                }
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

                auto plan_start = std::chrono::high_resolution_clock::now();

                // bind -> logical plan (validates internally) -> optimize -> lower
                auto logical = LogicalPlanBuilder::build(std::move(stmt), catalog);

                // Week 23: pushdown rewrites and lowering both consume the tree,
                // so each --explain section is captured as text while its stage
                // still exists — pre-optimization here, optimized below.
                std::vector<NodeLine> logical_lines;
                if (args.explain) {
                    collectLogicalNodes(logical.get(), 0, logical_lines);
                }

                if (!args.no_optimize) {
                    // Week 21: push single-relation WHERE predicates onto their
                    // own scan and order scan-local conjuncts by expected work.
                    // Reshapes the tree first so the estimator below annotates
                    // the final shape. --no-optimize skips this entirely.
                    logical = PredicatePushdown::apply(std::move(logical), catalog);

                    // Week 28: choose the join order. AFTER pushdown, so each
                    // relation's leaf carries its own filters and costs at its
                    // FILTERED cardinality; BEFORE estimation, so the stamps
                    // --explain prints and VectorizedPlanBuilder costs describe
                    // the tree that will actually run. Deliberately inside this
                    // block: --no-optimize keeps the written order, which is both
                    // the benchmark baseline and the differential oracle
                    // (compare_against_sqlite.py runs the vectorized suite twice,
                    // and a reordering that only happens with the optimizer on is
                    // what makes the second run able to catch it).
                    logical = JoinEnumeration::apply(std::move(logical), catalog);

                    // Week 20: annotate the (possibly rewritten) logical plan
                    // with estimated row counts.
                    CardinalityEstimator::estimate(*logical, catalog);
                }

                std::vector<NodeLine> optimized_lines;
                if (args.explain && !args.no_optimize) {
                    collectLogicalNodes(logical.get(), 0, optimized_lines);
                }

                std::unique_ptr<VecPlanNode> vec_node = VectorizedPlanBuilder::build(
                    std::move(logical), std::move(columnar_tables), catalog);

                double plan_us = std::chrono::duration<double, std::micro>(
                    std::chrono::high_resolution_clock::now() - plan_start).count();

                if (args.explain) {
                    std::cout << "=== Logical Plan ===\n";
                    printAligned(logical_lines);
                    if (!optimized_lines.empty()) {
                        std::cout << "\n=== Optimized Logical Plan ===\n";
                        printAligned(optimized_lines);
                    }
                    std::cout << "\n=== Physical Plan ===\n";
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
                            row.push_back(valueAt(cv, r));
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
