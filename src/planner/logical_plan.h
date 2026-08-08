
#pragma once

#include "common/schema.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
#include "planner/join_condition.h"   // JoinKey
#include <memory>
#include <string>
#include <vector>

enum class LogicalNodeType {
    SCAN, DERIVED, JOIN, FILTER, AGGREGATE, PROJECT, SORT, DISTINCT, LIMIT
};

// aggregate specification for a single aggregate function
struct AggregateSpec {
    std::string function;    // AVG, SUM, etc..
    std::string column;      // empty for star param
    bool is_star;
    // relation slot of the argument column (binder-assigned): distinguishes
    // join sides that share a column name, e.g. AVG(l2.speed) on a self-join.
    // -1 = unresolved/single-relation (resolve by bare name).
    //
    // Week 33. WAS a bare `int relation_slot` with NO query level, contained
    // only by Validator::validate refusing every statement with a subquery —
    // the containment Week 33 removes. It now carries the pair, like
    // GroupByColumn, so its two consumers (plan_nodes.cc,
    // vec_hash_aggregate_node.cc), which resolve indexOf(column, slot) against
    // a CHILD schema, cannot read a correlated slot as a local one.
    // See common/column_id.h and development.md's slot-consumer table.
    ColumnId id;
    // output-schema column name (aggregateOutputName of the bound expr);
    // empty only for hand-built specs in tests (falls back to FUNC(column))
    std::string output_name;
    // true = referenced only in HAVING/ORDER BY: computed but never projected
    bool hidden = false;
    // Week 34 — COUNT(DISTINCT x). The one aggregate in this engine whose
    // per-group state is not O(1): both HashAggregateNode and
    // VecHashAggregateNode keep a set of serialized values per group per spec.
    // Copied straight off AggregateExpr::distinct, and part of output_name via
    // aggregateOutputName, which is what keeps COUNT(x) and COUNT(DISTINCT x)
    // from deduping into one spec.
    bool distinct = false;
    // general argument expression (e.g. SUM(speed * (1 - x))). Non-owning:
    // points into the statement's AST, whose subtrees move into plan nodes of
    // the same tree — moving a unique_ptr never relocates the Expr (same
    // aliasing argument as the scan pruning hint in vectorized_plan_builder).
    // Execution uses `column` when set (plain ColumnRef fast path) and falls
    // back to evaluating `argument` per row otherwise. Last field so existing
    // positional brace-inits ({"COUNT", "", true}) stay valid.
    const Expr* argument = nullptr;
};

// execution independent plan node
// no table data and no execution state, just describes what to compute
// children[0] = FROM/left input, children[1] = JOIN input (join only)
struct LogicalPlanNode {
    LogicalNodeType type;
    Schema output_schema;
    // estimated output row count (Week 20 CardinalityEstimator);
    // -1 = not estimated (e.g. --no-optimize skips the pass)
    double estimated_rows = -1.0;
    std::vector<std::unique_ptr<LogicalPlanNode>> children;

    LogicalPlanNode(LogicalNodeType t, Schema schema) : type(t), output_schema(std::move(schema)) {}
    virtual ~LogicalPlanNode() = default;
    virtual std::string explain() const = 0;
};

// read one table
struct LogicalScan : LogicalPlanNode {
    std::string table_name;  // catalog name (binder normalized)

    LogicalScan(std::string table, Schema narrowed_schema) : LogicalPlanNode(LogicalNodeType::SCAN, std::move(narrowed_schema)), table_name(std::move(table)) {}
    std::string explain() const override;
};

// Week 34 — a relation of this block that is a PLAN rather than a table.
//
// WHY THIS IS A NODE AND NOT A BARE GRAFT, which is the smaller diff and the
// wrong one. Four walkers in this tree find "the relation" by descending
// children[0] until they hit a SCAN — leafScanTable and isSingleRelation
// (vectorized_plan_builder.cc), leafScanTableOf and countRelations
// (join_enumeration.cc) — and every one of them then draws a conclusion the cost
// model consumes. Walked THROUGH a derived subtree they return the BODY's base
// table, so the derived relation's column widths are attributed to whatever
// table its body happens to scan first, and countRelations inflates the range
// table's size so JoinEnumeration's out-of-range test stops meaning what it
// says. All four failures are silent. This node is the wall they stop at, which
// is Week 27's stance for a join-shaped input restated: refuse to guess rather
// than return a plausible wrong number.
//
// It also owns the alias for --explain, so a derived subtree is not an
// unexplained plan fragment at the bottom of the spine.
//
// !! ITS OUTPUT SCHEMA STAMPS SLOT 0, like a leaf scan's own schema. The outer
// slot is applied by the merged join schema. See derivedRelationSchema.
struct LogicalDerived : LogicalPlanNode {
    std::string alias;   // the enclosing block's ref name for this relation

