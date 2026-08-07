#pragma once

#include "parser/ast.h"
#include "common/schema.h"      // Row, Schema
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Week 31 — uncorrelated subquery execution, by materialization.
//
// WHY A CONSTANT AND NOT AN OPERATOR. "Uncorrelated" means the body references
// no relation of any enclosing block, so its value cannot depend on the outer
// row: it is loop-invariant, and this engine already has a pass whose entire
// purpose is turning a loop-invariant subtree into a Literal so that three fast
// paths can pattern-match on `ColumnRef op Literal` — see constant_folding.h,
// which measured 203ms against 0.35ms for exactly that shape. Running the body
// once and substituting the result gives zone-map pruning, scanColumn's tight
// loop and range selectivity back for `WHERE speed > (SELECT AVG(speed) ...)`.
// An expression-level SubPlan gives none of the three and arms all ten of the
// dispatch sites that handle SubqueryExpr but have never been reached.
//
// It also keeps Week 30's containment intact: after this pass the outer
// statement holds no SubqueryExpr at all, and an uncorrelated body is planned as
// its own TOP-LEVEL statement, where every ref is query_level 0 against that
// statement's own range table. No consumer anywhere sees two numbering domains.
// See development.md -> Relation slots and query levels.
//
// WHAT IT COSTS, so nobody has to discover it in review:
//   - --explain now executes the nested query (it must, to know the constant),
//     the same way it already performs constant folding;
//   - that time is charged to --explain-analyze's Plan: line, not Execution:;
//   - the nested query gets its own COPY of the tables it scans, because both
//     scan nodes take their table by value (Lowering's scan_uses already copies
//     for a self-join — same cost model);
//   - an IN set is materialized in full, and therefore capped (see below).

// What one nested query produced. `schema` is the sub-plan's output schema, and
// is what types an empty scalar result — see Literal::null_type.
struct SubqueryResult {
    Schema schema;
    std::vector<Row> rows;
};

// Plans and runs one already-bound, already-validated statement to completion.
//
// Injected rather than called directly for three reasons: the planner layer must
// not depend on the CLI's engine selection; the body must run on the SAME engine
// as the query containing it (handing a three-relation body to Volcano would
// refuse TPC-H Q11's subquery in vectorized mode); and an injected runner makes
// the whole rewrite unit-testable with canned rows and no data at all.
using SubqueryRunner = std::function<SubqueryResult(SelectStatement)>;

// Week 32 REMOVED MAX_MATERIALIZED_IN_VALUES. Week 31 capped an IN subquery's
// materialized set at 1024 distinct values because evaluate()'s InExpr case
// compares the list LINEARLY per row — `lap_id IN (SELECT lap_id FROM laps)` is
// 1e8 Value comparisons per Volcano mode — and documented the cap as a
// deliberate SQLite divergence whose removal was this week's business. It is
// gone because nothing is materialized any more, not because the constant was
// raised: materializeSubqueries now skips every Kind::IN node and
// subquery_lowering.h turns it into a hash semi-join, which is O(left + right).
// The README dialect-table row and the rejection-suite entry that pinned it went
// with it — the query moved into the DIFFED suite, where it is finally compared
// against SQLite rather than only asserted to fail.

// DISPATCH SITE 19 (development.md -> Extending the expression language).
//
// Visits every SubqueryExpr in an expression tree, INNERMOST FIRST, handing the
// callback the OWNING SLOT so the node can be replaced in place.
//
// This is the one walker that descends INTO the body, and that is deliberate.
// Week 30's rule for a query-bearing node — descend into the parts written in
// THIS block, never into the body — is a rule about SCOPE: the body has its own
// schema, range table and aggregate rule, so a scope question must not cross the
// boundary. "Which statements must run, and in what order" is not a scope
// question, and a nested body must be materialized before the body containing it
// can run. Only the expression tree is walked here; the body's own
// statement-level clauses are reached through materializeSubqueries.
//
// A missed Expr subtype is LOUD, not silent: the node survives into planning and
// inferExprType (site 12) throws, naming this walker. That is why sites 12 and
// 13 keep their throws.
void forEachSubquery(std::unique_ptr<Expr>& expr,
                     const std::function<void(std::unique_ptr<Expr>&)>& fn);

// The read-only shape of the same walk. Must stay in step with the mutating one
// above — two small functions rather than a const_cast.
void forEachSubqueryConst(const Expr* expr,
                          const std::function<void(const SubqueryExpr&)>& fn);

// Every table any query in this statement scans, nested ones included, deduped.
// main.cc loads and stats these: its pre-Week-31 walk over from_table + joins
// misses every nested table, and the failure was a raw std::out_of_range from
// table_rows.at() rather than anything a user could act on.
void collectQueryTables(const SelectStatement& stmt, std::vector<std::string>& out);

// Replaces every SubqueryExpr in `stmt` with a constant, innermost first, and
// clears has_subquery when none is left.
//
// PRECONDITIONS, all established by Validator::validate, which the caller runs
// FIRST (main.cc):
//   - no correlated subquery anywhere in `stmt` — refused in Week 33's name;
//   - a SCALAR or IN body has exactly one output column;
//   - a subquery appears only in WHERE / HAVING.
// The pass trusts them rather than re-checking: run it before validation and
// `WHERE speed > (SELECT speed, team FROM laps)` silently materializes column 0
// of a query the Validator was about to reject — a wrong answer instead of a
// diagnostic. The check belongs to the layer that owns the message.
void materializeSubqueries(SelectStatement& stmt, const SubqueryRunner& run);
