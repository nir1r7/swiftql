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
#include "planner/validator.h"
#include "planner/subquery_materialization.h"
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
    // Week 35: "aligned" (the default, unchanged) or "tsv". The aligned printer
    // pads to data-derived widths, which is unambiguous only while no value and
    // no COLUMN NAME contains two consecutive spaces and no value is empty.
    // TPC-H breaks all three: aggregateOutputName IS exprToString, which renders
    // a BinaryExpr as "(a * (1 - b))" -- spaces inside a single column name --
    // so a header split on whitespace shreds one column into seven and every row
    // is then silently dropped by the harness's field-count check. TSV removes
    // the ambiguity instead of teaching the parser to guess.
    std::string format = "aligned";
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
        else if (flag == "--format" && i+1 < argc) args.format = argv[++i];
        else std::cerr << "Unknown argument: " << flag << "\n";
    }
    if (args.format != "aligned" && args.format != "tsv") {
        throw std::runtime_error("--format must be 'aligned' or 'tsv', got '"
                                 + args.format + "'");
    }
    return args;
}

// Drain a vectorized plan into rows. Selection-vector aware: a chunk that has
// been filtered reports its surviving rows through `sel`, and reading num_rows
// instead returns rows the filter removed. Shared by the top-level run and by
// the nested-query runner below so there is one copy of that rule.
std::vector<Row> drainVec(VecPlanNode* node) {
    std::vector<Row> rows;
    while (DataChunk* chunk = node->nextChunk()) {
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
    return rows;
}

// Week 31. Plan and run one nested statement to completion, on the vectorized
// path. Mirrors the top-level path's stage order exactly — pushdown, then join
// enumeration, then estimation — so a nested query is optimized by the same
// passes in the same order as a top-level one.
//
// `no_optimize` is threaded through for the same reason it exists at the top
// level: compare_against_sqlite.py runs the vectorized suite twice, and that
// second leg is the differential oracle. A runner that always optimized would
// give both legs the same subquery result and quietly stop testing the sub-plan.
SubqueryResult runVectorizedToRows(SelectStatement stmt, const Catalog& catalog,
                                   std::unordered_map<std::string, ColumnarTable> tables,
                                   bool no_optimize) {
    auto logical = LogicalPlanBuilder::build(std::move(stmt), catalog);
    if (!no_optimize) {
        logical = PredicatePushdown::apply(std::move(logical), catalog);
        logical = JoinEnumeration::apply(std::move(logical), catalog);
        CardinalityEstimator::estimate(*logical, catalog);
    }
    auto node = VectorizedPlanBuilder::build(std::move(logical), std::move(tables), catalog);
    node->open();
    SubqueryResult out{node->outputSchema(), drainVec(node.get())};
    node->close();
    return out;
}

// The same, on the Volcano path. A three-or-more-relation body is refused here
// by the pre-existing Week 27 guard, with its existing message naming
// --execution vectorized: an ordinary capability difference, identical to the
// one a top-level multi-way join already has.
SubqueryResult runVolcanoToRows(SelectStatement stmt, const Catalog& catalog,
                                std::unordered_map<std::string, std::vector<Row>> table_rows,
                                std::unordered_map<std::string, ColumnarTable> columnar_tables) {
    auto plan = Planner::plan(std::move(stmt), catalog, std::move(table_rows),
                              std::move(columnar_tables));
    plan->open();
    std::vector<Row> rows;
    while (Row* r = plan->next()) rows.push_back(*r);
    plan->close();
    SubqueryResult out{plan->outputSchema(), std::move(rows)};
    return out;
}

// Week 35 — machine-readable output for the harnesses.
//
// The aligned printer below is for humans and stays the default. It is
// ambiguous to parse in exactly three ways TPC-H hits and the F1 suite never
// did: a COLUMN NAME containing spaces (aggregateOutputName IS exprToString,
// so `SUM(l_extendedprice * (1 - l_discount))` is ONE column whose name has
// six spaces in it), an EMPTY string value (which renders as pure padding and
// merges with its neighbour's gap), and a value containing two consecutive
// spaces. All three make a field-count check fail, and a harness that drops the
// mismatched row silently reports an empty result for a correct answer.
//
// A separator no value can contain removes the ambiguity at the source. A tab
// is that separator here: no TPC-H column contains one, and Value::toString
// never emits one. A NULL prints as the same "NULL" token the aligned printer
// uses, because normalize()'s NULL canonicalization is keyed on it.
void printResultsTsv(const std::vector<Row>& rows, const Schema& schema) {
    const auto& cols = schema.columns();
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) std::cout << '\t';
        std::cout << cols[i].name;
    }
    std::cout << "\n";
    for (const auto& row : rows) {
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) std::cout << '\t';
            std::cout << row[i].toString();
        }
        std::cout << "\n";
    }
    // An explicit terminator. Without it a zero-row result is one header line,
    // which a reader cannot distinguish from a truncated or failed run — the
    // same conflation parse_swiftql_output has today, where a genuine empty
    // result and a mis-parsed header both come back as [].
    std::cout << "(" << rows.size() << " rows)\n";
}