    // Week 37, seam audit pass 3 B3-3. Set by PredicatePushdown when a conjunct
    // routed to this relation was REFUSED entry to the body, and never
    // otherwise — same discipline as LogicalJoin::order_decision, so a body that
    // took every conjunct (and every --no-optimize plan, and every hand-built
    // test tree) keeps a byte-identical explain string.
    //
    // It exists because the refusal is the one thing --explain could not show.
    // A filter drawn above a LogicalDerived looks identical whether there was
    // nothing to push or whether there was and the pass declined, and B3-3 was
    // the third silent decline this phase found. Set AFTER construction, like
    // order_decision, so the three-argument constructor is unchanged.
    std::string pushdown_decision;

    LogicalDerived(std::unique_ptr<LogicalPlanNode> body, std::string alias, Schema schema)
        : LogicalPlanNode(LogicalNodeType::DERIVED, std::move(schema)),
          alias(std::move(alias)) {
        children.push_back(std::move(body));
    }
    std::string explain() const override;
};

// Week 32 — set-membership lowering. Kept OFF JoinType (parser/ast.h) on
// purpose: JoinType is the *syntactic* per-clause kind a user writes on a JOIN
// clause, and SEMI/ANTI are never written — they are lowering artifacts. Adding
// them there would make `stmt.joins[i].join_type == SEMI` representable and
// unreachable, and grow a dead case in every parser and validator switch.
//
// Week 33 SPLIT ANTI IN TWO, and the split is the point: the two negations do
// NOT agree on NULL, and one enumerator carrying both is how NOT IN's rule was
// silently applied to NOT EXISTS. Keep the textbook meaning on the plain name so
// that a reader who writes ANTI gets relational algebra, and make the special
// case wear the special name.
enum class JoinSemantics {
    STANDARD,  // ordinary equi-join; join_type says INNER or LEFT
    SEMI,      // emit each children[0] row AT MOST ONCE, when a match exists
    // The textbook anti-join, and exactly NOT EXISTS. TWO-VALUED: EXISTS is a
    // pure existence test that is never UNKNOWN, so a NULL key on either side
    // simply fails to match and the children[0] row SURVIVES.
    ANTI,
    // ANTI plus NOT IN's THREE-VALUED rule (Week 32), which is not a property of
    // anti-join at all but of the predicate lowered to it: `x NOT IN S` is never
    // TRUE when S holds a NULL, and `NULL NOT IN S` is UNKNOWN unless S is
    // empty. Both facts are unrepresentable in a match/no-match probe, so they
    // are carried here and applied in VecHashJoinNode's probe loop. Produced by
    // subquery_lowering.cc ONLY. Anything else that reaches for a negated join
    // wants ANTI.
    ANTI_NOT_IN
};

// equi-join, INNER by default. keys[k].from_col resolves against children[0]'s schema,
// keys[k].join_col against children[1]'s. Multi-key since Week 26 (TPC-H Q9).
// join_slot is the binder relation slot of children[1] — it stamps the merged
// schema and is what predicate pushdown routes conjuncts by. Left-deep only:
// children[1] is always a single relation (join enumeration, Week 28, keeps
// that shape).
struct LogicalJoin : LogicalPlanNode {
    std::vector<JoinKey> keys;
    // Binder relation slot of children[1]. Week 32: -1 when semantics !=
    // STANDARD, and -1 there means "children[1] is not a relation of this
    // block's range table" — there is no slot that names a subquery body. Every
    // reader of join_slot must therefore either decline on semantics !=
    // STANDARD or be provably unreachable for such a node (PredicatePushdown
    // declines; JoinEnumeration declines; the lowerings read it only in the
    // STANDARD branch). That is a development.md slot-table row, not a comment.
    int join_slot;
    // Week 28: set by JoinEnumeration on the TOP join of an enumerated tree, and
    // nowhere else. Empty for every single-join plan, every --no-optimize plan
    // and every hand-built test tree, so all pre-existing explain strings stay
    // byte-identical. Same discipline as VecHashJoinNode::cost_decision_ — and
    // the same rule: never print it when estimates did not drive the decision,
    // or --explain claims an optimizer choice that never happened.
    std::string order_decision;

