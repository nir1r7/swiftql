#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <iomanip>
#include <chrono>

#include "catalog/catalog.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "planner/plan_nodes.h"
#include "common/schema.h"
#include "common/value.h"

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
    std::string storage = "row"; // accepted, ignored in Phase 1
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
        else std::cerr << "Unknown argument: " << flag << "\n";
    }
    return args;
}

void printResults(const std::vector<Row>& rows, const Schema& schema) {
    const auto& cols = schema.columns();
    int ncols = static_cast<int>(cols.size());

    std::vector<size_t> widths(ncols);
    for (int i = 0; i < ncols; ++i)
        widths[i] = cols[i].name.size();
    for (const auto& row : rows)
        for (int i = 0; i < ncols; ++i)
            widths[i] = std::max(widths[i], row[i].toString().size());

    // header
    for (int i = 0; i < ncols; ++i)
        std::cout << std::left << std::setw(static_cast<int>(widths[i]) + 2) << cols[i].name;
    std::cout << "\n";

    // separator
    size_t total_width = 0;
    for (auto w : widths) total_width += w + 2;
    std::cout << std::string(total_width, '-') << "\n";

    // rows
    for (const auto& row : rows) {
        for (int i = 0; i < ncols; ++i)
            std::cout << std::left << std::setw(static_cast<int>(widths[i]) + 2) << row[i].toString();
        std::cout << "\n";
    }
}

void printTree(PlanNode* node, int depth, bool analyze) {
    std::cout << std::string(depth * 2, ' ') << node->explain();
    if (analyze) {
        if (node->stats.rows_in > 0)
            std::cout << "  rows_in=" << node->stats.rows_in;
        if (node->stats.rows_out > 0)
            std::cout << "  rows_out=" << node->stats.rows_out;
        std::cout << "  time=" << std::fixed << std::setprecision(1)
                  << node->stats.elapsed_us << "µs";
    }
    std::cout << "\n";
    for (PlanNode* child : node->children())
        printTree(child, depth + 1, analyze);
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
            if (multi)
                std::cout << "\n-- " << query << "\n";

            Parser parser(query);
            auto stmt = parser.parse();
            auto plan = Planner::plan(std::move(stmt), catalog);

            if (args.explain) {
                printTree(plan.get(), 0, false);
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
            while (Row* r = plan->next())
                rows.push_back(*r);
            plan->close();
            double total_us = std::chrono::duration<double, std::micro>(
                std::chrono::high_resolution_clock::now() - exec_start).count();

            if (args.explain_analyze) {
                printTree(plan.get(), 0, true);
                std::cout << "Total: " << std::fixed << std::setprecision(1) << total_us << "µs\n";
                continue;
            }

            if (!args.no_cache)
                result_cache.emplace(query, std::make_pair(plan->outputSchema(), rows));

            printResults(rows, plan->outputSchema());
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