void printResultsAligned(const std::vector<Row>& rows, const Schema& schema) {
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

// One dispatch point, so every existing printResults call site keeps working
// and none of them has to know which format is active.
static std::string g_output_format = "aligned";

void printResults(const std::vector<Row>& rows, const Schema& schema) {
    if (g_output_format == "tsv") printResultsTsv(rows, schema);
    else                          printResultsAligned(rows, schema);
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
    Args args;
    try {
        args = parseArgs(argc, argv);
    } catch (const std::exception& e) {
        // parseArgs validates --format, and it runs BEFORE the main try block,
        // so its throw needs its own handler or an unknown format terminates
        // instead of reporting.
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    g_output_format = args.format;

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
            // Week 31: a NESTED query scans tables the outer FROM/JOIN list
            // never names, and this loop used to walk only those two. One walker
            // answers both this question and the rewrite below
            // (subquery_materialization.h), so the two cannot drift; before it,
            // `WHERE x > (SELECT AVG(age) FROM drivers)` on a FROM laps query
            // died with a raw std::out_of_range from table_rows.at().
            //
            // One load per table: a self-join names the same table twice, and
            // both this map and the catalog's statistics are keyed by table
            // name, so collectQueryTables dedupes and the guard stays.
            std::vector<std::string> needed_tables;
            collectQueryTables(stmt, needed_tables);
            for (const auto& tname : needed_tables) {
                if (table_rows.count(tname)) continue;
                // A table that does not exist is the VALIDATOR's message to give
                // ("Table not found: 'x'"), and it runs a few lines below. Before
                // this skip, a nested `FROM nosuchtable` was diagnosed by
                // catalog.getTable() as "Table name does not exist" — a worse
                // message, from a loader, about a query defect. The loader is not
                // a diagnostic site.
                if (!catalog.hasTable(tname)) continue;
                const TableMetadata& tmeta = catalog.getTable(tname);
                // Week 35: the ONE production loader call, and therefore the one
                // that must pass the table's own file format (delimiter, header,
                // trailing delimiter). Every other CSVLoader::load in the tree is
                // a tests/ fixture and genuinely is CSV.
                table_rows[tname] = CSVLoader::load(tmeta.filepath, tmeta.schema,
                                                    tmeta.format);
                // Week 19: statistics are table-scoped, not query-scoped —
                // compute once per process, before columnar conversion frees the
                // row data. A nested query's tables need them too: the optimizer
                // costs its sub-plan from the same statistics.
                if (!catalog.hasStats(tname)) {
                    catalog.setStats(tname, TableStats::compute(table_rows[tname], tmeta.schema));
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

            // Week 31 — uncorrelated subqueries, materialized once, ABOVE both
            // engines. One implementation, four modes agreeing by construction:
            // the same property Week 30 bought by putting one refusal at the end
            // of Validator::validate, rather than one guard per planner.
            //
            // DIAGNOSTICS FIRST. materializeSubqueries TRUSTS three Validator
            // rules — exactly one output column for SCALAR/IN, position
            // restricted to WHERE/HAVING, and no correlated subquery — so a
            // query breaking one must be refused before anything runs. Running
            // the pass first would materialize column 0 of a two-column scalar
            // subquery the Validator was about to reject: a wrong answer instead
            // of a diagnostic. validate() is pure, so the planners calling it
            // again below cost one extra walk and keep the ordering discipline
            // Week 26 established (a genuine query defect outranks an engine
            // limitation).
            //
            // It runs BEFORE --explain returns, too: --explain must go through
            // the pass or it prints a plan the engine cannot build. The cost is
            // that --explain executes the nested query, exactly as it already
            // performs constant folding — see README's Limitations.
            // Materializing a subquery EXECUTES a nested query, so its time is
            // real query work and is added to the plan timer below rather than
            // vanishing between the clocks the way the CSV load deliberately
            // does. Week 37 must be able to see it.
            double subquery_us = 0.0;
            if (stmt.has_subquery) {
                auto sub_start = std::chrono::high_resolution_clock::now();
                // Inside the guard: an ordinary query would otherwise validate
                // twice (here and inside its planner) for no benefit, and the
                // ordering property only exists to protect the pass below.
                Validator::validate(stmt, catalog);

                SubqueryRunner run_subquery;
                if (args.execution == "vectorized" && args.storage == "columnar") {
                    run_subquery = [&](SelectStatement body) {
                        // Its own copies: both scan nodes take their table BY
                        // VALUE and the outer query still needs the originals.
                        // Lowering's scan_uses counter already copies for a
                        // self-join, so this is the existing cost model rather
                        // than a new one — and the reason a shared table
                        // representation is on Week 37's list, not this week's.
                        std::unordered_map<std::string, ColumnarTable> tables;
                        std::vector<std::string> names;
                        collectQueryTables(body, names);
                        for (const auto& n : names) tables.emplace(n, columnar_tables.at(n));
                        return runVectorizedToRows(std::move(body), catalog,
                                                   std::move(tables), args.no_optimize);
                    };
                } else {
                    run_subquery = [&](SelectStatement body) {
                        std::unordered_map<std::string, std::vector<Row>> rows_copy;
                        std::unordered_map<std::string, ColumnarTable> cols_copy;
                        std::vector<std::string> names;
                        collectQueryTables(body, names);
                        for (const auto& n : names) {
                            if (columnar_tables.count(n)) cols_copy.emplace(n, columnar_tables.at(n));
                            else                          rows_copy.emplace(n, table_rows.at(n));
                        }
                        return runVolcanoToRows(std::move(body), catalog,
                                                std::move(rows_copy), std::move(cols_copy));
                    };
                }
                // The nested query runs on the SAME engine as the query that
                // contains it: handing a three-relation body to Volcano would
                // refuse TPC-H Q11's subquery in vectorized mode, which is a
                // capability difference invented by the plumbing rather than by
                // either engine.
                materializeSubqueries(stmt, run_subquery);
                subquery_us = std::chrono::duration<double, std::micro>(
                    std::chrono::high_resolution_clock::now() - sub_start).count();
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

                double plan_us = subquery_us + std::chrono::duration<double, std::micro>(
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
                std::vector<Row> rows = drainVec(vec_node.get());
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
            double plan_us = subquery_us + std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - plan_start).count();

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