    // Week 29 — LEFT OUTER. Keys, merged schema and slot stamping are identical;
    // what changes is that an unmatched children[0] row is emitted with NULLs
    // across children[1]'s block, and that four passes must now ask which kind
    // of join this is (pushdown, enumeration, estimation, lowering). Set AFTER
    // construction, like order_decision, so the five-argument constructor — and
    // every hand-built test tree that calls it — is unchanged.
    JoinType join_type = JoinType::INNER;

    // Non-key ON conjuncts, conjoined. NEVER SET ON AN INNER JOIN: an inner
    // join's residuals are folded into the WHERE conjunction instead (Week 27),
    // because ON and WHERE are interchangeable there and the fold buys pushdown.
    // For an outer join they are part of the MATCH TEST — a left row whose every
    // candidate fails this predicate is null-extended, not deleted. MOVED into
    // the physical operator at lowering (like LogicalFilter::predicate).
    //
    // WEEK 36 — TWO PRODUCERS NOW, NOT ONE, and they resolve in DIFFERENT
    // SCHEMAS. logical_plan.cc sets this on a JoinType::LEFT node, where it
    // resolves against this node's merged output_schema. subquery_decorrelation.cc
    // sets it on a SEMI/ANTI node (TPC-H q21's `l2.l_suppkey != l1.l_suppkey`),
    // where output_schema IS the probe schema and the residual also names the
    // body's projected columns — so it resolves against joinResidualSchema()
    // below, which is probe (+) build. Every reader must ask that function rather
    // than reach for output_schema; the sentence "resolves against THIS node's
    // merged output_schema" stood here and is now true of only one producer.
    //
    // Why a semi join's residual cannot be folded into the WHERE the way an
    // inner join's is: `R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S)` is the identity the fold
    // rests on, and it FAILS for a semi join — the semi join has already
    // collapsed the matching build rows to a yes/no answer, so by the time `q`
    // could apply, the row that would satisfy it is gone. It must be evaluated
    // INSIDE the probe, against a probe(+)build pair.
    //
    // ANTI_NOT_IN NEVER carries one, and that is a containment rather than an
    // omission: its build_had_unmatchable_key_ short-circuit answers "S contains
    // a NULL, so `x NOT IN S` is never TRUE" — a claim about the KEY column that
    // a residual makes untrue, because a build row with a NULL key can no longer
    // stand for "some row matched". `NOT IN` produces no residual, so the
    // constraint costs nothing; VecHashJoinNode's constructor enforces it.
    //
    // !! IT IS EVALUATED PER CANDIDATE PAIR, NOT PER OUTPUT ROW, and that makes
    // it the one per-row expression in the join layer that is not a conjunct of
    // any list the totality screen reads (seam audit pass 5, P5-B1). Both probe
    // loops run it on every preserved-side row against every build-side match
    // (plan_nodes.cc's HashJoinNode::next, vec_hash_join_node.cc — both
    // `passes()`), so ANY rewrite that removes a preserved-side row removes
    // evaluations of this expression. PredicatePushdown::distribute therefore
    // screens it with conjunctMayRaise before it will push a WHERE conjunct into
    // children[0]; the INNER path needs no such screen because its residuals
    // become ordinary WHERE conjuncts (below) and the screen sees them in the
    // list. Anything else added that shrinks a LEFT join's preserved input owes
    // the same screen.
    //
    // WEEK 36: "a LEFT join's preserved input" is now "a LEFT join's preserved
    // input OR A SEMI/ANTI JOIN'S PROBE INPUT". The argument transfers verbatim
    // — distribute pushes into children[0] of a semi/anti join exactly as it does
    // for a LEFT one, and every probe row it removes removes that row's residual
    // evaluations — so the screen itself is UNCHANGED except for the schema it is
    // handed (joinResidualSchema, not output_schema). Handing it output_schema
    // for a semi/anti node would be worse than useless: the body-side refs are
    // named `$rN`, which resolves nowhere in the probe schema, so the screen
    // would answer "may raise" for every q21-shaped query and quietly cost
    // pushdown on all of them.
    //
    // A SEPARATE, STILL-OPEN PROPERTY (pass 5, P5-M1): there is no conjunct
    // CASCADE inside this expression. It is one Expr handed to evaluate(), which
    // computes both operands of an AND before looking at the operator, so
    // `ON k = k AND A AND B` evaluates B on rows where A is FALSE — which the
    // same text in a WHERE, or on an INNER join, does not. All four legs agree,
    // so it is not a divergence; it is expr_totality.h's central sentence being
    // false for a construct the parser accepts. Not fixed here.
    std::unique_ptr<Expr> on_residual;

