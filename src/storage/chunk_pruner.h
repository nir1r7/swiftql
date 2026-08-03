#pragma once

#include "columnar_table.h"
#include "parser/ast.h"
#include <tuple>

namespace {
    // collect triples (col_name, operator, literal_value) from AND connected predicates
    // skip OR predicates, complexity not worth pay off
    void collectSimplePredicates(const Expr* expr, std::vector<std::tuple<std::string, std::string, Value>>& out){
        if (!expr) return;
        const auto* bin = dynamic_cast<const BinaryExpr*>(expr);
        if (!bin) return;

        if (bin->op == "AND"){
            collectSimplePredicates(bin->left.get(), out);
            collectSimplePredicates(bin->right.get(), out);
            return;
        }

        // accept ColumnRef operator Literal.
        // A conjunct is only prunable when its refs belong to the scanned
        // table. Pushed conjuncts arrive re-stamped slot 0 (see
        // predicate_pushdown.cc), matching the standalone scan schema — for
        // both join sides. A slot >= 1 ref reaching a scan hint can only be a
        // residual or un-pushed (--no-optimize) predicate routed to the
        // FROM-side scan, where a shared column name (e.g. both tables have
        // `team`) would prune the FROM table on the JOIN table's value —
        // those must stay ignored. slot < 1 covers scan-local (0) and
        // unresolved/single-table (-1).
        const auto* col = dynamic_cast<const ColumnRef*>(bin->left.get());
        const auto* lit = dynamic_cast<const Literal*>(bin->right.get());
        if (col && lit && col->relation_slot < 1){
            out.emplace_back(col->column_name, bin->op, lit->value);
        }
    }
}

struct ChunkPruner {
    static bool canSkipChunk(const std::string& op, const Value& val, const ColumnChunk& chunk){
        const Value& mn = chunk.min_val;
        const Value& mx = chunk.max_val;

        if (op == "=") return val < mn || val > mx;
        if (op == "<") return val <= mn;
        if (op == ">") return val >= mx;
        if (op == "<=") return val < mn;
        if (op == ">=") return val > mx;
        return false;
    }

    static bool shouldSkip(const Expr* where, const std::unordered_map<std::string, std::vector<ColumnChunk>>& zone_maps, int chunk_idx){
        if (!where) return false;
        std::vector<std::tuple<std::string, std::string, Value>> preds;
        collectSimplePredicates(where, preds);
    

        for (const auto& [col, op, val] : preds) {
            auto it = zone_maps.find(col);
            if (it == zone_maps.end()) continue; // col is not in zone map
            if (chunk_idx >= static_cast<int>(it->second.size())) continue;
            if (canSkipChunk(op, val, it->second[chunk_idx])){
                // one sub predicate alone proves skip
                return true;
            }
        }
        return false;
    }
};