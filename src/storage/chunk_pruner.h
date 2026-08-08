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
    // one shared with the optimizer.
    //
    // !! `schema` IS THE SCHEMA THE HINT WAS WRITTEN AGAINST, NOT THE SCAN'S OWN.
    // Week 37. Through the previous round it was the scan's own output schema,
    // and the paragraph here claimed that made the screen conservative: "it
    // answers may raise for a conjunct this scan's schema cannot type — which is
    // every conjunct naming another relation". THAT SENTENCE WAS FALSE, and
    // three independent seam audits reached the same counter-example (engine
    // pass 5 E-20, storage pass 5 S-13, optimizer pass 5 P5-2). `staticTypeOf`'s
    // ColumnRef arm resolves slot-first WITH A BARE-NAME FALLBACK — the rule it
    // shares with evaluator.cc's resolveColumnIndex, where it is correct. Handed
    // the WRONG schema it does not answer "I cannot type this"; it answers "I
    // typed it", against the scanned relation's column of the same NAME. When
    // the two relations declare that name with different TYPES the screen types
    // the conjunct off a column it does not refer to, calls a raiser total, and
    // lets a later conjunct prune away the rows the raise was owed:
    //
    //   big(k INT, x INT, j INT) JOIN small2(j INT, x STRING, s STRING)
    //   SELECT COUNT(*) FROM big b JOIN small2 s2 ON b.j=s2.j
    //   WHERE s2.x AND b.k > 999999
    //     row storage       Error: std::get: wrong index for variant
    //     columnar storage  0            (chunks_skipped=3/3)
    //
    // with the RAISER WRITTEN FIRST, which is exactly the `j < k` case the proof
    // above calls unsound. A conservative screen may say "I do not know"; the one
    // answer it must never give is a confident wrong one, and a schema the
    // expression was not written against is what turns the first into the second.
    //
    // Giving this walker the hint's OWN schema removes the guess rather than
    // patching around it: `s2.x` then resolves at its own slot to the STRING
    // column, `conjunctMayRaise` answers TRUE, the walk stops, no chunk is
    // skipped, and every mode raises. Both scan nodes carry that schema beside
    // the hint (SeqScanNode, VecScanNode); the two builders that route a hint —
    // Planner::plan and VectorizedPlanBuilder's FILTER case — already hold it,
    // so `pruningHintForPreservedSide` (predicate_pushdown.h) needed no change:
    // it decides WHETHER a hint may descend, not what it means.
    //
    // THE SAME CHANGE RECOVERS PRUNING THAT THE PREVIOUS ROUND LOST, which is
    // why it is this fix and not the cheaper local guard (refuse to screen any
    // conjunct holding a ref that does not resolve at its own slot). With the
    // scan's schema, `dr.nationality = 'British'` written before `l.season = 2024`
    // could not be typed, the walk stopped, and the scan lost every skip; with
    // the join's schema it types as STRING vs STRING, cannot raise, and the walk
    // continues to the conjunct that prunes. Measured on Release, median of 11,
    // same query with the conjuncts swapped as the denominator: 2-chunk `laps`
    // +21% (col-volcano, BOTH optimizer settings — Planner::plan hands the raw
    // stmt.where to the FROM scan whether or not the optimizer ran, so this was
    // never a `--no-optimize` effect), and 2.80x on TPC-H sf0.01's 8-chunk
    // lineitem. One change closes a correctness divergence and a performance
    // regression, because both were the same missing argument.
    //
    // Do NOT "fix" this by deleting the bare-name fallback in staticTypeOf: it
    // is shared with evaluator.cc's resolution rule and is load-bearing wherever
    // a post-aggregate or projected schema no longer carries the original slot.
    // The defect was applying the screen in a schema the refs do not belong to.
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

    // `hint_schema` is the schema the HINT was written against — the child
    // schema of the filter the hint came from, which for an un-pushed WHERE over
    // a join is the JOIN's merged schema and not this scan's. It is used for the
    // raise SCREEN only; the zone-map lookup below is by column name and the
    // collection guard is by slot, so neither reads it. See
    // collectSimplePredicates for why passing the scan's own schema here was a
    // wrong answer rather than a conservative one.
    static bool shouldSkip(const Expr* where, const std::unordered_map<std::string, std::vector<ColumnChunk>>& zone_maps, int chunk_idx, const Schema& hint_schema){
        if (!where) return false;
        std::vector<std::tuple<std::string, std::string, Value>> preds;
        collectSimplePredicates(where, hint_schema, preds);


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