    // Week 32 — set-membership lowering (subquery_lowering.h). Set AFTER
    // construction, like order_decision and join_type, so the five-argument
    // constructor and every hand-built test tree are byte-identical.
    //
    // !! INVARIANT. Week 32 called this "the whole containment for the
    // two-range-table problem this node introduces". Week 34 made it ONE OF TWO,
    // and a reader must know both or they will draw the wrong conclusion from
    // either. The two are not alternatives; they cover different constructs:
    //
    //   (a) THIS one, for a semi/anti join: nothing from children[1] is ever in
    //       scope above the node, so the body's slot numbering never meets the
    //       outer one. Still exactly true, unchanged, and enforced below.
    //   (b) NORMALIZATION, for a derived relation (LogicalDerived, Week 34),
    //       whose columns ARE in scope above it and therefore cannot be kept
    //       out. derivedRelationSchema stamps every one of them slot 0 — like a
    //       leaf scan's own schema — and the merged join schema applies the outer
    //       slot, so what enters the outer plan carries OUTER numbering only.
    //
    // (a) keeps two domains apart; (b) converts one into the other. A STANDARD
    // join over a LogicalDerived (which Week 34's correlated-scalar rewrite
    // builds) relies on (b) and would be a wrong answer under (a) alone.
    //
    // The rest of this comment is (a), and is unchanged
    // (development.md -> Relation slots and query levels):
    // when semantics != STANDARD, output_schema IS children[0]->output_schema,
    // NOT a merged schema. children[1] is the plan of a subquery BODY, whose
    // relation slots are numbered against the BODY's range table — a second
    // numbering domain at the same query level. Nothing from children[1] is ever
    // in scope above this node, so the two domains never meet. A merged schema
    // here would put a body column at a body slot into scope, and every
    // indexOf(name, slot) above would be a coin flip — the silent
    // wrong-relation class buildAggregateSchema's tripwire exists for.
    //
    // join_type stays INNER for both: an outer semi-join is not a shape this
    // engine can produce, and the two fields are read independently. join_slot
    // is -1 — see below.
    JoinSemantics semantics = JoinSemantics::STANDARD;

    LogicalJoin(std::unique_ptr<LogicalPlanNode> from_child, std::unique_ptr<LogicalPlanNode> join_child, std::vector<JoinKey> keys, int join_slot, Schema merged) : LogicalPlanNode(LogicalNodeType::JOIN, std::move(merged)), keys(std::move(keys)), join_slot(join_slot) {
        children.push_back(std::move(from_child));
        children.push_back(std::move(join_child));
    }
    std::string explain() const override;
};

