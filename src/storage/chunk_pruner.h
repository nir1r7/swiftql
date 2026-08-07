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
        // Week 30. `relation_slot < 1` is a test on a slot, and since Week 30 a
        // slot is a position in the range table of the scope `query_level`
        // blocks out. A CORRELATED ref carries (level 1, slot 0), which reads as
        // scan-local here and is then matched against the scanned table's zone
        // maps BY NAME below — two numbering domains, and with a shared column
        // name (`team`, `driver_id`) the wrong relation's zone maps prune the
        // scan. Silently skipped chunks, no error: invariant 12's subject and
        // the worst failure mode in this file.
        //
        // Unreachable today (execution runs after Validator refuses a subquery),
        // and NOT protected by the collectSlots/soleSlot `-1` containment that
        // covers restampSlots: vectorized_plan_builder hands the whole un-pushed
        // WHERE to the FROM-side scan as a hint, so on `--no-optimize` a
        // correlated conjunct arrives here without pushdown ever seeing it.
        //
        // DECLINE rather than throw. A pruning hint is an optimization, so
        // contributing nothing is correct-and-slower — the "decline and fall
        // back" pattern development.md prescribes for exactly this. (Compare
        // buildAggregateSchema, which throws for a correlated GROUP BY key:
        // grouping is not an optimization and has no correct fallback.)
        const auto* col = dynamic_cast<const ColumnRef*>(bin->left.get());
        const auto* lit = dynamic_cast<const Literal*>(bin->right.get());
        // Week 31 adds the second decline, for the same reason as the first. A
        // NULL literal became possible when a materialized scalar subquery
        // returned zero rows: `speed > NULL` is UNKNOWN for every row, so no
        // zone map can say anything about it. canSkipChunk would in fact return
        // false for every chunk — Value's comparisons are three-valued — but
        // that is an accident of the comparison operators, not a stated rule,
        // and this file's whole subject is not resting on accidents.
        if (col && lit && !lit->value.isNull()
            && col->query_level == 0 && col->relation_slot < 1){
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