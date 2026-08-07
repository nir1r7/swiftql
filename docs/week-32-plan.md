# Week 32 — Semi-Joins + Anti-Joins (teaching plan)

**Checkpoint (README):** *Set-membership subqueries avoid nested-loop execution.*

**README bullets:**
- Add vectorized semi-join and anti-join operators
- Lower `IN`, `NOT IN`, `EXISTS`, and `NOT EXISTS` where applicable

**Inherited business, from the starting notes addressed to this week:**
- Remove the `IN (subquery)` 1024-distinct-value cap (Week 31 dialect-table divergence) **if** semi-join lowering makes it unnecessary.
- Semi/anti-joins need their **own** cardinality rule; a non-multiplicative rule lives at the *stamping* site (`CardinalityEstimator`), never inside `joinCardinality` (Week 29's discipline).
- Both Week 30 tripwires (`ChunkPruner` declines `query_level > 0`; `buildAggregateSchema` throws) stay armed — understand why they differ before touching either.
- `development.md` → *Relation slots and query levels* is a map to **verify**, not to trust (wrong by omission twice).
- The diffed oracle suite cannot hold a query that errors; changed refusals are asserted in the rejection suite in `python_tools/compare_against_sqlite.py`.

---

## Does Week 32 lower a correlated reference?

**No.** *(Reasoning in §0 below.)*

---

## Task list

0. Prerequisite: settle the `ColumnId { level, slot }` trigger, and the *real* structural hazard this week does hit
1. `LogicalJoin` gains SEMI / ANTI join kinds (logical layer + schema rule)
2. The lowering decision: which subquery shapes go down the semi-join path vs. Week 31 materialization
3. Rewrite pass — `SubqueryExpr` → `LogicalJoin{SEMI|ANTI}` (the graft, and the slot-domain problem it creates)
4. Cardinality + cost: the semi/anti rule, stamped not searched
5. Physical planning: `PhysicalJoin` kind plumb-through, `JoinEnumeration` decline
6. Vectorized semi-join / anti-join execution (`VecHashJoinNode`)
7. Volcano `HashJoinNode` — semi/anti, or a stated refusal
8. NULL semantics: `NOT IN` is not an anti-join
9. Remove the 1024-distinct-value cap
10. Tests + oracle + rejection suite + verification gate

---

## 0. Prerequisite — the `ColumnId { level, slot }` trigger, and the hazard this week *does* hit

### Why it matters

Week 30's starting notes bind the `ColumnId { level, slot }` refactor to a trigger, not a date:
it is done as its own standalone change in whichever of Weeks 32/34 **first lowers a correlated
reference**, never folded into a feature week. Measured cost: 87 non-comment mentions of
`relation_slot` / `from_slot` across six source layers, plus every test that hand-builds a
`ColumnRef` / `GroupByColumn` / `AggregateSpec`. Getting the trigger answer wrong in either
direction is expensive: firing it needlessly burns the week; missing it re-opens the silent
wrong-relation class that was found five times in one week across three audit rounds.

### The answer: Week 32 does NOT lower a correlated reference

Two independent facts, either of which is sufficient:

1. **The refusal is still in place.** `Validator::validate`'s last check tests
   `has_correlated_subquery` and refuses with `correlated subqueries are not yet executable
   (Week 33)`. Week 32's checkpoint does not ask for that refusal to move, and the README puts
   decorrelation in Week 33 by name. A `ColumnRef` with `query_level > 0` exists **only** inside
   a correlated subquery (development.md states this as the containment), so none can reach a
   plan node this week.
2. **The shapes Week 32 lowers are uncorrelated by definition.** An uncorrelated `IN` body
   references no relation of any enclosing block; every `ColumnRef` in it is `query_level 0`
   against the body's own range table. The `IN` operand — the left-hand side — belongs to the
   *enclosing* query and is likewise level 0 there (`SubqueryExpr::operand`, bound in the outer
   scope, and `restampSlots` already restamps it).

So: **the `ColumnId` structural change stays deferred, and its trigger now points at Week 33.**
Record that in `development.md` in the same words, so Week 33 does not have to re-derive it.
Do **not** open the refactor this week.

### The hazard Week 32 *does* introduce — and it is not the level problem

Semi-join lowering does something Week 31 deliberately never did: it **grafts the subquery
body's plan subtree into the outer plan tree**. Week 31 avoided the whole question by planning
an uncorrelated body as its own *top-level statement* — "there is never more than one range
table in play for one plan" (development.md). A semi-join breaks that sentence. One plan now
holds nodes built from two range tables. Both are level 0; the collision is not between levels,
it is between two **slot numbering domains at the same level**.

Week 31's own starting note predicted this would not happen until Week 34 —
*"`JoinEnumeration`'s decline for a slot outside the range table did not become live … Week 34's
derived tables are where a nested scan genuinely joins the outer one."* **That prediction is
wrong for the semi-join path.** Verify it rather than inherit it; this is the second kind of
thing `development.md`'s slot table has been wrong about by omission.

**The containment that makes it tractable — and it is a strong one:** a semi-join and an
anti-join emit **only left-side columns**. Nothing from the body's schema appears above the
join. So:

- No expression above the semi-join can name a body slot, because no body column is in scope.
- `LogicalJoin::output_schema` for a SEMI/ANTI node is `children[0]->output_schema` **unchanged**
  — not a merged schema — so the merged-schema slot stamping in `Planner::plan` and
  `VectorizedPlanBuilder` never runs for it, and there is no second numbering domain to stamp.
- The only place a body slot is read is `JoinKey::join_col` resolved against `children[1]`'s own
  schema, which is exactly the domain it came from.

Make that an explicit invariant and assert it, rather than leaving it as an observation:
**a SEMI/ANTI `LogicalJoin`'s `output_schema` must compare equal to `children[0]->output_schema`.**
That single assertion is what keeps the two domains from meeting.

### Prerequisite knowledge

- `development.md` → *Relation slots and query levels* — read both halves, and treat it as a map
  to verify. It has been wrong by omission twice. Add rows for every consumer this week touches:
  the SEMI/ANTI branch of `CardinalityEstimator`, the `VectorizedPlanBuilder` lowering, the new
  lowering pass, and `JoinEnumeration`'s decline.
- README → Week 29 starting notes, for the non-multiplicative-rule discipline (§4 below).
- The `invariants` skill, before touching `src/planner/` or `src/execution/`.

### The two Week 30 tripwires — why they differ, and whether Week 32 arms either

- `collectSimplePredicates` / `ChunkPruner::shouldSkip` (`src/storage/chunk_pruner.h`)
  **declines** a `query_level > 0` ref. Chunk pruning is an **optimization**: contributing
  nothing is correct-and-slower, so the safe failure is silence.
- `buildAggregateSchema` (`src/planner/logical_plan.cc`) **throws** on one. Grouping is
  **semantics**, and the failure mode is not a miss — `indexOf("team", 0)` against the wrong
  child schema is a clean **hit on the wrong relation**, so neither the bare-name fallback nor
  the `idx < 0` throw fires and the query silently groups by the wrong column. There is no
  correct local fallback, so it must be loud.

**Week 32 arms neither**, for the §0 reason: no correlated ref is lowered. Do not replace either
tripwire with "real behaviour" — that is Week 33's. But there is one thing to *check* rather
than assume: an `IN` body may itself contain a `GROUP BY`
(`x IN (SELECT k FROM t GROUP BY k)`), and that body's plan is now built by a nested
`LogicalPlanBuilder` call **inside** the outer build rather than as a separate top-level plan.
Its `GroupByColumn`s are level 0 against the body's range table, so `buildAggregateSchema`'s
guard still does not fire — confirm this with a test rather than by reading, because the call
context changed even though the data did not.

---

## 1. `LogicalJoin` gains SEMI / ANTI semantics

### Why it matters

Every downstream pass asks a `LogicalJoin` what kind it is: predicate pushdown (may I cross
it?), join enumeration (may I reorder it?), cardinality estimation (what rule?), and both
lowerings (which operator?). Week 29 already paid for this fan-out once for `LEFT`, and the
README records it: *"four passes must now ask which kind of join this is."* Adding a third and
fourth kind touches the same four passes plus a fifth — the schema rule, which `LEFT` did not
change and SEMI/ANTI does.

### Conceptual explanation

Relational algebra: `R ⋉ S` (semi-join) is `π_R(R ⋈ S)` **with duplicates from S collapsed** —
each R row appears at most once, and the output schema is R's. `R ▷ S` (anti-join) is
`R − (R ⋉ S)`. Neither is an inner join with a projection on top, because the projection would
not collapse duplicates: an inner join against a body with three matching rows emits the left
row three times.

**Where the kind lives.** `JoinType` (`src/parser/ast.h`: `enum class JoinType { INNER, LEFT }`)
is a **parser** enum — a per-clause syntactic join type carried on `JoinClause` and copied onto
`LogicalJoin::join_type`. SEMI and ANTI are never written by a user; they are lowering artifacts.
Putting them in `JoinType` makes `stmt.joins[i].join_type == SEMI` representable and
unreachable, and grows a dead case in every parser and validator switch over the enum.

**Recommendation:** a separate field on `LogicalJoin`, orthogonal to `join_type`, set *after*
construction — the same discipline `order_decision` and `join_type` already use, which is what
keeps the five-argument constructor and every hand-built test tree byte-identical.

### Code snippet (illustrative — `src/planner/logical_plan.h`)

```cpp
// Week 32 — set-membership lowering. Kept OFF JoinType (parser/ast.h) on
// purpose: JoinType is the *syntactic* per-clause kind, and SEMI/ANTI are
// never written by a user, so adding them there makes an unreachable state
// representable in the AST and grows a dead case in every parser switch.
enum class JoinSemantics {
    STANDARD,  // ordinary equi-join; join_type says INNER or LEFT
    SEMI,      // emit each children[0] row AT MOST ONCE, when a match exists
    ANTI       // emit each children[0] row when NO match exists
};

struct LogicalJoin : LogicalPlanNode {
    // ... keys, join_slot, order_decision, join_type, on_residual as today ...

    // Week 32. Set AFTER construction, like order_decision and join_type, so the
    // five-argument constructor and every hand-built test tree are unchanged.
    //
    // !! INVARIANT, and the whole containment for the two-range-table problem
    // this node introduces (development.md -> Relation slots and query levels):
    // when semantics != STANDARD, output_schema IS children[0]->output_schema,
    // NOT a merged schema. Nothing from children[1] is ever in scope above this
    // node, so the body's slot numbering domain never meets the outer one.
    // join_type stays INNER for both: an outer semi-join is not a shape this
    // engine can produce, and the two fields are read independently.
    JoinSemantics semantics = JoinSemantics::STANDARD;
};
```

### Implementation guidance

1. Add the enum and the field. Do **not** touch the constructor.
2. `LogicalJoin::explain()` (`logical_plan.cc`) — print `SemiJoin` / `AntiJoin` instead of
   `HashJoin`. Mind the existing discipline: `order_decision` is printed only when enumeration
   actually ran, so do not print an ordering annotation on a node enumeration declined.
3. Find every `switch`/`if` on `join_type == JoinType::LEFT` and decide, per site, whether
   `semantics != STANDARD` belongs alongside it. The Week 29 sites are the map:
   `PredicatePushdown` (must not cross to the right side — same as `LEFT`, and for a *stronger*
   reason: the right side's columns are not in the output schema at all, so a pushed conjunct
   naming one is unresolvable, not merely wrong), `JoinEnumeration` (decline — §5),
   `CardinalityEstimator` (§4), `VectorizedPlanBuilder` and `Planner::plan` (§5–§7).
4. The `output_schema == children[0]->output_schema` invariant: assert it at construction time
   of the lowering pass, and add it to the `invariants` skill's list. This is the assertion that
   replaces an audit round.

### Anticipated mistakes specific to this codebase

- **Building a merged schema out of habit.** Every existing `LogicalJoin` call site builds one.
  A merged schema here would put body columns in scope above the join, and then
  `buildProjectSchema` / `inferExprType` would resolve `indexOf(name, slot)` for an outer name
  against a schema containing a same-named body column at a body slot — the exact silent
  wrong-relation hit `buildAggregateSchema`'s tripwire exists for.
- **Setting `join_type = JoinType::LEFT` to get the "preserve the left side" behaviour.** It
  would drag in null-extension, `on_residual`'s match-test semantics, and the outer-join
  `max()` cardinality rule — three wrong things for one right one.
- **`join_slot`.** For a STANDARD join it is the binder relation slot of `children[1]`, and
  `PredicatePushdown` routes conjuncts by it. There is no such slot for a body relation. Set it
  to `-1` and document that `-1` here means "children[1] is not in this block's range table" —
  then make sure every reader of `join_slot` either declines on `semantics != STANDARD` or is
  provably unreachable for such a node. That is a slot-table row, not a comment.

### Verification

- `--explain` on `SELECT * FROM laps WHERE lap_id IN (SELECT lap_id FROM drivers)` prints a
  `SemiJoin` node whose parent's column list is exactly `laps`'s.
- A unit test in `tests/test_logical_plan.cc` asserting
  `join->output_schema == join->children[0]->output_schema` for a SEMI node.
- Every pre-existing `--explain` string is byte-identical (the after-construction discipline).

---

## 2. The lowering decision — which shapes go down which path

### Why it matters

The README bullet says *"Lower `IN`, `NOT IN`, `EXISTS`, and `NOT EXISTS` **where applicable**"*.
"Where applicable" is the whole design decision, and getting it wrong costs performance in the
opposite direction from the one the week is trying to fix. Week 31 shipped
materialize-then-substitute; semi-join lowering is a **different production**, not a replacement
for it. Which shapes move is the first thing to settle, because §3 (the pass), §9 (the cap) and
§10 (the suites) all follow from it.

### Conceptual explanation

Materialization is right exactly when the subquery's contribution is **a constant**, and a
semi-join is right exactly when it is **a set membership test evaluated per outer row**.

| Shape | Week 31 today | Week 32 | Why |
|---|---|---|---|
| Uncorrelated `EXISTS` / `NOT EXISTS` | Runs the body once, substitutes constant `TRUE`/`FALSE` | **Unchanged — do not re-route** | An uncorrelated `EXISTS` does not depend on the outer row *at all*: its value is one boolean for the whole query. A semi-join would recompute that boolean as a hash probe per outer row, and — worse — would turn a query the optimizer can fold away entirely into a pipeline breaker. Materialization is strictly better here. Lowering it to a semi-join is the obvious wrong reading of "where applicable" |
| Uncorrelated `IN` / `NOT IN` | Materializes the set into the Week 25 `InExpr`; `evaluate()` compares **linearly per row**; capped at 1024 distinct values | **Lower to SEMI / ANTI join** | This is the shape the week exists for. `lap_id IN (SELECT lap_id FROM laps)` is 10 000 × 10 000 `Value` comparisons on the correctness baseline. A hash semi-join is O(left + right). This is also what removes the cap (§9) |
| Correlated anything | Refused: `correlated subqueries are not yet executable (Week 33)` | **Unchanged — still refused** | Week 33's. The operator built this week is what Week 33's decorrelation will target; building the operator and building decorrelation are two different weeks, and the README separates them |
| Scalar `(SELECT ...)` | Materialized to a `Literal` | **Unchanged** | Not set membership. No semi-join shape exists for it |

So the surface Week 32 actually changes is **`IN (subquery)` and `NOT IN (subquery)`, in the
uncorrelated case only**, and the checkpoint — *"set-membership subqueries avoid nested-loop
execution"* — is exactly that sentence.

### The tradeoff to surface rather than hide

Materialization gives `WHERE x IN (…)` a `ColumnRef IN <literal list>`, which is a shape
`ChunkPruner`'s `collectSimplePredicates` and `CardinalityEstimator::selectivity` may already
pattern-match. A semi-join gives up whatever they do with it, in exchange for dropping the
quadratic probe. **Check what is actually lost before assuming it is nothing** — grep
`chunk_pruner.h` and `cardinality_estimator.cc` for their `InExpr` handling. If `InExpr`
contributes a real pruning hint today, note the regression honestly in the plan and in the
README rather than discovering it in Week 37's benchmark; if it does not, say so and move on.

**Do not keep both paths behind a cost threshold.** Two paths that must agree on NULL semantics
(§8) is precisely the drift the README punishes repeatedly — Week 30's "one check, at the end of
`Validator::validate`", Week 28's "one `joinCardinality`, shared by the search and the stamp",
Week 26's split refusals. One production, one set of semantics.

### Verification

- Every `EXISTS` query in `tests/test_subquery.cc` and in the oracle suite still produces a plan
  with **no** join node added, and its `--explain` output is unchanged from Week 31.
- Every `IN (subquery)` query produces a `SemiJoin`; every `NOT IN (subquery)` an `AntiJoin`.
- Correlated queries still emit the Week 33 message, byte-identical, in all four modes.

---

## 3. The rewrite pass — `SubqueryExpr{IN}` → `LogicalJoin{SEMI|ANTI}`

### Why it matters

This is the only new *structure* in the week. It decides where the body's plan is built, when it
is built relative to `Validator::validate` and `materializeSubqueries`, and where the join node
is spliced into the outer tree. Get the ordering wrong and you either get a silent wrong answer
(running before validation — the exact trap `subquery_materialization.h` documents) or a throw
from dispatch sites 12/13, which still refuse a `SubqueryExpr`.

### Conceptual explanation

**It cannot live where materialization lives.** `materializeSubqueries` runs on the **AST**, in
`main.cc`, before planning, and its output is an `Expr`. A semi-join's output is a *plan node*,
so the rewrite must happen where plan nodes are built: inside `LogicalPlanBuilder`, after the
`FROM`/`JOIN` spine exists and before the `WHERE` `LogicalFilter` is constructed.

**The split with materialization must be explicit, and made in one place.** `materializeSubqueries`
currently consumes every `SubqueryExpr`. It must now **skip** `kind == IN` and leave that node
in the predicate, materializing only `SCALAR` and `EXISTS`. Two consequences follow immediately:

- `has_subquery` must **not** be cleared while an `IN` node survives. It is documented as "a
  `SubqueryExpr` is still in this tree", and `buildScanSchema` widens to the full schema while it
  is set — which is the behaviour you want, because the operand column must survive narrowing.
- The `IN` node must be removed from the predicate **before** `inferExprType` and
  `buildProjectSchema` walk it. Sites 12 and 13 still throw a Week-31 message on a
  `SubqueryExpr`, deliberately, so that a missed subtype is loud. That throw is your tripwire: if
  it fires, the extraction ran too late or missed a nesting position.

**The shape of the rewrite.** For `WHERE p AND x IN (SELECT k FROM t) AND q`:

```
LogicalFilter [p AND q]                 <- IN conjunct REMOVED from the predicate
  LogicalJoin {SEMI, keys=[x = k]}      <- output_schema == left child's, unchanged
    <FROM/JOIN spine>                   <- children[0], the probe side
    <plan of (SELECT k FROM t)>         <- children[1], the build side
```

Order of operations, per `WHERE` conjunct, after the spine is built:

1. Flatten the `WHERE` `AND`-chain — the same flatten `classifyJoinCondition` and
   `PredicatePushdown` already use. Do not invent a second flattener.
2. A conjunct that **is** a bare `SubqueryExpr{IN}` (possibly `negated`) is extracted: build the
   body's plan, wrap the spine in a `LogicalJoin{SEMI|ANTI}`, drop the conjunct.
3. A conjunct that merely *contains* an `IN` subquery below its root — e.g.
   `x IN (SELECT …) OR y > 5` — is **not** extractable: a semi-join is a whole-conjunct
   construct, and there is no disjunctive semi-join here. Since materialization no longer handles
   `IN`, such a query would now reach sites 12/13 and throw an unhelpful Week-31 message. Decide
   deliberately: either keep the materialization path alive for exactly the non-top-level `IN`
   (which re-opens the two-paths problem and the cap), or **refuse it with a message naming the
   restriction**. Refusing is the smaller, more honest change and matches the dialect table's
   existing style — no TPC-H query puts an `IN (subquery)` under an `OR`. Whichever you pick,
   it must be a stated row in the README's dialect table and an entry in the rejection suite,
   because the diffed oracle suite **cannot hold a query that errors**.
4. Remaining conjuncts are re-conjoined into the `LogicalFilter` exactly as today.

**Building the body's plan.** The body is uncorrelated and already bound and validated, so it is
a self-contained query block: call the same `LogicalPlanBuilder` entry point on it recursively.
`Validator` already guarantees it has exactly one output column (`IN subquery must return
exactly one column`), so the equi-join key's `join_col` is that column, resolved against the body
plan's own output schema.

**Two `SubqueryExpr` nodes over one shared statement** — `cloneExpr` shares the `shared_ptr`,
so `(SELECT …)` really can appear twice over one statement. For materialization Week 31 solved
this with a cache keyed on the statement address. For semi-join lowering the answer is different
and simpler: two conjuncts means **two semi-joins**, and that is correct — they are two separate
membership tests against the same relation. There is nothing to cache; do not port the cache.

### Code snippet (illustrative — new `src/planner/subquery_lowering.{h,cc}`)

```cpp
// Week 32 — set-membership lowering. A DIFFERENT PRODUCTION from Week 31's
// materialization (subquery_materialization.h), not a replacement for it:
// materialization is right when the subquery's contribution is a CONSTANT
// (scalar, uncorrelated EXISTS); a semi-join is right when it is a MEMBERSHIP
// TEST evaluated per outer row (IN / NOT IN). See docs/week-32-plan.md §2.
//
// PRECONDITIONS, all established by Validator::validate, which both planner
// entry points run FIRST — the same trust-don't-recheck stance
// materializeSubqueries takes, and for the same reason: re-checking here would
// put the message in a layer that does not own it.
//   - no correlated subquery anywhere (refused in Week 33's name), so every
//     ColumnRef in the body is query_level 0 against the BODY's range table;
//   - an IN body has exactly one output column.
//
// Extracts every top-level WHERE conjunct that IS a SubqueryExpr{IN} and wraps
// `spine` in one LogicalJoin{SEMI|ANTI} per extraction. `conjuncts` is edited in
// place; what is left is re-conjoined into the LogicalFilter by the caller.
std::unique_ptr<LogicalPlanNode> lowerInSubqueries(
    std::unique_ptr<LogicalPlanNode> spine,
    std::vector<std::unique_ptr<Expr>>& conjuncts,
    const Catalog& catalog);
```

```cpp
// inside lowerInSubqueries, per extracted conjunct:

const auto* sq = static_cast<const SubqueryExpr*>(conjunct.get());

// The body is a self-contained block: plan it with the same builder. Its refs
// are level 0 against ITS range table, which is why nothing here needs a level.
auto body_plan = LogicalPlanBuilder(catalog).build(*sq->statement);

// One equi-join key. from_col/from_slot come from the OPERAND, which belongs to
// the ENCLOSING query and is already bound at level 0 there — so from_slot is a
// slot in the outer range table, exactly the domain leftKeyIndices() resolves
// against. join_col is the body's single output column, resolved against
// body_plan's own schema. The two slots never meet.
const auto* operand = static_cast<const ColumnRef*>(sq->operand.get());
JoinKey key{ operand->column_name,
             body_plan->output_schema.columns()[0].name,
             operand->relation_slot };

// !! output_schema is the LEFT child's, NOT a merged schema. This is the
// invariant that keeps the body's slot numbering out of the outer plan
// (docs/week-32-plan.md §0). join_slot is -1: children[1] is not a relation of
// this block's range table, so there is no slot to name it by, and every reader
// of join_slot must decline on semantics != STANDARD.
Schema out = spine->output_schema;
auto join = std::make_unique<LogicalJoin>(std::move(spine), std::move(body_plan),
                                          std::vector<JoinKey>{key}, /*join_slot=*/-1,
                                          std::move(out));
join->semantics = sq->negated ? JoinSemantics::ANTI : JoinSemantics::SEMI;
spine = std::move(join);
```

### Implementation guidance

1. Teach `materializeSubqueries` to skip `kind == IN`. One condition, at the classification
   point — not a second walker. Keep `has_subquery` set while any node survives.
2. Add the new pass and call it from `LogicalPlanBuilder` between the spine construction and the
   `LogicalFilter` construction. `HAVING` is a second `LogicalFilter` position; decide explicitly
   whether an `IN` subquery in `HAVING` is lowered (it sits above the aggregate, so the semi-join
   would have to go above `LogicalAggregate` — legal, and the operand is a group/aggregate output
   column) or left to materialization. TPC-H's `HAVING` subqueries (Q11) are **scalar**, so the
   minimum-code answer is to lower only `WHERE` and leave `HAVING`'s `IN` to materialization —
   but if you do, say so in the dialect table, and the cap cannot be fully removed (§9).
3. `cloneExpr` (site 11) and `exprKey` (site 1) are unchanged — the node still exists in the AST;
   only its lowering is new.
4. Re-read the dispatch-site table in `development.md` before the commit, not after. Ten of the
   eighteen sites were handled-but-unexercised as of Week 30; Week 31 armed some. This week arms
   the `IN` path through the *planner*, which is a different set from the ones materialization
   armed.

### Anticipated mistakes specific to this codebase

- **Running the pass before `Validator::validate`.** The preconditions are not re-checked.
  `subquery_materialization.h` spells out what that costs: a wrong answer instead of a
  diagnostic.
- **Assuming the operand is always a plain `ColumnRef`.** The grammar allows
  `additive [NOT] IN (select_stmt)`, so `x + 1 IN (SELECT …)` parses. `JoinKey` holds column
  *names*, not expressions — there is no computed-key join in this engine. Refuse a non-`ColumnRef`
  operand with a stated message, and put it in the dialect table and the rejection suite.
  Silently falling back to materialization re-opens the two-paths problem.
- **Forgetting that `PredicatePushdown` runs after this.** It rewrites a `FILTER` whose **direct**
  child is a `JOIN` — and now the direct child of the `WHERE` filter is a *semi*-join. Confirm the
  pushdown decline lands (§1 step 3) and that it does not silently stop pushing predicates it used
  to push, which is the failure Week 27 hit when it nearly stacked a second filter node.
- **`buildScanSchema` narrowing.** With `has_subquery` still set for an `IN` query, the scan
  widens to the full schema — projection pushdown stays off for these queries, as in Week 31.
  Expected, not a regression; note it so Week 37's benchmark does not read it as one.

### Verification

- `--explain` shows exactly one `SemiJoin`/`AntiJoin` per top-level `IN` conjunct, in the right
  position (below the `WHERE` filter, above the spine).
- Two `IN` conjuncts produce two stacked joins.
- A `WHERE` with an `IN` and other conjuncts still shows those conjuncts in the `Filter` node.
- Sites 12/13 never throw for a supported shape — and *do* throw (or the new refusal fires) for
  the `OR`-nested and computed-operand shapes, asserted in the rejection suite.

---

## 4. Cardinality — the semi/anti rule, stamped and never searched

### Why it matters

The README's Week 32 starting note is entirely about this, and it points at Week 29's note rather
than restating it *so the two cannot drift*. Read both. The rule feeds the cost model, which
feeds physical join selection, which is the one decision `--explain` claims the optimizer made.

### Conceptual explanation

**The outer-join rule is the wrong shape.** `max(selectivity(residual) * matches, left_rows)`
encodes "every left row survives, at least once" — null-extension. A semi-join preserves *some*
left rows, each *at most* once; an anti-join preserves the complement. Neither null-extends.

The standard estimates, in this engine's vocabulary:

- **Semi:** `left_rows * min(1.0, ndv(right_key) / ndv(left_key))` — the fraction of distinct
  left key values that appear on the right, applied to the left row count. Clamped to
  `[0, left_rows]`. Note this is a *selectivity on the left*, not a product: the right side
  contributes only through its NDV.
- **Anti:** `left_rows - semi_estimate`, floored at the engine's existing ≥ 1-row floor.

`ndv` comes from the same `StatsContext` / `ColumnStatsEntry` machinery `joinCardinality`
already uses for its left lookup, with the same discipline: track `have_ndv` separately, so an
NDV of 1 stays a usable statistic rather than being confused with "no statistic" — a rule this
codebase has corrected **twice**.

**Where it lives — this is the load-bearing part.** Both rules are **non-multiplicative** (a
`min`/clamp against `left_rows`, and a subtraction). Week 28 moved the ≥ 1-row floor out of the
search for exactly this reason, and Week 29 moved the outer `max()` out for the same one: if a
non-multiplicative rule lives inside `joinCardinality`, a subset's estimated row count depends on
the *path* that reached it, and the DP's optimal substructure is gone. So:

**Put the SEMI/ANTI rule at the stamping site — `CardinalityEstimator::estimateNode`'s JOIN
case, next to the `join_type == JoinType::LEFT` block — and never inside `joinCardinality`.**

And, as Week 29 requires, **both facts must be true independently**: enumeration declines
semi/anti trees (§5), *and* the rule is out of the search. Do not let either be the reason the
other is unnecessary.

### Code snippet (illustrative — `src/planner/cardinality_estimator.cc`, JOIN case)

```cpp
// Week 32 — semi/anti. AT THE STAMP, never inside joinCardinality: both rules
// are NON-MULTIPLICATIVE (a clamp against l_rows, and a subtraction), so a
// subset's estimate would depend on the path that reached it and the DP's
// optimal substructure would be gone. Identical argument to the >=1-row floor
// (Week 28) and the outer-join max() below (Week 29). JoinEnumeration also
// declines these trees — both facts must hold INDEPENDENTLY.
if (join.semantics != JoinSemantics::STANDARD) {
    // Semi-join selectivity is a property of the LEFT side: the fraction of
    // left rows whose key value also occurs on the right. The right side
    // contributes only its NDV, never its row count — which is exactly why the
    // product form is wrong here.
    double frac = 1.0;
    if (have_left_ndv && have_right_ndv && left_ndv > 0.0) {
        frac = std::min(1.0, right_ndv / left_ndv);
    }
    double semi = l_rows * frac;
    rows = (join.semantics == JoinSemantics::SEMI) ? semi
                                                   : (l_rows - semi);
    rows = std::max(0.0, std::min(rows, l_rows));   // never exceeds the left side
}
```

### Implementation guidance

1. Read the existing JOIN case end to end first. The `LEFT` block, the `on_residual` handling and
   the ≥ 1-row floor are all there and interact.
2. Reuse the existing NDV lookup, do not write a second one. `joinCardinality` takes the max of
   the two keys' NDVs; the semi rule needs them *separately*, so extract the lookup rather than
   duplicating the resolution logic — and keep `have_ndv` tracked separately from the value.
3. A semi/anti join has no `on_residual` (§3 builds none), so do not add residual selectivity
   handling for a case that cannot occur — but assert it rather than assume it.
4. Add the row to `development.md`'s slot-consumer table: `CardinalityEstimator`'s SEMI/ANTI
   branch reads `JoinKey::from_slot` for the left lookup, which is level 0 in the outer block by
   §0's containment.

### Verification

- `--explain-analyze` on `x IN (SELECT k FROM t)`: the semi-join's `est=` is ≤ the left child's
  `rows_out`, always, on every query in the suite. This is a **checkable invariant**, not a hope
  — the outer join's `est=10000 / rows_out=20` mismatch is how the equivalent bug was found in
  Week 29.
- Anti + semi estimates for the same pair sum to the left estimate.
- `tests/test_cardinality.cc`: a hand-built SEMI node with known NDVs.
- `tests/test_join_enumeration.cc`: `joinCardinality` is never called with a semi/anti node —
  assert by construction (the decline in §5 makes it unreachable) *and* by test.

---

## 5. Physical planning — vectorized lowering, forced sides, enumeration decline

### Why it matters

`VectorizedPlanBuilder` is the only path that will execute a multi-relation query with a
semi-join, and it is where two Week 29 decisions must be repeated rather than re-derived: the
build side is **forced, not costed**, and enumeration **declines** the tree.

### Conceptual explanation

**The build side is forced.** A hash semi-join emits probe-side rows. The probe input must
therefore be the side whose rows survive — the outer spine — and the build input must be the
subquery body. This is the same "the one place Week 22's build-side decision does not apply"
argument `vec_hash_join_node.h` already makes for `left_outer`, and it has the same enforcement:
`swapped == false` is the only legal combination, and the constructor **throws** on the illegal
one rather than emitting wrong rows. Reuse that stance; do not add a second, softer guard.

Consequence for `--explain`: `setCostDecision` must **not** be called for a semi/anti join. The
discipline is stated at `LogicalJoin::order_decision` and `VecHashJoinNode::cost_decision_` —
never print a cost decision when estimates did not drive the choice, or `--explain` claims an
optimizer choice that never happened.

**Enumeration declines.** `JoinEnumeration` already declines any tree containing an outer join.
Add semi/anti to the same decline, for two reasons, both worth stating in the code:

1. The body subtree's relation slots are **not in the outer range table**. This is precisely the
   condition `JoinEnumeration`'s Week 30 silent decline tests for — the one Weeks 28–30 expected
   Week 31 to make live and Week 31 reported it had not. **Week 32 is where it becomes live.**
   Verify that the decline actually fires (put a test on it) rather than trusting the hoisted
   check; it was hoisted above `decompose()` precisely because a decline found afterwards has
   nothing clean to return.
2. Semi-joins are not freely reorderable with inner joins in this engine's left-deep shape, and
   `JoinEnumeration::rebuild` sets `from_slot = 0` on the first join's keys — a rule that assumes
   exactly one relation is present at the bottom, which a semi-join's build side violates.

If a supported query starts paying a *real* plan-quality cost for that decline, that is when it
earns a reported decision, in the shape Week 29's `join-ordering=skipped (outer join)` uses — not
before.

### Code snippet (illustrative — `src/planner/vectorized_plan_builder.cc`)

```cpp
// Week 32 — SEMI/ANTI. The side is FORCED, not costed: a hash semi-join emits
// PROBE-side rows, so the outer spine must be the probe input and the subquery
// body the build input. Same stance as Week 29's left_outer (vec_hash_join_node.h)
// and the same enforcement: swapped == false is the only legal combination and
// the constructor throws on the other one.
//
// No setCostDecision() here: estimates did not drive this choice, and printing
// one would make --explain claim an optimizer decision that never happened
// (the discipline at LogicalJoin::order_decision).
if (join->semantics != JoinSemantics::STANDARD) {
    auto probe = build(join->children[0].get());   // outer spine
    auto bld   = build(join->children[1].get());   // subquery body
    // Key indices resolve in DIFFERENT schemas and that is the point: probe
    // against the outer merged schema (by slot, via leftKeyIndices()), build
    // against the body's own schema. The two numbering domains never meet
    // because output_schema below is the PROBE schema, unmerged.
    return std::make_unique<VecHashJoinNode>(
        std::move(probe), std::move(bld),
        leftKeyIndices(join->keys, join->children[0]->output_schema),
        buildKeyIndices(join->keys, join->children[1]->output_schema),
        join->output_schema,                       // == probe schema
        /*swapped=*/false,
        /*left_outer=*/false,
        /*on_residual=*/nullptr,
        join->semantics);                          // new trailing parameter
}
```

### Implementation guidance

1. Add the semantics as a **trailing defaulted parameter** on `VecHashJoinNode`'s constructor, so
   every existing construction and every hand-built test tree compiles unchanged — the same
   after-the-fact discipline `left_outer` and `on_residual` used.
2. Assert in the constructor: `semantics != STANDARD` implies `!swapped`, `!left_outer`,
   `on_residual == nullptr`, and `output_schema == probe_child->outputSchema()`.
3. `leftKeyIndices()` **throws** on a miss, deliberately, since Week 27 — the bare-name fallback
   is the bug. Keep that. But note the `from_slot` contract: it is the slot *as presented by the
   left child's own schema*, 0 for a single relation. For a semi-join over a single-relation FROM,
   `operand->relation_slot` is 0 and that is correct; over a join spine it is the binder slot and
   that is also correct. Both work only because the semi-join sits *above* the whole spine.
4. `PredicatePushdown`: add the decline. Then check the pushdown *ordering* — the semi-join is
   introduced by the logical builder, so pushdown sees it. Confirm a `WHERE` conjunct that used to
   push to a scan still does, since the semi-join is now between the filter and the spine.

### Verification

- `tests/test_vec_plan_builder.cc`: a SEMI logical node lowers to a `VecHashJoinNode` with the
  body on the build side and `swapped == false`; the illegal combination throws.
- `tests/test_join_enumeration.cc`: a tree containing a SEMI join is declined, and the decline is
  the *slot-outside-the-range-table* one — assert which decline fired, not just that one did.
- `--explain` on a semi-join query prints no `build=... cost=...` annotation.

---

## 6. Vectorized semi-join / anti-join execution

### Why it matters

This is the README's first bullet and the thing the checkpoint measures. Everything above only
decides *which* operator runs; this decides whether its answers are right.

### Conceptual explanation

The build phase differs from an inner join's in one important way: **a semi-join needs a hash
*set*, not a hash multimap.** `VecHashJoinNode` currently builds
`unordered_map<string, vector<Row>>` — the payload is the build-side rows, which a semi-join
never emits. Storing them is pure waste, and worse, it invites an implementation that emits one
output row per match, which is the duplicate bug the whole operator exists to avoid.

The probe phase:

- **SEMI:** for each probe row, serialize the key (`key_encoding.h` — do not write a second
  encoder; the shared encoder is why the search and the stamp cannot disagree, and the same
  argument applies here), look it up, emit the probe row **once** on a hit, skip on a miss.
- **ANTI:** the exact complement — emit on a miss, skip on a hit. Plus the NULL rule in §8, which
  is not symmetric and is the sharpest correctness item in the week.

**Late materialization.** Because the output schema *is* the probe schema, a semi-join is
structurally a **filter**: it selects a subset of probe rows and changes no column. The
late-materialization-correct implementation therefore produces a `SelectionVector` over the probe
chunk and copies nothing — exactly what `VecFilterNode` does. The existing `VecHashJoinNode`
assembles `Row`s into `output_buffer_` because a real join must merge two schemas; a semi-join
must not.

Surface the tradeoff rather than deciding it silently:
- **Row path (reuse `output_buffer_`)**: smaller diff, obviously correct, one code path in the
  node. Costs a full row copy per surviving row and forfeits the design principle Phase 3 is
  built on.
- **Selection-vector path**: matches `VecFilterNode`, copies nothing, and is what makes the
  checkpoint's "avoids nested-loop execution" claim mean something at scale. Costs a second
  output mode inside `VecHashJoinNode`, and `SelectionVector` cascading rules must hold (the
  `vectorized-audit` skill's invariants).

**Recommendation:** ship the selection-vector path, because the operator is a filter and
pretending otherwise is the kind of "correct but structurally wrong" that later weeks pay for.
If time is short, ship the row path and record the deferral as a starting note for Week 37 — but
do not ship both.

### Code snippet (illustrative — `src/execution/vec_hash_join_node.cc`)

```cpp
// Week 32 — SEMI/ANTI build phase. A hash SET, not the map: a semi-join never
// emits a build-side row, so storing them wastes the memory AND invites an
// implementation that emits one output row per match — the duplicate bug this
// operator exists to prevent (R semi S is pi_R(R join S) with duplicates
// COLLAPSED, not an inner join with a projection on top).
std::unordered_set<std::string> build_keys_;
bool build_had_null_key_ = false;   // ANTI's NULL rule, see §8

// ... build loop, using the SHARED encoder in key_encoding.h -- do not write a
// second one; one encoder is what keeps build and probe from disagreeing, the
// same argument that produced key_encoding.h in the first place.

// SEMI/ANTI probe phase. Structurally a FILTER: the output schema IS the probe
// schema, so nothing is assembled and nothing is copied -- a SelectionVector
// over the probe chunk, exactly like VecFilterNode.
for (int r = 0; r < probe_chunk->num_rows; ++r) {
    if (probeKeyIsNull(*probe_chunk, r)) continue;   // §8: NULL key never emits,
                                                     // for SEMI and ANTI alike
    const bool hit = build_keys_.count(encodeProbeKey(*probe_chunk, r)) > 0;
    const bool emit = (semantics_ == JoinSemantics::SEMI) ? hit : !hit;
    if (emit) sel_.indices[sel_.size++] = r;         // ONCE per probe row
}
```

### Implementation guidance

1. Keep the two phases in `VecHashJoinNode` rather than adding a new node type. The build/probe
   machinery, the key encoder and the chunk lifecycle are identical; only the payload and the
   emit rule differ. A separate node duplicates the lifecycle, and the lifecycle is where
   Volcano/vectorized divergences have historically come from.
2. `build_width_` is read off `build_child_`'s schema in `open()` for null-extension. For a
   semi/anti join it is meaningless — guard it, do not let it silently compute a width nothing
   uses.
3. The build side is a *plan subtree*, not a table: it is opened, drained and closed like any
   child. It is a **pipeline breaker**, exactly like the existing build side.
4. Run the `vectorized-audit` skill's checklist on the new path — `SelectionVector` invariants,
   `DataChunk` lifecycle, blocking-operator behaviour, cascading. This is a new selection-vector
   producer, which is the case that checklist is for.

### Verification

- `operator-correctness` skill: hand-simulate SEMI and ANTI on ~6 concrete rows including a
  duplicate build key (must **not** duplicate the probe row) and an empty build side (SEMI emits
  nothing; ANTI emits every non-NULL-key probe row).
- Vectorized results equal Volcano results for every supported query (`optimizer-diff` and the
  four-mode harness).
- The duplicate test is the one that catches the "inner join with a projection" mistake, and it
  is the single most valuable test in the week.

---

## 7. Volcano — semi/anti, or a stated refusal

### Why it matters

Volcano is **the correctness baseline**, and `compare_against_sqlite.py` runs it as a separate
oracle leg. If the semi-join exists only on the vectorized path, every `IN (subquery)` query
loses its baseline and the four-mode agreement the harness is built on. But `Planner::plan`
builds **exactly one join**, and a semi-join is a *second* join for any query whose `FROM` already
joins — so full parity is not free.

### Conceptual explanation

Two live constraints in `Planner::plan`:

1. The `stmt.joins.size() > 1` refusal (`three or more relations execute on --execution
   vectorized only`). A semi-join is not a `stmt.joins` entry, so the refusal does not
   automatically cover it — but the *structure* it protects (one `HashJoinNode`) does.
2. `preserved_slots` is now **derived** from the FROM scan's own schema (Week 30's fix), so it no
   longer silently depends on that refusal. Do not re-introduce a constant.

The honest options:

- **(a) Support semi/anti in `HashJoinNode` for a single-relation `FROM`.** Covers
  `SELECT … FROM laps WHERE lap_id IN (SELECT …)`, which is the shape the checkpoint is about and
  the shape the oracle suite mostly holds. Multi-relation `FROM` + `IN` refuses with the existing
  Week 27-style message. This preserves a Volcano baseline for the queries that matter and keeps
  the capability boundary in the shape the README already documents.
- **(b) Refuse semi/anti on Volcano entirely.** Smaller, but it invents a capability difference
  for the *most common* shape and blinds the oracle for the whole feature.

**Recommendation: (a).** The delta is one build/probe variant in `HashJoinNode` mirroring §6 —
the same hash set, the same emit-once rule, the same NULL rule — and it is what lets the diffed
suite carry these queries in all four modes rather than two.

### Implementation guidance

- `HashJoinNode` is the semantic reference. Implement SEMI/ANTI there **first**, hand-simulate it
  (§6), and make the vectorized version match it — not the other way round.
- Whichever refusal you land, it goes in `Planner::plan` and must produce a message naming the
  restriction and the mode, matching `MULTIWAY_VOLCANO_REJECTED`'s existing shape. Then add it to
  `python_tools/compare_against_sqlite.py`'s Volcano-only rejection suite
  (`WEEK31_SUBQUERY_VOLCANO_REJECTED` is the current example of that pattern).
- The refusal must **not** be duplicated per engine in `Validator`. Week 26 split refusals across
  engines only because the engines genuinely differed in capability — which is true here, so the
  refusal belongs in `Planner::plan`, not in the shared validator. Getting this backwards is how
  the two paths drift.

### Verification

- Same query, all four modes, identical rows — or an identical refusal message in exactly the
  two Volcano modes, asserted in the rejection suite.
- `bisect-stage` if a mode diverges: the mode matrix will localize it to lowering vs. execution
  immediately.

---

## 8. NULL semantics — `NOT IN` is not simply an anti-join

### Why it matters

This is the correctness item most likely to ship wrong, because the shipped CSV data cannot
express a NULL (`ColumnarTable` cannot either) so it will not surface by accident, and SQLite
*will* disagree the moment a NULL appears. Week 31 solved the identical problem at a place that
no longer exists.

### Conceptual explanation

SQL's three-valued logic, restated for this operator:

- `x IN S` is TRUE if `x` equals some element, FALSE if `S` has no NULLs and no match,
  **UNKNOWN** if `S` contains a NULL and there is no match, and UNKNOWN if `x` itself is NULL and
  `S` is non-empty.
- `x NOT IN S` is the negation: TRUE only if `S` has **no NULLs** and no match. If `S` contains
  any NULL, `x NOT IN S` is **never TRUE** — FALSE where `x` matches, UNKNOWN elsewhere.

A `WHERE` clause keeps only TRUE, so UNKNOWN and FALSE are indistinguishable here. Translating to
the operator:

| Situation | SEMI (`IN`) | ANTI (`NOT IN`) |
|---|---|---|
| Probe key is NULL | emit nothing | emit nothing |
| Build side contains **any** NULL key | normal (a NULL simply never matches) | **emit nothing at all** |
| Build side empty | emit nothing | emit every non-NULL-key probe row |
| Duplicate build keys | emit probe row **once** | n/a |

**Week 31 solved this at the substitution site** — `x NOT IN (S ∪ {NULL})` folded to a constant
false, and the positive form dropped the NULLs because UNKNOWN and FALSE are indistinguishable to
every consumer reachable there. That reasoning was a *proof*, resting on: no general `NOT`,
`IS NULL` parsing at the additive level, and `CASE WHEN` not taking an UNKNOWN branch. **The
proof still holds** — nothing this week adds a general `NOT` — so the same collapse is legal in
the operator. But the *site* is gone: a semi-join has no substitution point, so the rule must be
implemented in the probe loop, and the "build side contains a NULL" fact must be **carried out of
the build phase as a flag**.

That flag is the one piece of state that is easy to forget and impossible to notice: without it,
`NOT IN` over a nullable column returns rows SQLite does not.

### Implementation guidance

1. Set `build_had_null_key_` during the build loop, at the same place the key is encoded.
2. In `open()`/first `nextChunk()`, if `semantics_ == ANTI && build_had_null_key_`, short-circuit
   to "no output" — do not probe at all.
3. NULL detection must go through `ColumnVector::valueAt` / the validity mask, **not** through a
   sentinel. `all_valid == true` means the mask is empty and no row is NULL — the common case,
   since `ColumnarTable` cannot express NULL and scan output is always all-valid. A NULL can only
   arrive from a computed column (`x / 0` is the engine's first NULL producer) or a
   null-extending outer join below the semi-join. Both are constructible in a test.
4. Do the same in `HashJoinNode` (§7). One rule, two implementations, one test file.

### Verification

- Construct NULLs the way the engine actually makes them (`x / 0`, or a `LEFT JOIN` below), then
  diff against SQLite in all four modes. This is the only way to test it — the CSVs cannot hold a
  NULL.
- Explicit unit cases in `tests/test_execution.cc` / `tests/test_vectorized.cc` for each row of
  the table above.
- The NULL rows in `python_tools/compare_against_sqlite.py`'s diffed suite are the ones that
  matter; a passing suite without them is a suite that never tested this.

---

## 9. Removing the `IN (subquery)` 1024-distinct-value cap

### Why it matters

Week 31 documented `MAX_MATERIALIZED_IN_VALUES = 1024` in the README's dialect table as a
**deliberate divergence from SQLite** — "the bound is temporary by construction: Week 32's
semi-join materializes nothing" — and pinned it by message in `WEEK31_MATERIALIZATION_REFUSED`.
Removing it is this week's business, and it is the cleanest evidence that the week's design is
sound: the cap disappears *because* nothing is materialized, not because the constant was raised.

### Conceptual explanation

The cap is a bound on `evaluate()`'s **linear** `InExpr` scan, not a measurement. If §2's routing
is total — every `IN (subquery)` becomes a semi-join — then no subquery result ever reaches
`InExpr`, `MAX_MATERIALIZED_IN_VALUES` becomes dead code, and all three of its artifacts must go
together:

1. the constant and its comment in `src/planner/subquery_materialization.h`, plus the check that
   raises the refusal;
2. the dialect-table row in `README.md` (the row whose message names Week 32 by number);
3. the `WEEK31_MATERIALIZATION_REFUSED` entry in `python_tools/compare_against_sqlite.py`.

**And (3) is not a deletion — it is a move.** *The diffed oracle suite cannot hold a query that
errors*, which is exactly why the capped query lives in a rejection suite today. Once it is
answerable, that same SQL must **move into the diffed suite** so it is actually compared against
SQLite. Deleting the rejection entry without adding the diff entry silently reduces coverage at
the moment coverage matters most: `lap_id IN (SELECT lap_id FROM laps)` is the single query that
best demonstrates the week's checkpoint, and it is the one that has never been diffed.

**If §3's `HAVING` or `OR` decisions leave any materialization path for `IN` alive**, the cap
cannot be fully removed. Then the honest outcome is: keep the constant, **narrow the dialect-table
row** to the surviving shape (as Week 31 narrowed the correlated refusal rather than moving it),
and keep a rejection entry for exactly that shape. Say which outcome landed and why — do not leave
the README claiming a limit that no longer applies, or removing one that does.

### Verification

- `grep -rn MAX_MATERIALIZED_IN_VALUES src/` returns nothing (total removal) or returns exactly
  the narrowed path, matching the README row word for word.
- `lap_id IN (SELECT lap_id FROM laps)` runs in all four modes and diffs clean against SQLite.
- The rejection suite still passes — with the entry moved, not deleted.
- Time the query before and after: the cap existed because the materialized form was ~1e8 `Value`
  comparisons. A semi-join should be visibly faster, and that number belongs in the week's notes.

---

## 10. Tests, oracle, rejection suite, and the verification gate

### Why it matters

Week 32 changes what is **refused**, and refusals are invisible to the diffed oracle by
construction. It also adds an operator whose failure mode (duplicated probe rows) produces
*plausible* output. Neither is caught by "the build passes".

### Success criteria — define these before writing code, and loop until every one is green

1. `SELECT … FROM laps WHERE lap_id IN (SELECT lap_id FROM drivers)` returns SQLite's rows in all
   four modes (or in two, with an asserted Volcano refusal per §7).
2. The same for `NOT IN`, including a build side containing a NULL (§8), and including an empty
   build side.
3. A body with duplicate keys does **not** duplicate outer rows.
4. `--explain` shows `SemiJoin` / `AntiJoin`, no cost annotation, and no join-ordering annotation.
5. `--explain-analyze`: the semi-join's `est=` never exceeds its left child's `rows_out` on any
   suite query.
6. `EXISTS` / `NOT EXISTS` / scalar plans and `--explain` strings are **unchanged** from Week 31.
7. Correlated subqueries still refuse with the byte-identical Week 33 message in all four modes.
8. The 1024-cap query is answered and diffed (§9), and its rejection entry has moved rather than
   vanished.
9. Every pre-existing test passes with byte-identical `--explain` output.

### Where each test goes

| File | What to add |
|---|---|
| `tests/test_subquery.cc` | Lowering-pass shape: one SEMI/ANTI per top-level `IN` conjunct; `EXISTS` still materializes; the `OR`-nested and computed-operand refusals |
| `tests/test_logical_plan.cc` | The `output_schema == children[0]->output_schema` invariant; `join_slot == -1` |
| `tests/test_cardinality.cc` | Semi/anti estimates; semi + anti = left; clamp at `left_rows` |
| `tests/test_join_enumeration.cc` | The decline fires, and it is the slot-outside-range-table one |
| `tests/test_vec_plan_builder.cc` | Forced build side; illegal `swapped`/`left_outer`/residual combinations throw |
| `tests/test_execution.cc`, `tests/test_vectorized.cc` | Per-row semantics from §6/§8, both engines, same expectations |
| `python_tools/compare_against_sqlite.py` | Diffed: the ex-capped query, `NOT IN` with NULL, `IN` over a join spine. Rejection: the new refusals, plus any Volcano-only one |

### The gate

Run the `verify` skill: build, C++ unit tests, the SQLite correctness harness, and the regression
harness across **all** storage/execution modes. Then `verification-before-completion` — evidence
before assertions. A green build is not a passing week; the four-mode diff is.

### Anticipated mistakes specific to this codebase

- **Asserting a refusal in the diffed suite.** It cannot hold a query that errors. Refusals go in
  a rejection suite, pinned **by message**, which is what makes a message change visible.
- **Testing only the vectorized path** because it is the one with the new operator. The value of
  the four-mode matrix is that it catches lowering bugs the operator tests cannot see.
- **Adding a consumer without a `development.md` row.** A missing row is worse than a wrong one —
  the next week reads the table as already-checked. This week adds at least four consumers (§0).

---

## Hand-forward notes for Week 33

- **The `ColumnId { level, slot }` trigger now points at Week 33**, and Week 33 *is* the week that
  first lowers a correlated reference. Do it as its own standalone change, before the feature
  work, not folded into it. 87 non-comment mentions across six source layers plus every test that
  hand-builds a `ColumnRef`.
- **Both Week 30 tripwires are still armed and still unreached.** Week 33 is the week that
  replaces them with real behaviour — replace, do not delete.
- **The semi/anti operator built this week is what decorrelation lowers into.** A correlated
  `EXISTS` decorrelates to a semi-join over the correlated key; a correlated `NOT EXISTS` to an
  anti-join. Week 33 should need no new operator — only the rewrite that produces the join keys.
- **One plan now holds two range tables** (§0). The containment is that a semi/anti join's output
  schema is its left child's. Week 34's derived tables break that containment for real, because a
  derived table's columns *are* in scope above it.


---

## Progress

Status at the end of the first implementation run. Commits are split by pipeline
layer and every one is pushed to `claude/phase5-week26-qomtkb`.

### Done

- **§1 — `LogicalJoin` gains SEMI/ANTI.** `JoinSemantics` enum in
  `src/planner/logical_plan.h`, set after construction; `join_slot == -1` with the
  contract stated on the field; `explain()` prints `LogicalSemiJoin` /
  `LogicalAntiJoin` by node name, so every pre-existing plan string is unchanged.
- **§2/§3 — the lowering pass.** New `src/planner/subquery_lowering.{h,cc}`,
  called from `LogicalPlanBuilder::build` between the spine and the `WHERE`
  filter. `materializeSubqueries` skips `Kind::IN` and keeps `has_subquery` set
  while one survives. `EXISTS` / scalar routing is unchanged.
  `splitConjuncts` moved from `predicate_pushdown.cc`'s anonymous namespace to
  `parser/expr_utils.h` so both passes share one notion of "a conjunct".
  **Refused, each with a stated message:** an `IN` under an `OR` (not a whole
  top-level conjunct), an `IN` in `HAVING`, a computed operand, and a body
  statement shared by two `SubqueryExpr` nodes.
- **§4 — cardinality.** SEMI/ANTI rule at the stamp in
  `CardinalityEstimator::estimateNode`'s JOIN case, never inside
  `joinCardinality`. Asserts a semi/anti join carries no `on_residual`.
  `JoinEnumeration` declines independently: `hasSlotOutsideRangeTable` already
  fires on `join_slot == -1`, so Week 30's silent decline is now live — **this
  contradicts Week 31's hand-forward note, which predicted Week 34.**
- **§5 — physical planning.** `VectorizedPlanBuilder` forces the side
  (`swapped=false`, spine probes, body builds), calls no `setCostDecision`.
- **§6/§8 — vectorized execution.** `VecHashJoinNode` takes a trailing defaulted
  `semantics` parameter, builds a hash **set** (`build_keys_`), emits each probe
  row once, and carries `build_had_null_key_` out of the build phase so an ANTI
  probe short-circuits to no output. Constructor throws on every illegal
  combination. Explain prints `VecSemiHashJoin` / `VecAntiHashJoin`.
- **§7 — Volcano.** Option (b), not the plan's recommended (a): `Planner::plan`
  refuses an `IN` subquery by name (`"IN subqueries are lowered to a semi-join
  and are not supported on the Volcano path; use --execution vectorized"`). The
  cost is stated at the site — these queries are now diffed in two modes, not
  four — and pinned in the harness's Volcano rejection suite.
- **§9 — the 1024 cap is gone.** `MAX_MATERIALIZED_IN_VALUES` removed outright,
  along with `distinctNonNull` and the whole `Kind::IN` materialization branch
  (which now throws an internal message, deliberately, since it is unreachable).
- **§10 (partial) — the oracle.** `python_tools/compare_against_sqlite.py` has
  `WEEK32_SEMI_JOIN_VEC_ONLY` (diffed, two modes),
  `WEEK32_SEMI_JOIN_VOLCANO_REJECTED` and `WEEK32_LOWERING_REFUSED` (four modes).
  The cap query **moved** into the diffed suite. Duplicate build keys, a NULL on
  the build side, a NULL on the probe side and an empty build side are all
  covered.

### Deferred, with the reason

- **§6's selection-vector path.** The row path shipped: `output_buffer_` holds a
  copy of each surviving probe row. Correct, but it forfeits late
  materialization for an operator that is structurally a filter. **This is a
  Week 37 starting note**, exactly as §6 allows — do not ship both paths.
- **§7's option (a)**, semi/anti in Volcano's `HashJoinNode` for a
  single-relation `FROM`. It is what restores the four-mode baseline for the
  feature; the refusal is the honest interim, not the intended end state.

### Done in the second run (continuation)

- **§10 — the C++ unit tests.** All of §10's table is now covered.
  - `tests/test_vectorized.cc` — the predecessor's in-flight edit, completed.
    Duplicate build key emits the probe row once (the test the operator exists
    for), the anti complement, an empty build side in both polarities, and the
    three NULL rows of §8's table. **Two of those tests failed as written** and
    the operator was not the reason: they used `makeScan`, which builds a
    `ColumnarTable` and cannot express a NULL, so `Value::null()` came back out
    as a plain `0` and the NULL rule was asserted against data holding no NULL.
    They now go through `NullableSourceNode`, and a comment at the helper says
    why, because this is the trap the whole section exists to catch.
  - `tests/test_join_enumeration.cc` — the slot-outside-the-range-table decline
    fires on a three-scan semi-join query, and the test asserts **which** decline
    fired (silent, no `order_decision`) rather than only that the order did not
    change: the outer-join decline is reported, so a weaker test would pass on
    either and would not notice the two swapping.
  - `tests/test_cardinality.cc` — hand-built SEMI/ANTI nodes with known NDVs.
    The right side is seeded with **20 rows over 10 distinct keys on purpose**:
    with rows == NDV the semi rule and `joinCardinality`'s product form agree
    numerically and the test would pass against the wrong rule. Semi 200 vs
    product 400, both pinned; semi + anti = left; the clamp at `left_rows` with
    the anti side never going negative; the `on_residual` assertion throws.
  - `tests/test_vec_plan_builder.cc` — the forced build side, written with a
    **big body and a small spine** so a cost-based choice would put them the
    other way round; no `cost=` / `build=` annotation; two `IN` conjuncts give
    two stacked joins. The body arrives wrapped in its own `VecProject`, so the
    table-name assertion recurses — which is the week's shape, not an
    inconvenience.
  - `tests/test_subquery.cc` already carried the lowering-shape tests and the
    `output_schema == children[0]->output_schema` / `join_slot == -1` invariant
    (commit `5079b00`), so `tests/test_logical_plan.cc` needed nothing.
  - Full unit suite: **768 tests, all passing.**
- **§4's verification — the `--explain-analyze` invariant, now checked across
  the suite** rather than spot-checked: all 13 `WEEK32_SEMI_JOIN_VEC_ONLY`
  queries × both storage modes, `est=` never exceeds the semi-join's left
  child's `rows_out`. Zero violations.
- **§9/§10 — `README.md`.** The dialect rows and the corrected Week 31
  hand-forward note landed in `aff022b`. What was still stale and is now fixed:
  the **Limitations** section still described *every* uncorrelated subquery as
  materializing and still claimed the 1024-distinct-value cap. It now names the
  two productions and which shape picks which. Added the **Week 33 starting
  note** (the `ColumnId` trigger fires there and before the feature work; both
  Week 30 tripwires still armed; decorrelation needs no new operator; the
  Volcano refusal is the thing to reconsider) and a **Week 37 starting note**
  (the selection-vector deferral, with the shape of the measurement that decides
  whether to convert, plus the projection-pushdown and `frac = 1.0` effects that
  would otherwise read as regressions in the plots).

### Not done — the next concrete steps, in order

1. **Run the `verify` gate.** Build + `compare_against_sqlite.py` + the
   regression harness in all modes. The **C++ unit tests are green (768)** and
   the `--explain-analyze` invariant is checked, but the four-mode SQLite diff
   and the regression harness have not been run against the new suites. This is
   the only remaining gate on the week.
2. **§7's option (a)** — semi/anti in Volcano's `HashJoinNode` for a
   single-relation `FROM`. Still the honest end state; the refusal shipped is
   the interim. Until it lands, every `IN`-subquery query is diffed in two modes
   rather than four, which is a real reduction in the oracle's power on exactly
   the feature the week added. `HashJoinNode` is the semantic reference, so
   implement it there and check the vectorized operator against it.
3. **§6's selection-vector path.** Deferred to Week 37 by §6's own allowance and
   now recorded as a Week 37 starting note. Do not ship both paths.

### Measurements worth carrying forward

- `SELECT COUNT(*) FROM laps WHERE lap_id IN (SELECT lap_id FROM laps)` —
  the checkpoint query, and the one Week 31 could not answer at all (10 000
  distinct values against a 1024 cap; ~1e8 `Value` comparisons under the linear
  `InExpr` scan). It now answers in ~124 ms end to end, of which the semi-join
  is ~104 ms (84%). That 104 ms is the **row-copy path**: every one of the
  10 000 probe rows survives and is copied into `output_buffer_`, which is
  exactly the worst case for the deferral in §6 and the number Week 37 should
  measure against.
- The semi/anti estimate falls back to `frac = 1.0` on every real query, because
  the body plan's `LogicalProject` empties its `StatsContext` and the right-side
  NDV lookup misses. Conservative and invariant-preserving — and the reason the
  rule's arithmetic is exercised only by hand-built nodes in
  `tests/test_cardinality.cc`.