// Week 36 — THE RELATION SLOT EVERY BUILD-SIDE COLUMN CARRIES INSIDE A SEMI/ANTI
// JOIN'S RESIDUAL SCHEMA, and the whole reason q21's residual resolves to the
// right column.
//
// q21's residual is `l3.l_suppkey != l1.l_suppkey`: the SAME COLUMN NAME, from
// two aliases of the same table, one on each side of the join. In a concatenated
// probe(+)build schema `indexOf(name)` takes the FIRST match — which is always
// the probe half, because the probe half is first — so a body-side ref left to
// resolve by name reads the PROBE's column and the residual becomes `x != x`.
// Wrong rows, no error, an identical --explain: the H-1 failure shape verbatim.
//
// So the two halves are separated by SLOT, not by name. The probe half keeps the
// outer block's own slots (the residual's level-1 refs are stamped
// `id.outward()`, exactly as splitCorrelation stamps JoinKey::from_slot). The
// build half is re-stamped to THIS value, and the residual's body-side refs are
// stamped to match — so `indexOf(name, slot)` is exact on both sides and neither
// can reach the other.
//
// Out of band on purpose: a relation slot is a range-table position, so every
// real one is small and non-negative and no probe column can collide with this.
// It is never stored in any node's `output_schema` — joinResidualSchema builds a
// PRIVATE schema for the residual, and nothing else reads it.
//
// Belt AND braces: decorrelation also RENAMES the appended body columns to
// `$rN`, and `$` is not lexable in an identifier, so even the bare-name fallback
// in resolveColumnIndex / staticTypeOf cannot cross the seam. The slot is the
// guarantee; the name is what makes a wrong one visible in --explain.
constexpr int kResidualBuildSlot = 1 << 20;

// The schema `LogicalJoin::on_residual` resolves in, which is NOT always the
// node's output schema.
//
//   STANDARD (incl. LEFT)  the merged output schema, exactly as Week 29 had it.
//   SEMI / ANTI            probe (+) build — because output_schema IS the probe
//                          schema (the Week 32 containment, which does NOT move)
//                          and the residual also names the body's projected
//                          columns.
//
// ONE function, three consumers that must agree: VecHashJoinNode (which
// evaluates the residual), PredicatePushdown::distribute (which screens it with
// conjunctMayRaise before it will shrink the probe side) and collectIntOrigins's
// taint walk. A second derivation of this schema is the drift that would make a
// screen answer about a different expression than the one that runs.
Schema joinResidualSchema(const Schema& probe, const Schema& build);
Schema joinResidualSchema(const LogicalJoin& join);

// evaluate a predicate (bool expr) and discard non matching rows
// serves both WHERE and HAVING; position encodes which (below aggregate = WHERE)
struct LogicalFilter : LogicalPlanNode {
    std::unique_ptr<Expr> predicate;

    LogicalFilter(std::unique_ptr<LogicalPlanNode> child, std::unique_ptr<Expr> pred) : LogicalPlanNode(LogicalNodeType::FILTER, child->output_schema), predicate(std::move(pred)) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// group rows by specified columns and compute aggregates
struct LogicalAggregate : LogicalPlanNode {
    std::vector<GroupByColumn> group_by;  // empty for global aggregates
    std::vector<AggregateSpec> aggregates;

    LogicalAggregate(std::unique_ptr<LogicalPlanNode> child, std::vector<GroupByColumn> group_by, std::vector<AggregateSpec> aggregates, Schema output_schema) : LogicalPlanNode(LogicalNodeType::AGGREGATE, std::move(output_schema)), group_by(std::move(group_by)), aggregates(std::move(aggregates)) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// evaluate the SELECT list
struct LogicalProject : LogicalPlanNode {
    std::vector<std::unique_ptr<Expr>> exprs;  // SELECT list, or synthesized ColumnRefs for SELECT *

    LogicalProject(std::unique_ptr<LogicalPlanNode> child, std::vector<std::unique_ptr<Expr>> exprs, Schema output_schema) : LogicalPlanNode(LogicalNodeType::PROJECT, std::move(output_schema)), exprs(std::move(exprs)) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// sort rows by specified expressions
struct LogicalSort : LogicalPlanNode {
    std::vector<OrderByItem> order_by;  // owns the sort exprs
    // > 0: only the row_cap smallest rows are needed, because the parent is a
    // LIMIT. Set by deterministicCut (logical_plan.cc) on the sort it inserts,
    // and lowered to VecSortNode's bounded top-N. Left 0 on a user-written
    // ORDER BY, whose sort predates this and is unchanged by it.
    int row_cap = 0;

    LogicalSort(std::unique_ptr<LogicalPlanNode> child, std::vector<OrderByItem> order_by) : LogicalPlanNode(LogicalNodeType::SORT, child->output_schema), order_by(std::move(order_by)) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// remove duplicate rows
struct LogicalDistinct : LogicalPlanNode {
    LogicalDistinct(std::unique_ptr<LogicalPlanNode> child) : LogicalPlanNode(LogicalNodeType::DISTINCT, child->output_schema) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// limit the number of output rows
struct LogicalLimit : LogicalPlanNode {
    int limit;

