#pragma once

#include "columnar_table.h"
#include "parser/ast.h"
#include "parser/expr_totality.h"
#include "common/schema.h"
#include <tuple>

namespace {
    // collect triples (col_name, operator, literal_value) from AND connected predicates
    // skip OR predicates, complexity not worth pay off
    //
    // WALKS THE AND SPINE IN WRITTEN ORDER and STOPS at the first conjunct that
    // may raise. Returns false once it has stopped, so the caller's spine walk
    // unwinds without collecting anything further.
    //
    // SEAM AUDIT PASS 4, P4-1's second raise site — generalized, because the
    // throw was only half of it. A zone-map skip removes rows BEFORE the filter
    // runs, so it can mask a raise the filter owed. Measured at HEAD on the
    // shipped catalog, `--no-optimize` in every mode, so no optimizer pass is
    // involved:
    //
    //   SELECT lap_id FROM laps
    //   WHERE lap_id * 9223372036854775807 > 0 AND lap_id > 999999
    //     row storage       Error: integer overflow in '*'
    //     columnar storage  0 rows      (`lap_id > 999999` prunes every chunk)
    //
    // The rule that closes it is the SAME one PredicatePushdown obeys and the
    // same index: a conjunct is evaluated on the rows every conjunct written
    // BEFORE it kept, so only a conjunct written ahead of every raising one may
    // prove a skip. If C_k proves the skip and C_j may raise:
    //   j > k  — C_j would never have been evaluated on that chunk anyway, since
    //            C_k is false for every row in it. Skipping is invisible.
    //   j < k  — C_j IS evaluated on rows of that chunk, and skipping removes
    //            them. Unsound; this walker stops before reaching C_k.
    //
    // The screen is exprMayRaise/conjunctMayRaise (parser/expr_totality.h), the
    // one shared with the optimizer. It answers "may raise" for a conjunct this
    // scan's schema cannot type — which is every conjunct naming another
    // relation, so an un-pushed WHERE handed to the FROM-side scan of a join
    // stops contributing hints at its first cross-relation conjunct.
    //
    // THAT COST IS REAL AND MEASURED, not a theoretical concession. On
    // `laps JOIN drivers WHERE dr.nationality = 'British' AND l.season = 2024`
    // (shipped catalog, columnar/vectorized, median of 3): the OPTIMIZED leg is
    // unchanged at `chunks_skipped=1/2`, because pushdown gives each scan a hint
    // made only of its own conjuncts; the `--no-optimize` leg drops to
    // `chunks_skipped=0/2` and 42.5 ms becomes 51.5 ms, +21%.
    //
    // It is the honest price of the hint arriving here WITHOUT the schema it was
    // written against — guessing is what the wrong answer above was made of. The
    // fix is not in this file: `pruningHintForPreservedSide`
    // (predicate_pushdown.h) is the single place both planners route a hint
    // through, and threading the filter's child schema alongside the hint would
    // let this walker type every conjunct instead of only the scan-local ones.
    // That is one line in each of the two builders and is left to a round that
    // owns them.
    bool collectSimplePredicates(const Expr* expr, const Schema& schema,
                                 std::vector<std::tuple<std::string, std::string, Value>>& out){
        if (!expr) return true;
        const auto* bin = dynamic_cast<const BinaryExpr*>(expr);
        if (!bin) return !conjunctMayRaise(expr, schema);

        if (bin->op == "AND"){
            if (!collectSimplePredicates(bin->left.get(), schema, out)) return false;
            return collectSimplePredicates(bin->right.get(), schema, out);
        }

        // One conjunct. Whatever else is true of it, if it can raise then
        // nothing written after it may prune.
        if (conjunctMayRaise(bin, schema)) return false;

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
        // Week 33: REACHABLE. The refusal is gone and a correlated ref now
        // reaches a plan. Still a DECLINE, and now for the reason that survives
        // the refusal's removal rather than depending on it: a correlated ref
        // names a relation this scan is not scanning, so it can supply no hint
        // about THIS table's zone maps. Pruning is an optimization, so
        // contributing nothing is correct-and-slower; matching by name below
        // would prune the wrong relation's chunks silently.
        //
        // Was unreachable (execution ran after Validator refused a subquery),
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
            && col->id.isLocal()
            && col->id.localSlot("collectSimplePredicates") < 1){
            out.emplace_back(col->column_name, bin->op, lit->value);
        }
        return true;
    }
}

struct ChunkPruner {
    // A zone map answers "can any row of this chunk satisfy `col op val`?".
    //
    // IT MUST NOT THROW. `val < mn` is Value::operator<, which raises
    // "Type mismatch in Value comparison" across the STRING boundary
    // (value.cc's NUMERIC_COERCE) — and this runs at SCAN time, on metadata,
    // before a single row exists. Seam audit pass 4's P4-1 turned that into a
    // divergence in both directions on the shipped catalog
    // (`WHERE team = 5 AND speed > 999999`), decided purely by which conjunct
    // came first in the WHERE.
    //
    // The guard is a decline, not a fix for the query: a predicate comparing a
    // STRING column against a number still raises PER ROW, from the filter,
    // which is where it is written and where both engines and both legs agree
    // it belongs. All this rule says is that a metadata skip cannot be the
    // thing that decides it. Same "an optimization declines and falls back"
    // stance as collectSimplePredicates' three other declines above.
    static bool canSkipChunk(const std::string& op, const Value& val, const ColumnChunk& chunk){
        const Value& mn = chunk.min_val;
        const Value& mx = chunk.max_val;
        if (mn.isNull() || mx.isNull() || val.isNull()) return false;
        const bool val_str = val.type() == TypeId::STRING;
        if (val_str != (mn.type() == TypeId::STRING)
            || val_str != (mx.type() == TypeId::STRING)) return false;

        if (op == "=") return val < mn || val > mx;
        if (op == "<") return val <= mn;
        if (op == ">") return val >= mx;
        if (op == "<=") return val < mn;
        if (op == ">=") return val > mx;
        return false;
    }

    // `schema` is the scanning node's own output schema — the one the hint's
    // refs are looked up in. See collectSimplePredicates for why it is needed
    // and what it costs when a hint names a column this scan does not have.
    static bool shouldSkip(const Expr* where, const std::unordered_map<std::string, std::vector<ColumnChunk>>& zone_maps, int chunk_idx, const Schema& schema){
        if (!where) return false;
        std::vector<std::tuple<std::string, std::string, Value>> preds;
        collectSimplePredicates(where, schema, preds);


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