    LogicalLimit(std::unique_ptr<LogicalPlanNode> child, int limit) : LogicalPlanNode(LogicalNodeType::LIMIT, child->output_schema), limit(limit) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// SEAM AUDIT PASS 5, P5-1. The screen the two SET-MEMBERSHIP lowerings need,
// and the third distinct SHAPE the one rule of parser/expr_totality.h takes.
//
// The rule: a conjunct of a filter is evaluated on the rows for which every
// conjunct WRITTEN BEFORE IT evaluated TRUE, and nothing may change that set for
// an expression that can raise. `firstMayRaise` states it for a conjunct list
// that STAYS a conjunct list. lowerInSubqueries and lowerExistsSubqueries do
// something that list has no vocabulary for: they DELETE their conjunct and
// interpose a row-REDUCING semi/anti join BELOW the LogicalFilter the survivors
// end up in. Every conjunct written EARLIER than the deleted one is then
// evaluated on the join's survivors instead of on the spine's rows. Measured on
// the shipped catalog before this existed:
//
//   WHERE l.lap_id * 9223372036854775807 > 0 AND l.driver_id > 999999
//     -> Error: integer overflow in '*'          (the definition's answer)
//   WHERE l.lap_id * 9223372036854775807 > 0 AND l.driver_id IN (999999)
//     -> Error: integer overflow in '*'          (a constant IN stays a conjunct)
//   WHERE l.lap_id * 9223372036854775807 > 0
//         AND l.driver_id IN (SELECT d.driver_id FROM drivers d WHERE d.age > 999)
//     -> 0 rows                                  (the semi-join moved underneath)
//
// Same rows, same predicates, opposite error behaviour, decided by which
// spelling of the second predicate was written. Both legs agree — the lowerings
// run inside LogicalPlanBuilder::build, ahead of the --no-optimize gate — so no
// harness can see it.
//
// THE REMEDY IS THE RULE ITSELF, not a refusal: put the earlier conjuncts BELOW
// the join, which is exactly where the written order says they are evaluated.
// The result set is unchanged (conjunction), the answer is never lost, and the
// prefix keeps its own pushdown because PredicatePushdown reaches a
// FILTER-over-JOIN wherever it sits.
//
// Called once per extraction, with the conjuncts the lowering has kept so far.
// Returns `spine` and `prefix` UNTOUCHED — no node added, no plan shape changed
// — unless some prefix conjunct can raise, so an ordinary IN/EXISTS query plans
// byte-identically to before.
//
// ONE BOUNDED DECLINE, stated rather than silent: a prefix conjunct that still
// holds a SubqueryExpr is left where it is. It has to be — refuseUnloweredIn /
// refuseUnloweredCorrelated and lowerCorrelatedScalars all run on what stays in
// the caller's vector, and a conjunct moved out of it reaches none of them. That
// leaves `raiser AND EXISTS(...) AND IN(...)` (a raiser with TWO lowerable
// conjuncts after it, the first of which is not yet consumed) still masked, and
// it is recorded as open rather than half-fixed.
std::unique_ptr<LogicalPlanNode> guardLoweredConjunctPrefix(
        std::unique_ptr<LogicalPlanNode> spine,
        std::vector<std::unique_ptr<Expr>>& prefix);

// logical schema helpers — shared by LogicalPlanBuilder and the Volcano
// Planner (relocated from Planner in Week 18 so the logical layer no longer
// includes physical operator headers)

// result type of an expression against a schema; throws std::runtime_error on
// ill-typed trees (arithmetic over STRING). Mirrors evaluate()'s dispatch:
// same slot-first column resolution, same INT-as-boolean convention.
TypeId inferExprType(const Expr* expr, const Schema& schema);

// Result type of an aggregate, as a function of the function AND its argument
// type. COUNT is always INT. MIN/MAX are order statistics — they return an
// element of the input domain, so they preserve the argument type (including
// STRING; typing them DOUBLE made MIN(team) throw bad_variant_access at the
// materialization point). SUM/AVG accumulate in double, a deliberate divergence
// from SQLite's INT-preserving SUM: the harness compares numerically, and one
// accumulator type keeps the aggregate nodes simple.
// `arg_type` is ignored for COUNT and for star aggregates.
TypeId aggregateResultType(const std::string& function, TypeId arg_type);

// narrowed scan schema for one table: only columns the query references;
// returns full_schema unchanged for SELECT *
Schema buildScanSchema(const SelectStatement& stmt, const Schema& full_schema);

// schema of the project node's output based on the select list
Schema buildProjectSchema(const SelectStatement& stmt, const Schema& table_schema);

// Week 34 — the output schema of a whole query BLOCK, without building its plan.
//
// The Binder needs a derived table's schema to put in its range-table entry, and
// it needs it BEFORE LogicalPlanBuilder::build has run (build calls
// Validator::validate, which resolves against the range table this schema is
// for). Writing a second, private derivation in the Binder is the two-paths
// drift Weeks 26/28/30 each had to undo, and it would be worse than usual here:
// the two copies would have to agree on aggregateOutputName (which IS
// exprToString, a byte-for-byte contract), on `hidden` columns and on SELECT *
// expansion, and a disagreement is a silent wrong-relation lookup rather than an
// error.
//
// So: one function, composed of the SAME helpers build() uses
// (buildScanSchema / extractAggregates / buildAggregateSchema /
// buildProjectSchema), and LogicalPlanBuilder::build ASSERTS at the graft that
// the plan it actually produced has this schema. That assertion compares two
// objects computed by different code paths, so it can genuinely fail — unlike
// the dead one Week 33 deleted, which compared a copy of an object with the
// object.
//
// Precondition: `stmt` is bound. Nested derived tables recurse.
Schema blockOutputSchema(const SelectStatement& stmt, const Catalog& catalog);

// Week 34 — turn a derived table's BODY schema into the schema of a RELATION of
// the enclosing block. Two operations and one refusal, all of which must happen
// identically wherever a derived relation's schema is computed, which is why
// they are here and not open-coded at the Binder and the plan builder:
//
//  - the column-alias list (`AS d (a, b)`) RENAMES positionally; a length
//    mismatch is an error;
//  - every column is stamped relation_slot 0, exactly as a leaf scan's own
//    schema is. THE OUTER SLOT IS NOT APPLIED HERE: it is applied by the merged
//    join schema, by the same loop that stamps a base relation. A body that
//    JOINS produces slots 0 AND 1 — the BODY's numbering — and grafting that
//    unchanged would put two numbering domains inside one schema, which is the
//    silent wrong-relation class ColumnId makes loud inside a block and which
//    nothing checks across a range-table boundary. Keeping it 0 is also what
//    PredicatePushdown's `restampSlots(c, 0)` before a push and ChunkPruner's
//    `relation_slot < 1` scan-local test both already assume of a leaf.
//  - two output columns of one name are REFUSED. A catalog table cannot have
//    them; a derived table can, and then BOTH indexOf overloads are a coin flip.
Schema derivedRelationSchema(Schema body_schema, const TableRef& ref);

// schema of the aggregate node's output: group-by columns, then one column per aggregate
Schema buildAggregateSchema(const std::vector<GroupByColumn>& group_by,
                            const std::vector<AggregateSpec>& aggregates,
                            const Schema& table_schema);

// extract aggregate specifications from the select list
std::vector<AggregateSpec> extractAggregates(const SelectStatement& stmt);

// Rewrite post-aggregate clauses (select list, HAVING, ORDER BY) so any
// subtree textually matching an expression GROUP BY key becomes a ColumnRef
// to the aggregate's group-key output column (named by exprToString). Runs
// AFTER Validator::validate — validation checks the original trees against
// base-table schemas; the synthesized refs resolve only post-aggregate.
// No-op unless expression group keys exist. WHERE is never rewritten (it
// runs pre-aggregate), and AggregateExpr arguments are left intact.
void substituteGroupKeyRefs(SelectStatement& stmt);

// validates the statement, then builds an execution-independent logical plan tree
// moves expressions out of stmt (select list, predicates, order by) rather than copying
// precondition: Binder::bind has run on stmt; relation slots drive join-key routing,
// falling back to positional routing when unbound, matching Planner::plan
class LogicalPlanBuilder {
    public:
        static std::unique_ptr<LogicalPlanNode> build(SelectStatement stmt, const Catalog& catalog);
};