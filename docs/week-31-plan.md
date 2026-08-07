# Week 31 — Scalar + Uncorrelated Subqueries

> Teaching plan. Read the Design section before writing code: this week deletes a
> refusal that six groups of slot consumers depend on for their safety, and the
> whole question of how much of that containment has to be replaced is decided by
> which execution model you pick. Pick the model first.

README checkpoint:

- Execute scalar subqueries and materialized uncorrelated subqueries
- Validate scalar cardinality at runtime

**Checkpoint: Uncorrelated TPC-H subqueries execute correctly.**

Plus the block addressed to this week: *Starting notes, from Week 30's
foundations* (README, immediately above the Week 31 section). Its first four
bullets are blockers, not advice, and every one of the twelve is answered
explicitly in [The Starting notes, and where each is answered](#the-starting-notes-and-where-each-is-answered).

---

## The inventory this checkpoint is measured against

What exists after Week 30 (`git log`, `src/`):

| Piece | State |
|---|---|
| `SubqueryExpr` (`parser/ast.h`) | Three kinds (`SCALAR` / `EXISTS` / `IN`), `negated`, the `IN` form's `operand` (enclosing scope), `shared_ptr<SelectStatement> subquery`, binder-set `correlated` |
| Parser | All three productions parse, in `WHERE` and `HAVING` only |
| `Binder` | Scope chain, innermost-first, walks out; stamps `(query_level, relation_slot)`; idempotent; sets `stmt.has_subquery`; `bindQuery` returns whether the body correlated |
| `Validator` | Position rule, arity rule, nested query validated in its own scope, correlated refs skipped — then ONE refusal at the end of `Validator::validate` on `stmt.has_subquery` |
| Dispatch sites | All eighteen handle `SubqueryExpr`; **ten of them are handled but unexercised**, because the refusal keeps the node out of every logical plan |
| Two tripwires | `collectSimplePredicates` (`storage/chunk_pruner.h`) **declines** a `query_level > 0` ref; `buildAggregateSchema` (`planner/logical_plan.cc`) **throws** on one |
| `buildScanSchema` | Returns the full schema for any statement with `has_subquery` — no projection pushdown for subquery queries |
| Sites 12 / 13 | `inferExprType` and `evaluate` throw `"...not yet executable (Week 31)"` |
| Harness | `WEEK30_SUBQUERY_BINDS` (24 queries) asserts the refusal is *reached*; `WEEK30_REJECTED_QUERIES` asserts every real defect outranks it |

Nothing executes. Reaching the refusal is currently the assertion.

## What Week 31 must deliver

1. An **uncorrelated** scalar subquery produces a value, and that value reaches
   the outer predicate — in all four modes.
2. An **uncorrelated** `EXISTS` / `NOT EXISTS` and `IN` / `NOT IN` produce the
   right rows — in all four modes.
3. A scalar subquery returning more than one row is a clean, data-dependent
   error, not a wrong answer.
4. A **correlated** subquery is still refused, with a message that names Week 33.
5. The containment Week 30 built is still true afterwards, and `development.md`
   says so with evidence rather than by omission.

## What not to build

- **No semi-join or anti-join operator.** Week 32. `IN` / `EXISTS` execute this
  week by materialization, which is precisely the thing Week 32 replaces for the
  set-membership forms.
- **No decorrelation, no per-outer-row execution, no `SubPlan` operator.**
  Week 33.
- **No `FROM (subquery)`.** Week 34; the parser does not accept it.
- **No `ColumnId { level, slot }` refactor.** Deferred by decision; see
  [D3](#d3--the-containment-audit-the-mandatory-part). This week does not lower a
  correlated reference, so it is not the week that owes it.
- **Do not delete either tripwire.** Neither becomes reachable this week. See D3.
- No TPC-H data (Week 35), no benchmark table (Week 37).

## Prerequisite knowledge

Flagged because a wrong mental model here produces a plausible-looking wrong
answer rather than a compile error:

- **SQL three-valued logic, and specifically `NOT IN` against a set containing
  NULL.** `x NOT IN (1, NULL)` is never TRUE — it is FALSE when `x = 1` and
  UNKNOWN otherwise. This is the single most common subquery bug in real
  engines. Task 4 turns it into two lines, but only if you can state the rule.
- **Loop invariance.** "Uncorrelated" means the subquery references no relation
  of any enclosing block, therefore its value does not depend on the outer row,
  therefore it can be computed once. That single sentence is the entire
  justification for this week's design.
- The engine's **boolean-as-INT** convention (a predicate infers to `TypeId::INT`;
  non-zero is true) and the fact that **a filter treats UNKNOWN and FALSE
  identically** — both are load-bearing in Task 4.
- Week 24's **constant folding** rationale (`planner/constant_folding.h`): three
  fast paths pattern-match on `ColumnRef op Literal`. Measured 203 ms → 0.35 ms.
  This week is the same argument applied to a new kind of constant.
- `development.md` → *Relation slots and query levels* (read in full) and
  *Extending the expression language* (the eighteen sites, and the rule a
  query-bearing node established: descend into the parts written in **this**
  block, never into the body).

---

## Design

### D1 — Two ways to execute an uncorrelated subquery, and why this codebase takes the constant one

**Option A — `SubPlan` at execution time.** The `SubqueryExpr` survives binding,
validation, logical planning and lowering. Each engine's expression evaluator
gains a case that owns a child plan, runs it on first evaluation and caches the
result. This is what Postgres does, and it is the shape Week 30's hand-forward
note anticipated ("both sites must close in the same commit that lowers one").

What it costs here:

- Sites 12, 13, 15 and 16 all become live in one commit, in two engines.
- The node reaches `PredicatePushdown`, `JoinEnumeration`, `CardinalityEstimator`
  and both plan builders — i.e. it arms **all ten** of the unexercised dispatch
  sites and every "contained by the refusal" row of the slot-consumer table at
  once, in the same week that also has to make the feature work.
- `WHERE speed > (SELECT AVG(speed) FROM laps)` stays a `ColumnRef op <opaque
  node>` predicate: no zone-map pruning, no `scanColumn` tight loop, no range
  selectivity. Week 24 measured that shape at 203 ms against 0.35 ms.

**Option B — materialize, then substitute (recommended).** Run the uncorrelated
body once, before planning the outer query, and replace the `SubqueryExpr` in the
outer AST with a constant:

```
WHERE speed > (SELECT AVG(speed) FROM laps)      ->  WHERE speed > 312.4471
WHERE EXISTS (SELECT * FROM drivers WHERE age>90) ->  WHERE 0
WHERE driver_id IN (SELECT driver_id FROM laps)   ->  WHERE driver_id IN (1,2,...,20)
```

Why B is the right fit for *this* codebase, in order of weight:

1. **Uncorrelated means loop-invariant, and this engine already has a pass whose
   entire purpose is turning a loop-invariant subtree into a `Literal` so that
   the three fast paths can see it.** B is constant folding with a new source of
   constants. A is constant folding refused.
2. **It keeps Week 30's containment intact for one more week.** After
   substitution the outer statement contains no `SubqueryExpr` at all, so the ten
   unexercised sites stay unexercised, both tripwires stay unarmed, and the
   second half of the slot-consumer table is still contained — by a *different*
   argument, which D3 states and which must be written into `development.md`.
3. **The body is planned as an ordinary top-level query.** Every ref inside it is
   `query_level 0` against its own range table, so every slot consumer in the
   sub-plan is correct for the same reason it is correct for `SELECT ... FROM
   laps`. No new numbering domain reaches any consumer.
4. **One implementation, four modes.** The pass sits above both engines, so
   Volcano and vectorized agree by construction — the property Week 30 bought by
   putting one refusal at the end of `Validator::validate` and which Week 29
   spent an audit round re-establishing.
5. `IN` collapses onto Week 25's `InExpr`, which already hashes its set once at
   compile time, has a vectorized kernel, and has an `IN`-specific selectivity
   rule (`k/ndv`). No new node, no new kernel, no new estimator rule.

**What B costs, stated up front — do not discover these in review:**

| Cost | Detail |
|---|---|
| `--explain` executes the subquery | The README says `EXPLAIN` "prints the query plan tree without executing". For a subquery query it must now run the nested query to know the constant. Same as constant folding, which `--explain` also performs. Document it in Limitations |
| Timing attribution | Subquery execution lands in `--explain-analyze`'s **Plan:** time, not **Execution:**. Honest, but Week 37 must know |
| Table copies | The nested query needs its own copy of the tables it scans (both scan nodes take a `ColumnarTable` / `vector<Row>` **by value**). `Lowering`'s `scan_uses` counter already copies for a self-join, so this is an existing pattern, not a new one — but it is a real memory cost, noted in Limitations |
| The `IN` set is fully materialized | Bounded by a cap (Task 4). Week 32's semi-join removes the need |
| Two textually identical subqueries run twice | Identity is the statement pointer (D4). A structural key is a Week 37 nicety, not correctness |

**Is B throwaway work?** No. Week 32 replaces the *lowering of the two
set-membership forms*, not the scalar path; the scalar materialization survives
into Week 33 and beyond as the uncorrelated fast path (every real optimizer keeps
it — an uncorrelated `InitPlan` is not a semi-join). Week 33's correlation is
a different mechanism entirely (per-outer-row values), which is why it is a
different week.

### D2 — What "uncorrelated" buys, and the refusal that replaces the blanket one

A correlated subquery's value depends on the outer row. It is not a constant, so
it cannot be substituted for one, so it is refused — until Week 33. That makes
the refusal's condition change from *"the statement has a subquery"* to *"the
statement has a **correlated** subquery"*, in place, at the end of
`Validator::validate`. Same site, same by-construction agreement across four
modes, new message naming Week 33.

**The subtlety that will bite you.** `SelectStatement::has_subquery`'s comment
says "Any statement containing a subquery at any depth contains one DIRECTLY, so
the top-level flag is always the right test". That is true for *containment* and
**false for correlation**. Consider Q20's shape:

```sql
SELECT name FROM drivers d WHERE d.driver_id IN
  (SELECT l.driver_id FROM laps l WHERE l.speed >
     (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))
```

The inner subquery is correlated **to the middle block**, not to the top one. Its
`correlated` flag is set on the inner node, and the top-level node's flag is
`false`. A top-level test on `sq->correlated` alone would accept this query, then
try to run the middle block, whose own `Validator::validate` would refuse it —
correct message, but arrived at after executing whatever was materialized on the
way down, and reported from a depth the user cannot see.

The containment is one line in the Binder: mark the enclosing statement when the
node is correlated **or when the body itself contains a correlated subquery**.
Then the outermost statement's flag means "this query contains a correlated
subquery at any depth", the refusal fires once, at the top, before anything runs.

### D3 — The containment audit (the mandatory part)

Read `development.md` → *Relation slots and query levels* before touching
`Validator::validate`. This section is the audit that document asks for, done for
option B.

**The claim to prove:** after this week, a `relation_slot` carrying a non-zero
`query_level` still cannot reach any consumer in the table's second half.

**The proof, in three steps:**

1. The only producer of `query_level > 0` is `Binder::resolveColumnRef` walking
   out of a scope — i.e. a correlated reference.
2. A statement containing a correlated reference (at any depth, after D2's
   propagation) is refused by `Validator::validate` before either planner builds
   anything.
3. An *uncorrelated* body is handed to `LogicalPlanBuilder::build` /
   `Planner::plan` as a top-level statement. Its refs were bound in its own
   scope, so they carry `query_level == 0` and slots that index its own range
   table. There is exactly one range table in play for that plan.

Therefore every consumer in the second half of the table sees level-0 refs, from
one range table, exactly as it does today — but the reason has changed from
"there is a blanket refusal" to "a correlated ref is refused, and an uncorrelated
body is its own top-level query". **Write that sentence into the table's preamble.
It is now the containment.**

Consumer-by-consumer, for the second half of the table:

| Consumer | Reachable with a level > 0 ref this week? | Why |
|---|---|---|
| `collectSimplePredicates` / `ChunkPruner::shouldSkip` | **No** | Its guard (decline on `query_level > 0`) stays a tripwire. The sub-plan's own hints are level-0 refs against the sub-plan's own scan |
| `buildAggregateSchema` (+ `HashAggregateNode`, `VecHashAggregateNode`, `CardinalityEstimator` via `GroupByColumn`) | **No** | Its guard (throw) stays a tripwire. A correlated `GROUP BY` key only exists inside a correlated subquery |
| `restampSlots` (site 9) | **No** | Its independent proof (`collectSlots` → `-1` → `soleSlot` → not pushed) is untouched; and after substitution there is no `SubqueryExpr` for pushdown to meet at all |
| `AggregateSpec::relation_slot` | **No** | Same argument as `buildAggregateSchema` |
| `JoinKey::from_slot` | **No** | `classifyJoinCondition` still refuses to build a key from a `level > 0` operand |
| `inferExprType` / `buildProjectSchema` / `collectCols` / `resolveColumnIndex` / `CardinalityEstimator` / `Planner::plan` / `VectorizedPlanBuilder` | **No** | All resolve against a plan schema built for one block; the sub-plan is one block |

**Both tripwires stay. Do not replace them with "real behaviour" and do not
delete them.** Week 30's note says to replace a tripwire *when this week makes it
reachable*; this week does not. Understand why the two differ before you go near
either:

- `ChunkPruner` **declines** because a pruning hint is an optimization: a rule
  that contributes nothing is correct-and-slower, and its failure mode
  (`relation_slot < 1` read as scan-local, then matched **by name** against the
  scanned table's zone maps) is silent chunk skipping on the `--no-optimize`
  path, where the `collectSlots`/`soleSlot` containment never applies.
- `buildAggregateSchema` **throws** because grouping is not an optimization and a
  correlated group key has no correct local fallback: `indexOf("team", 0)`
  against a subquery's `drivers` child schema is a clean **hit on the wrong
  relation**, so neither the bare-name fallback nor the `idx < 0` throw fires.
  Wrong groups, no error, in both engines.

The rule to carry forward: **decline where the consumer is an optimization, throw
where it is semantics.** That is the same rule as sites 14–17 ("prefer decline
and fall back over assume and guess") and it is what tells Week 32/33 which shape
their own guards should take.

**`ColumnId { level, slot }` — not this week, and here is the explicit statement
the brief asks for.** The structural change is to be done as its own standalone
change in whichever of Weeks 31 / 32 / 34 first lowers a correlated reference.
**Week 31 does not lower a correlated reference**: correlated subqueries remain
refused, and every reference this week's plans contain is level 0. So the
prerequisite does not trigger, and folding an 87-mention refactor into a feature
week would be exactly the thing the decision forbids. The trigger condition,
restated for whoever reads this next: *the first week in which a `ColumnRef` with
`query_level > 0` reaches a logical plan node.* On the current schedule that is
Week 33 (correlation); Week 32 will trip it only if it chooses to lower a
correlated `EXISTS` to an anti-join, which is Week 32's call to make and, if made,
its prerequisite to pay first.

One more forward note answered: `JoinEnumeration`'s silent decline for a relation
slot outside the range table (`e.slot_a >= n`) does **not** become live this week
either. Week 28/30 expected "Week 31/34 make it reachable" on the assumption that
a subquery's scans would join the outer query's tree. Under option B they do not —
the body is its own plan with its own range table. Leave the decline alone and
leave its comment's forward reference to Week 34 (derived tables), which is where
a nested scan genuinely does enter the outer tree.

### D4 — One statement, two parents

Week 30 handed this week an explicit decision: *"Week 31 must decide whether two
`SubqueryExpr` nodes over one statement build one subplan or two."*

**Decision: one run, cached on the statement's address.** `cloneExpr` shares the
`shared_ptr` (because `SelectStatement` is move-only and a deep-copy walker's
omissions would be silent), so two nodes over one statement is a real state —
`(SELECT MAX(age) FROM drivers) BETWEEN 1 AND 99` produces exactly it, because
`BETWEEN` clones its left operand before binding. The cache key is
`sq->subquery.get()`, which is the same identity `exprKey` already uses for a
subquery (`expr_utils.h`: `"@" + address`), so the two cannot disagree.

Cache the **rows**, not the substituted expression: the `IN` form's `operand`
belongs to each node individually, so each node builds its own replacement from
shared rows.

This also makes it safe to `std::move(*sq->subquery)` into the planner (which
takes its statement by value and moves expressions out of it). The moved-from
statement is left empty, and that is only safe **because the cache is consulted
first**. State that invariant in the code; it is what a future reader needs in
order not to add a third consumer of the statement after materialization.

### D5 — The first typeless NULL constant in the engine

`constant_folding.h` says, as a documented premise:

> A fold that evaluates to NULL (`1 / 0`) is skipped: there is no NULL literal in
> the grammar, and a `Literal` holding a null `Value` has no `type()` for
> `inferExprType` to report.

An empty (or NULL-valued) scalar subquery is exactly what breaks that premise:

```sql
SELECT team FROM laps WHERE speed > (SELECT speed FROM laps WHERE lap_id = -1)
-- SQL: the scalar is NULL, the comparison is UNKNOWN, zero rows
```

Substituting `Literal(Value::null())` makes `inferExprType` throw *Cannot get
type of null Value* at plan time on a query SQLite answers. Three facts decide
the fix:

- `Value` has no typed null (`Value::type()` throws when `is_null_`), and giving
  it one is a Layer-1 change touching every module.
- `ExpressionExecutor::compileNode` **already declines a null `Literal`**
  (`expression_executor.cc`: "a null Value has no type() to broadcast. Decline
  rather than invent one"), so the vectorized path falls back to `evaluate()` for
  that predicate — correct-but-slow, the documented pattern, and the reason no
  kernel needs to learn anything.
- `foldNode` already refuses to *produce* a null literal and, because arithmetic
  over one evaluates to NULL, declines to fold *through* one.

So the only gap is the type, and **the type is known at the substitution site** —
it is the subquery plan's output column type. Recommended: a defaulted field on
`Literal` (`TypeId null_type = TypeId::INT`), set only when the value is null,
read only by `inferExprType`. Alternative considered: `inferExprType` returns
`TypeId::INT` for any null literal. It is one line shorter and it is wrong in a
reachable place — `SUBSTRING((SELECT name FROM drivers WHERE driver_id = -1), 1, 2)`
would fail plan-time typing with *SUBSTRING requires a STRING operand* on a query
whose answer is NULL. Do not invent a type you already have.

### D6 — Where the pass runs, and why the order is fixed

```
parse -> bind (scopes, idempotent stamps, foldConstants)
      -> LOAD tables, now including every table a nested query names   (Task 5)
      -> Validator::validate(stmt, catalog)                            (Task 5)
      -> materializeSubqueries(stmt, runner)                           (Tasks 2-4)
      -> [existing] LogicalPlanBuilder::build / Planner::plan -> optimize -> run
```

Three orderings are load-bearing:

1. **Validation before materialization.** The pass *trusts* three Validator
   rules: exactly one output column for `SCALAR` / `IN`, position restricted to
   `WHERE`/`HAVING`, and no correlated subquery. Run the pass first and
   `WHERE speed > (SELECT speed, team FROM laps)` silently materializes column 0
   of a query the Validator was about to reject. `Validator::validate` is pure,
   so calling it here and again inside the planner costs one extra walk and buys
   the ordering discipline Week 30 established: every parse, bind and validate
   error a query is entitled to fires before an engine limitation.
2. **Materialization before planning and before the optimizer.** The optimizer
   then sees ordinary literals, so `optimized ≡ --no-optimize` (invariant 11)
   holds for the same reason it does for any constant, and pushdown/pruning/
   selectivity get the folded shape.
3. **The pass is above both engines, not inside each planner.** One
   implementation, four modes by construction. `Planner::plan` and
   `LogicalPlanBuilder::build` keep their signatures, so every existing test and
   both engines' entry points are untouched — the alternative (threading a runner
   through both planners) changes two public signatures and every test that calls
   them.

Invariant 5 ("planner performs no I/O") is preserved: the pass is not the
planner, and it does no I/O either — `main.cc` loads the data, as it does today,
and hands the pass a callback that plans and runs a statement over data already
in memory.

---

## The Starting notes, and where each is answered

Week 30's block, bullet by bullet. Nothing here is left implicit.

| Note | Answer |
|---|---|
| **The slot-consumer enumeration is the containment — read it before deleting the refusal** | D3. Audited consumer by consumer; the containment survives, with a new stated reason. Task 7 rewrites the preamble and adds the rows this week creates |
| **`ColumnId { level, slot }` is deferred by decision** | D3. Week 31 lowers no correlated reference, so the prerequisite does not trigger. Trigger condition restated for the next week |
| **Two tripwires, which Week 31 will be the first to arm** | D3. It is not. Neither becomes reachable; both stay, both keep their asymmetry (decline vs throw) and their comments gain a line saying Week 31 checked and did not arm them |
| **`GroupByColumn` carries a level and every consumer ignores it** | Contained, unchanged: a correlated group key exists only inside a correlated subquery. `AggregateSpec` likewise |
| **A shared subquery statement is one statement with two parents — decide one subplan or two** | D4. One run, cached on the statement address, the same identity `exprKey` uses. Task 3 |
| **`collectSlots` gives a correlated subquery `-1`; land the precise set with Week 33** | Untouched. After substitution there is no `SubqueryExpr` left for pushdown to classify, so the conservative value costs nothing this week |
| **`buildScanSchema` widens for any statement with a subquery — expect it in the first benchmark** | Task 3 clears `has_subquery` once every node has been substituted, so the outer query gets its projection pushdown back and the surprise never happens. The flag keeps its meaning: "a `SubqueryExpr` is still in this tree" |
| **`inferExprType` and `evaluate` (12/13) must close in the same commit that lowers one** | Task 6, both in one commit. Under option B neither ever sees a `SubqueryExpr` from the CLI, so the honest closure is a reworded internal-error throw plus the null-`Literal` type rule that substitution actually creates (D5) |
| **Ten of the eighteen dispatch sites are handled but unexercised; the week that deletes the refusal inherits all ten** | Re-read done (D3). Option B does not arm them: the substituted tree contains no `SubqueryExpr`. The new walker is site **19**, and Task 2 registers it |
| **`JoinEnumeration` declines any tree carrying a slot outside the range table; Week 31/34 make that reachable** | D3. Not this week — the body is its own plan with its own range table. Leave the decline; the forward reference belongs to Week 34 |
| **The refusal masks a plan-time type error in the same query; do not add a second refusal site per engine** | Preserved: still one site, still at the end of `Validator::validate`, only the condition and the message change |
| **The moved `SUM`/`AVG` type check runs during binding and outranks every Validator rule** | Unchanged. Nothing this week moves a check into or out of the Binder |

---

## Task 1 — Narrow the refusal to correlated subqueries

### Why it matters

This is the single line that decides what the rest of the week may assume. Every
"contained by the refusal" row in `development.md`'s slot-consumer table, both
tripwires, and `JoinKey`'s and `AggregateSpec`'s stated contracts rest on it. Its
new condition is the *new* containment, so it has to mean exactly "a `ColumnRef`
with `query_level > 0` exists somewhere in this query" — no more, no less.

Downstream: the pass in Task 3 may then assume every `SubqueryExpr` it meets is
uncorrelated, so it never has to ask a scope question, which is why it does not
need to be level-aware and therefore does not need a row in the *reachable* half
of the slot table.

### Conceptual explanation

Correlation is *per node* (`SubqueryExpr::correlated`), set by `bindQuery`'s
return value. The refusal is *per statement*, at the top. Those are two different
granularities and the bridge is a statement flag, propagated **upward** — because
correlation is relative to a block, so a node that is correlated to a middle
block leaves the top block's nodes uncorrelated (D2).

Note the asymmetry with `has_subquery`, which needs no propagation: containment
is trivially transitive at the top ("any statement containing a subquery at any
depth contains one directly"), correlation is not.

### Code

`src/parser/ast.h`, beside `has_subquery`:

```cpp
    // Week 31. Set by the Binder when THIS statement contains a correlated
    // SubqueryExpr *or* when one of its subqueries does. Propagated UPWARD,
    // unlike has_subquery: correlation is relative to a block, so a node
    // correlated to a MIDDLE block leaves the top block's node uncorrelated
    // (Q20's shape). Without the propagation the top-level refusal accepts the
    // query and the middle block refuses it later, after work has been done.
    bool has_correlated_subquery = false;
```

`src/planner/binder.cc`, in the `SubqueryExpr` branch of `bindExpr` (dispatch
site 3), replacing the single `has_subquery` line's neighbourhood:

```cpp
        if (scope.stmt) scope.stmt->has_subquery = true;

        // (unchanged) the body opens a new scope whose parent is this one
        if (sq->subquery) sq->correlated = bindQuery(*sq->subquery, catalog, &scope);

        // Week 31. AFTER bindQuery, which is what sets both inputs: this node's
        // own correlation, and the body's flag for anything correlated deeper
        // in. Reading it before would see a default-constructed false.
        if (scope.stmt && sq->subquery
            && (sq->correlated || sq->subquery->has_correlated_subquery)) {
            scope.stmt->has_correlated_subquery = true;
        }
```

`src/planner/validator.cc`, end of `Validator::validate` — same site, new
condition, new message. Keep the whole comment block above it (it explains the
placement, which is unchanged) and amend the first paragraph:

```cpp
    // Week 30, narrowed in Week 31. Still ONE check, still LAST, still shared by
    // both planners — the four modes agree by construction, not by two guards
    // that can drift.
    //
    // What changed: an UNCORRELATED subquery is loop-invariant, so
    // materializeSubqueries computes it once and substitutes a constant before
    // either planner runs (subquery_materialization.h). A CORRELATED one has a
    // different value per outer row; that is decorrelation, and it is Week 33.
    //
    // This condition is also the containment that development.md's
    // slot-consumer table now rests on: a ColumnRef with query_level > 0 exists
    // only inside a correlated subquery, so refusing those keeps every consumer
    // in that table's second half on level-0 refs from ONE range table.
    if (stmt.has_correlated_subquery) {
        throw std::runtime_error(
            "correlated subqueries are not yet executable (Week 33)");
    }
```

### Implementation guidance

1. Do **not** add a walker to find correlated nodes. The Binder already visits
   every one of them and is the only layer holding the scope chain; a second
   walker would be a nineteenth silent dispatch site answering a question the
   Binder can answer for free.
2. Order inside the branch matters: `bindQuery` must run before the propagation
   test, or `sq->subquery->has_correlated_subquery` is read before it is set.
3. `Binder::bind` is called once per statement from `main.cc`, but binding is
   idempotent and a shared statement can be walked twice — setting a bool to
   `true` twice is idempotent, so nothing further is needed.
4. Leave `has_subquery` alone here. It keeps its meaning ("a `SubqueryExpr` is in
   this tree") and Task 3 clears it after substitution.
5. Gotcha: `Validator::validate` is called by both planners *and* (from Task 5)
   by `main.cc`. It must stay pure — no mutation of `stmt` — or the second call
   changes behaviour.

### Verification

```bash
# still refused, and by the new week
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT l.lap_id FROM laps l WHERE l.speed > (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)"
# Error: correlated subqueries are not yet executable (Week 33)

# Q20's shape: correlated to the MIDDLE block. Must give the SAME message,
# reported at the top, not from inside a nested run
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT name FROM drivers d WHERE d.driver_id IN (SELECT l.driver_id FROM laps l WHERE l.speed > (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))"
# Error: correlated subqueries are not yet executable (Week 33)

# the uncorrelated ones must now get PAST the validator (they will fail further
# down until Task 3 lands — that is the expected intermediate state)
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)"
```

Unit test to add (`tests/test_binder.cc`): bind Q20's shape and assert
`stmt.has_correlated_subquery` on the **outermost** statement. That assertion is
the propagation; without it the bug is invisible until a nested run misreports.

---

## Task 2 — One walker for subquery nodes (dispatch site 19), and the tables a nested query needs

### Why it matters

Two jobs need to find every `SubqueryExpr` in a statement: the rewrite (Task 3)
and `main.cc`'s loader, which must load and stat the tables a *nested* query
scans — today it walks `stmt.from_table` and `stmt.joins` only, so
`WHERE x IN (SELECT driver_id FROM drivers)` on a `FROM laps` query has no
`drivers` data in memory at all.

Two private copies of one walker is precisely the defect `collectSlots` was
promoted to a shared, declared walker to prevent ("one walker, now THREE callers,
never a private copy"). Follow that precedent from the start.

### Conceptual explanation

This is dispatch site **19**, and it is the first walker that deliberately
**descends into the body**. Week 30 established the opposite rule ("descend into
the parts written in this query block, never into the body"), and the rule is
right — for *scope* questions. Name resolution, slot routing, aggregate
collection and grouping are all scope questions, and the body is a different
scope. Materialization is not a scope question: it is "which statements must be
executed, innermost first". So the rule's justification does not apply, and the
walker must state that it is a deliberate exception rather than look like an
oversight.

Failure mode if the walker misses a subtype: the `SubqueryExpr` is never
substituted, survives into planning, and hits `inferExprType` (site 12) — which
**throws**. Loud, at plan time, naming the site. That is the whole reason Task 6
keeps sites 12 and 13 throwing rather than deleting them: they are this walker's
backstop, and they are what makes site 19 a *loud* site rather than the eleventh
silent one.

### Code

`src/planner/subquery_materialization.h` (new file):

```cpp
#pragma once

#include "parser/ast.h"
#include "common/schema.h"      // Row, Schema
#include <functional>
#include <string>
#include <vector>

// DISPATCH SITE 19 (development.md -> Extending the expression language).
//
// Visits every SubqueryExpr in an expression tree, INNERMOST FIRST, handing the
// caller the owning slot so it can be replaced in place.
//
// This is the one walker that descends INTO the body, and that is deliberate.
// Week 30's rule for a query-bearing node — descend into the parts written in
// THIS block, never into the body — is a rule about SCOPE: the body has its own
// schema, range table and aggregate rule, so a scope question must not cross the
// boundary. "Which statements must be executed, and in what order" is not a
// scope question. A nested body must be materialized before the body that
// contains it can run.
//
// A missed Expr subtype here is LOUD, not silent: the node survives into
// planning and inferExprType (site 12) throws. Keep that throw.
void forEachSubquery(std::unique_ptr<Expr>& expr,
                     const std::function<void(std::unique_ptr<Expr>&)>& fn);

// Every table any query in this statement scans, including nested ones, in no
// particular order and without duplicates. main.cc loads and stats these; the
// FROM/joins walk it used before Week 31 misses every nested table.
void collectQueryTables(const SelectStatement& stmt, std::vector<std::string>& out);
```

`src/planner/subquery_materialization.cc`:

```cpp
void forEachSubquery(std::unique_ptr<Expr>& expr,
                     const std::function<void(std::unique_ptr<Expr>&)>& fn) {
    if (!expr) return;

    // Every container subtype, exactly as sites 4/8/14 enumerate them. A new
    // Expr subtype added later and missed here means its subqueries are never
    // materialized -> site 12 throws. Loud, but still add the branch.
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr.get())) {
        forEachSubquery(bin->left, fn);
        forEachSubquery(bin->right, fn);
    } else if (auto* isn = dynamic_cast<IsNullExpr*>(expr.get())) {
        forEachSubquery(isn->operand, fn);
    } else if (auto* un = dynamic_cast<UnaryExpr*>(expr.get())) {
        forEachSubquery(un->operand, fn);
    } else if (auto* agg = dynamic_cast<AggregateExpr*>(expr.get())) {
        if (!agg->is_star) forEachSubquery(agg->argument, fn);
    } else if (auto* in = dynamic_cast<InExpr*>(expr.get())) {
        forEachSubquery(in->operand, fn);     // values are literals
    } else if (auto* lk = dynamic_cast<LikeExpr*>(expr.get())) {
        forEachSubquery(lk->operand, fn);
    } else if (auto* c = dynamic_cast<CaseExpr*>(expr.get())) {
        for (auto& w : c->when_clauses) {
            forEachSubquery(w.condition, fn);
            forEachSubquery(w.result, fn);
        }
        forEachSubquery(c->else_expr, fn);
    } else if (auto* sub = dynamic_cast<SubstringExpr*>(expr.get())) {
        forEachSubquery(sub->operand, fn);
        forEachSubquery(sub->start, fn);
        forEachSubquery(sub->length, fn);     // nullptr-safe
    } else if (auto* sq = dynamic_cast<SubqueryExpr*>(expr.get())) {
        // the IN operand belongs to THIS block and may itself hold a subquery
        forEachSubquery(sq->operand, fn);
        // INNERMOST FIRST: the callback may run this body, and a body cannot run
        // while it still contains an unmaterialized subquery of its own. The
        // body's own statement-level clauses are visited by the caller, which
        // recurses through materializeSubqueries(*sq->subquery, ...).
        fn(expr);                              // `expr` may be replaced here
        return;                                // it is no longer a SubqueryExpr
    }
    // ColumnRef / Literal / IntervalLiteral: nothing to visit
}
```

`collectQueryTables` reuses it:

```cpp
static void collectFromOneStatement(const SelectStatement& stmt,
                                    std::vector<std::string>& out) {
    auto add = [&out](const std::string& t) {
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    };
    add(stmt.from_table);
    for (const auto& j : stmt.joins) add(j.join_table);
}

void collectQueryTables(const SelectStatement& stmt, std::vector<std::string>& out) {
    collectFromOneStatement(stmt, out);
    // const walk: forEachSubquery takes an owning slot, so use a local const_cast
    // -free helper — see the implementation note in the guidance below.
    forEachSubqueryConst(stmt, [&out](const SubqueryExpr& sq) {
        if (sq.subquery) collectQueryTables(*sq.subquery, out);
    });
}
```

### Implementation guidance

1. You need **two shapes** of the walker: a mutating one over
   `std::unique_ptr<Expr>&` (the rewrite needs the owning slot) and a read-only
   one over `const Expr*` (table collection). Write the const one as the primitive
   and the mutating one separately, or write one template — but do **not**
   `const_cast`. Two small functions in one file with one comment saying they must
   stay in step is acceptable and honest; a `const_cast` is not.
2. Both must also cover the statement-level clauses. Only `WHERE` and `HAVING`
   can hold a subquery (the Validator enforces it), but walking `select_list`,
   `group_by`, `order_by` and each join's `condition` as well costs four lines and
   means the walker does not silently depend on a rule enforced in another file.
   If you rely on the position rule instead, say so in a comment naming
   `validateExpr`'s `allow_subqueries` flag.
3. `collectQueryTables` must dedupe — a self-join names one table twice, and
   `main.cc`'s loader is keyed by table name (the existing loop already guards
   this).
4. Do not add the walker to `expr_utils.h`. It belongs beside the pass that owns
   it, exactly as `collectSlots` lives beside pushdown and is *declared* for its
   other callers.
5. Register it in `development.md`'s site table as site 19 in the same commit
   (Task 7). The doc says eighteen; leaving it stale is precisely the failure the
   Week 25 audit found ("the dispatch checklist was under-counted").

### Verification

Unit tests (`tests/test_logical_plan.cc` or a new `tests/test_subquery.cc`):

- `ForEachSubqueryFindsOneInsideEveryContainerNode` — build `WHERE CASE WHEN
  lap_id > (SELECT ...) THEN 1 ELSE 0 END = 1`, `WHERE SUBSTRING(team, (SELECT
  ...), 2) = 'x'`, `WHERE speed BETWEEN (SELECT ...) AND 9` (parse it, so the
  desugared clone is exercised) and assert the callback fires the expected number
  of times for each.
- `ForEachSubqueryVisitsInnermostFirst` — a two-deep uncorrelated nest; record
  the visit order and assert the inner body is visited before the outer node.
- `CollectQueryTablesFindsNestedTables` — `SELECT team FROM laps WHERE driver_id
  IN (SELECT driver_id FROM drivers)` yields `{laps, drivers}`.

---

## Task 3 — The materialization pass: run a body once, cache by statement identity

### Why it matters

This is the week's engine. It converts "a subquery is bound" into "a subquery has
a value", and it is the single site where the four modes are made to agree. It is
also where `has_subquery` is cleared, which is what returns projection pushdown
to subquery queries (Week 30's "first benchmark surprise" note).

### Conceptual explanation

The pass is a rewrite over the statement:

1. For each `SubqueryExpr` slot, innermost first:
2. If its body has not been run before (cache miss on `sq->subquery.get()`):
   recursively materialize the body's own subqueries, then hand the body to the
   **runner** and store `{Schema, rows}` in the cache.
3. Build the replacement expression from the cached result and the node's own
   `kind` / `negated` / `operand` (Task 4), and move it into the slot.
4. When every node in the statement has been replaced, clear `has_subquery` and
   re-run `foldConstants`.

The **runner** is injected, not called directly, for three reasons: the planner
layer must not depend on `main.cc`'s engine selection; the body must run on the
**same engine** as the outer query (running everything on Volcano would refuse a
three-relation body — TPC-H Q11's subquery joins three relations — in vectorized
mode); and an injected runner makes the whole rewrite unit-testable with canned
rows and no data at all.

Re-running `foldConstants` afterwards is one line and restores the
`ColumnRef op Literal` shape for `WHERE speed > (SELECT AVG(speed) FROM laps) * 2`.
It is safe by inspection: `foldNode` folds only arithmetic, declines any fold
that evaluates to NULL, and never folds a comparison.

### Code

Header additions (`src/planner/subquery_materialization.h`):

```cpp
// What one nested query produced. Schema comes from the sub-plan's output, and
// is what types an empty scalar result (see Literal::null_type).
struct SubqueryResult {
    Schema schema;
    std::vector<Row> rows;
};

// Plans and runs one already-bound, already-validated statement to completion.
// Supplied by main.cc, one implementation per engine, so a nested query runs on
// the SAME engine as the query that contains it — a three-relation body must not
// be refused in vectorized mode by being handed to Volcano.
using SubqueryRunner = std::function<SubqueryResult(SelectStatement)>;

// Replaces every SubqueryExpr in `stmt` with a constant.
//
// PRECONDITIONS (all established by Validator::validate, which main.cc calls
// first — see the ordering note in docs/week-31-plan.md D6):
//   - no correlated subquery anywhere in `stmt` (refused in Week 33's name)
//   - a SCALAR or IN body has exactly one output column
//   - a subquery appears only in WHERE / HAVING
// Violating any of them here is a wrong answer, not an error, which is why the
// pass asserts rather than re-checks: the check belongs to the layer that owns
// the diagnostic.
void materializeSubqueries(SelectStatement& stmt, const SubqueryRunner& run);
```

The pass itself:

```cpp
namespace {

// Identity is the STATEMENT ADDRESS, which is the same identity exprKey uses
// (expr_utils.h: "@" + address). cloneExpr SHARES the shared_ptr rather than
// deep-copying, so two SubqueryExpr nodes over one statement is a real state —
// `(SELECT MAX(age) FROM drivers) BETWEEN 1 AND 99` produces exactly it, because
// BETWEEN clones its left operand before binding. ONE run, two substitutions.
//
// Two textually identical but DISTINCT subqueries still run twice. That is a
// missed optimization, not a wrong answer; a structural key is Week 37's.
using ResultCache = std::unordered_map<const SelectStatement*, SubqueryResult>;

const SubqueryResult& runOnce(SubqueryExpr* sq, const SubqueryRunner& run,
                              ResultCache& cache) {
    auto it = cache.find(sq->subquery.get());
    if (it != cache.end()) return it->second;

    // Innermost first: a body cannot run while it still holds a subquery.
    materializeSubqueries(*sq->subquery, run);

    // Bound the work where the shape allows it. EXISTS needs one row; a scalar
    // needs two, because "more than one" is the cardinality error and two rows
    // prove it. Never widen an existing LIMIT.
    SelectStatement body = std::move(*sq->subquery);
    if (sq->kind == SubqueryExpr::Kind::EXISTS) {
        body.limit = body.limit ? std::min(*body.limit, 1) : 1;
    } else if (sq->kind == SubqueryExpr::Kind::SCALAR) {
        body.limit = body.limit ? std::min(*body.limit, 2) : 2;
    }

    // MOVING out of the shared statement is safe ONLY because the cache above is
    // consulted first: a second node over the same statement never reaches here.
    // Anything added later that reads *sq->subquery after this point reads an
    // empty statement.
    auto [pos, ok] = cache.emplace(sq->subquery.get(), run(std::move(body)));
    (void)ok;
    return pos->second;
}

void materializeInExpr(std::unique_ptr<Expr>& slot, const SubqueryRunner& run,
                       ResultCache& cache) {
    auto* sq = static_cast<SubqueryExpr*>(slot.get());
    const SubqueryResult& res = runOnce(sq, run, cache);
    std::unique_ptr<Expr> replacement = buildReplacement(sq, res);  // Task 4
    replacement->alias = slot->alias;   // empty in WHERE/HAVING, kept anyway —
                                        // foldNode preserves it for the same reason
    slot = std::move(replacement);
}

} // namespace

void materializeSubqueries(SelectStatement& stmt, const SubqueryRunner& run) {
    if (!stmt.has_subquery) return;          // the common case, one bool

    ResultCache cache;
    auto visit = [&](std::unique_ptr<Expr>& slot) {
        materializeInExpr(slot, run, cache);
    };
    forEachSubquery(stmt.where, visit);
    forEachSubquery(stmt.having, visit);
    // Position is WHERE/HAVING only (validateExpr's allow_subqueries), but walk
    // the rest too rather than depend silently on a rule enforced elsewhere.
    for (auto& e : stmt.select_list) forEachSubquery(e, visit);
    for (auto& i : stmt.order_by)    forEachSubquery(i.expr, visit);
    for (auto& j : stmt.joins)       forEachSubquery(j.condition, visit);

    // The flag means "a SubqueryExpr is still in this tree", and none is. This
    // is what gives the outer query its projection pushdown back:
    // buildScanSchema widens to the full schema whenever the flag is set.
    stmt.has_subquery = false;

    // A substituted constant may sit under arithmetic — `> (SELECT ...) * 2` —
    // and three fast paths pattern-match on ColumnRef op Literal. Folding only
    // touches arithmetic and declines any fold that evaluates to NULL, so this
    // cannot change a result. Same argument as constant_folding.h.
    foldConstants(stmt);
}
```

### Implementation guidance

1. **`materializeSubqueries` must early-return on `!has_subquery`.** Every query
   in the engine calls it; the flag makes it a single test for the 99% case.
2. **Do not** call `Validator::validate` from inside the pass. It belongs at the
   call site (Task 5), once, before the pass — calling it here would double the
   nested validation and put a diagnostic inside a rewrite.
3. The recursive call in `runOnce` operates on `*sq->subquery` **before** the
   move. Getting that order wrong (move first, then materialize the moved-from
   husk) is a silent no-op that leaves the body's own subquery in place and shows
   up as site 12's throw from inside a nested run.
4. `body.limit` for `EXISTS`: correct for `NOT EXISTS` too — negation is applied
   to *existence*, which one row settles.
5. `body.limit = 2` for `SCALAR` is what makes the cardinality check O(1) in
   memory instead of materializing a million-row "scalar". Do not use 1: you
   would silently accept a multi-row scalar, which is a wrong answer.
6. Gotcha: `LIMIT` is applied above `ORDER BY` in `LogicalPlanBuilder::build`, so
   a body with its own `ORDER BY` still returns the right first rows.
7. Gotcha: `foldConstants` also folds each `GroupByColumn::expr` through a clone
   and swaps it back. That is fine to re-run; it is idempotent for an
   already-folded tree.
8. Add `src/planner/subquery_materialization.cc` to `CMakeLists.txt`'s
   `swiftql_lib` source list, next to `constant_folding.cc`.

### Verification

Unit-test the pass with a **fake runner** — no catalog, no data, no CSV:

```cpp
TEST(SubqueryMaterialization, RunsASharedStatementExactlyOnce) {
    // "(SELECT MAX(age) FROM drivers) BETWEEN 1 AND 99" — BETWEEN clones its
    // left operand before binding and cloneExpr SHARES the statement, so two
    // SubqueryExpr nodes point at one SelectStatement.
    Parser parser("SELECT lap_id FROM laps WHERE "
                  "(SELECT MAX(age) FROM drivers) BETWEEN 1 AND 99");
    SelectStatement stmt = parser.parse();
    Binder::bind(stmt, catalog);

    int runs = 0;
    materializeSubqueries(stmt, [&](SelectStatement) {
        ++runs;
        return SubqueryResult{Schema({{"MAX(age)", TypeId::INT}}),
                              {Row{Value(int64_t(42))}}};
    });
    EXPECT_EQ(runs, 1);              // one statement, one run, two substitutions
    EXPECT_FALSE(stmt.has_subquery); // and the flag is cleared
}
```

Plus: `ClearsHasSubqueryOnlyWhenNothingIsLeft`, `MaterializesTheInnerBodyFirst`
(assert the runner receives a statement whose own `has_subquery` is already
false), and `RefoldsArithmeticAroundTheSubstitutedConstant` (assert the `WHERE`
is a `ColumnRef op Literal` after `> (SELECT ...) * 2`).

---

## Task 4 — The three substitution rules

### Why it matters

This is where SQL semantics live. Every other task is plumbing; this one decides
whether the answers are right. Two of the three rules have a NULL case that is
wrong in a way no test catches unless you write the test on purpose, and one has
an unbounded memory/time cost.

### Conceptual explanation

**SCALAR.** The subquery yields one row, one column.
- more than one row → error, *"scalar subquery returned more than one row"*. This
  is the checkpoint's "validate scalar cardinality at runtime": it is decidable
  only from data, and it fires when the body runs.
- zero rows → NULL (SQL says so; SQLite agrees).
- one row → that value, which may itself be NULL (`SELECT AVG(speed)` over an
  empty selection returns one NULL row).
- The replacement is a `Literal`. When the value is null it carries the type of
  the body's single output column — D5.

**EXISTS.** Only existence matters, never values (which is why the Validator
gives `EXISTS` no arity rule — Q4 and Q21 both write `SELECT *`). Replacement:
`Literal(int64_t(exists != negated))`, in the boolean-as-INT convention.

**IN.** Rewrite to Week 25's `InExpr` over the materialized column. Three things
must be decided, and two of them are NULL rules:

1. **Dedupe.** `evaluate()`'s `InExpr` case does a **linear scan of `values` per
   row** (`evaluator.cc`). On the shipped data `driver_id` has 20 distinct values
   out of 10 000 rows — deduping turns 10 000 comparisons per row into 20.
2. **Cap the set.** Even deduped, `WHERE lap_id IN (SELECT lap_id FROM laps)` is
   10 000 distinct values × 10 000 rows = 10^8 `Value` comparisons per Volcano
   mode, and the harness runs two of them. Refuse above a documented limit with a
   message naming Week 32, which replaces this path with a semi-join. Declining
   loudly beats hanging the correctness oracle. (The alternative — teaching
   `evaluate()`'s `InExpr` to hash — is a change to the semantic reference's hot
   path for one week's benefit.)
3. **The three-valued rule.** `InExpr::values` is documented as "non-empty, never
   NULL" precisely because the grammar has no NULL literal. A subquery result
   *can* contain NULLs, so that invariant is this week's to preserve, not to
   break. The rules:

| Shape | Materialized set | Replacement | Why |
|---|---|---|---|
| `IN` | any | `InExpr` over the **non-null** values | A match is TRUE; a non-match is FALSE without NULLs and UNKNOWN with them, and **UNKNOWN and FALSE are indistinguishable to every consumer reachable here** |
| `IN` | no non-null values | `Literal(0)` | `x IN ()` is FALSE; `x IN (NULL)` is UNKNOWN — same collapse |
| `NOT IN` | contains a NULL | `Literal(0)` | `x NOT IN (S ∪ {NULL})` is FALSE when x matches and UNKNOWN otherwise — **never TRUE**. This is the classic NOT-IN trap; getting it wrong returns rows SQLite does not |
| `NOT IN` | no NULLs, non-empty | `InExpr(values, negated=true)` | ordinary |
| `NOT IN` | empty (no rows at all) | `Literal(1)` | `x NOT IN ()` is TRUE, even for a NULL `x` |

**The UNKNOWN ≡ FALSE collapse must be justified, not assumed.** It holds because
of what the grammar allows, and only that:
- a subquery is legal in `WHERE` and `HAVING` only, so the predicate's ultimate
  consumer is always a filter, and a filter drops both UNKNOWN and FALSE;
- there is no general `NOT` (`development.md`, *Syntax Deliberately Not
  Supported*) — the only negations are `NOT IN` / `NOT LIKE` / `NOT BETWEEN` /
  `IS NOT NULL`, none of which can be applied to a predicate's *result*;
- `IS [NOT] NULL` parses at the *additive* level, so `(x IN S) IS NULL` does not
  parse;
- three-valued `AND`/`OR` differ between UNKNOWN and FALSE only in cases that a
  filter then drops identically;
- `CASE WHEN` treats UNKNOWN and FALSE alike (the branch is not taken).

**Write that list into the code.** If a general `NOT` is ever added, this
collapse is the first thing that breaks, and the comment is what will make that
findable.

### Code

```cpp
// Week 31. Builds the constant that replaces one uncorrelated SubqueryExpr.
// Precondition (Validator): SCALAR and IN bodies have exactly one output column.
std::unique_ptr<Expr> buildReplacement(SubqueryExpr* sq, const SubqueryResult& res) {
    switch (sq->kind) {

    case SubqueryExpr::Kind::EXISTS: {
        const bool exists = !res.rows.empty();
        return std::make_unique<Literal>(
            Value(static_cast<int64_t>(exists != sq->negated)));
    }

    case SubqueryExpr::Kind::SCALAR: {
        // CARDINALITY, the checkpoint's runtime check. Arity was decided at bind
        // time from the select list; this one needs data, and runOnce capped the
        // body at LIMIT 2 so seeing two rows is proof of "more than one".
        if (res.rows.size() > 1) {
            throw std::runtime_error("scalar subquery returned more than one row");
        }
        // Zero rows is NULL in SQL, and a one-row result may itself hold one
        // (SELECT AVG(speed) over an empty selection). Either way the type is
        // the body's single output column's — see Literal::null_type.
        if (res.rows.empty() || res.rows[0][0].isNull()) {
            auto lit = std::make_unique<Literal>(Value::null());
            lit->null_type = res.schema.column(0).type;
            return lit;
        }
        return std::make_unique<Literal>(res.rows[0][0]);
    }

    case SubqueryExpr::Kind::IN: {
        // Deduped, NULLs separated. evaluate()'s InExpr scans `values` LINEARLY
        // per row, so the dedupe is not tidiness: 20 distinct driver_ids against
        // 10000 rows is the difference between 2e5 and 1e8 comparisons.
        bool has_null = false;
        std::vector<Value> values = distinctNonNull(res.rows, &has_null);

        if (static_cast<int>(values.size()) > MAX_MATERIALIZED_IN_VALUES) {
            throw std::runtime_error(
                "IN subquery produced " + std::to_string(values.size())
                + " distinct values, above the materialization limit of "
                + std::to_string(MAX_MATERIALIZED_IN_VALUES)
                + " (Week 32 lowers this form to a semi-join)");
        }

        // THREE-VALUED IN. The set can hold a NULL for the first time in this
        // engine's history: InExpr's "never NULL" invariant held only because the
        // grammar has no NULL literal, and a subquery result is not the grammar.
        //
        //   x IN (S)      -> TRUE on a match, else FALSE (no NULLs) / UNKNOWN
        //   x NOT IN (S)  -> FALSE on a match, else TRUE (no NULLs) / UNKNOWN
        //
        // UNKNOWN and FALSE are indistinguishable to every consumer reachable
        // here — a subquery is legal in WHERE/HAVING only, so the consumer is a
        // filter; there is no general NOT; `IS NULL` parses at the additive
        // level so it cannot be applied to a predicate's result; and CASE WHEN
        // does not take an UNKNOWN branch. THAT is why the collapse below is
        // sound. Adding a general NOT breaks it first.
        if (sq->negated && has_null) {
            return std::make_unique<Literal>(Value(int64_t(0)));  // never TRUE
        }
        if (values.empty()) {
            // x IN () is FALSE; x NOT IN () is TRUE, even for a NULL x.
            return std::make_unique<Literal>(
                Value(static_cast<int64_t>(sq->negated ? 1 : 0)));
        }

        auto in = std::make_unique<InExpr>();
        in->operand  = std::move(sq->operand);   // belongs to THIS block, already bound
        in->values   = std::move(values);
        in->negated  = sq->negated;
        return in;
    }
    }
    throw std::runtime_error("internal: unknown SubqueryExpr::Kind");
}
```

### Implementation guidance

1. `distinctNonNull` must dedupe **by value identity, not by display text**.
   `Value::toString()` renders a DOUBLE with `%.15g`, which Week 27 measured
   collapsing 3245 distinct sums into 2526 texts. Either sort with
   `compareForSort` + unique, or reuse `keyFieldText` from
   `execution/key_encoding.h` — the shared contract that exists for exactly this
   question. Do not hand-roll a third rule.
2. `MAX_MATERIALIZED_IN_VALUES`: start at 1024, and **measure before defending
   it** — time `SELECT COUNT(*) FROM laps WHERE lap_id IN (SELECT lap_id FROM
   laps WHERE lap_id < N)` on `--storage row` for a few N. Put the number and the
   measurement in `subquery_materialization.h`, not in a commit message.
3. The `IN` operand is **moved** out of the `SubqueryExpr`. It is already bound
   and already folded (site 14 folds the operand and never the body), so it needs
   neither.
4. Type mismatch between the operand and the materialized values (STRING vs
   numeric) is caught by `inferExprType`'s existing `InExpr` rule with a
   plan-time message. That is the same family as the Week 29 `JOIN ON` STRING/
   numeric refusal; note the divergence from SQLite (which answers, with zero
   matches) in Limitations rather than inventing an affinity rule.
5. Do not "optimize" `EXISTS` into `COUNT(*) > 0`. `LIMIT 1` on the body already
   bounds it and keeps the body's own semantics untouched.
6. Common mistake: applying `negated` twice — once when building the constant and
   again by leaving `negated` set on a substituted `InExpr`. The `InExpr` branch
   is the only one that keeps the flag, because `InExpr` implements it.

### Verification

Unit tests with a fake runner, one per row of the table above. The NULL ones are
the point:

```cpp
TEST(SubqueryMaterialization, NotInWithANullInTheSetIsNeverTrue) {
    // SQL: `x NOT IN (1, NULL)` is FALSE for x = 1 and UNKNOWN otherwise, so the
    // predicate is never TRUE and the query returns no rows. Returning rows here
    // is the classic NOT-IN defect.
    ...
    EXPECT_EQ(literalIntOf(stmt.where.get()), 0);
}
```

End-to-end, against the oracle (all four modes):

```bash
# scalar
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT COUNT(*) FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)"

# scalar cardinality — the checkpoint's runtime check
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT team FROM laps WHERE speed > (SELECT speed FROM laps)"
# Error: scalar subquery returned more than one row

# empty scalar -> NULL -> UNKNOWN -> zero rows (and NOT a plan-time type error)
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT team FROM laps WHERE speed > (SELECT speed FROM laps WHERE lap_id = -1)"

# EXISTS / NOT EXISTS, uncorrelated
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT COUNT(*) FROM drivers WHERE EXISTS (SELECT * FROM laps WHERE speed > 340)"
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT COUNT(*) FROM drivers WHERE NOT EXISTS (SELECT * FROM laps WHERE speed > 100000)"

# IN / NOT IN, uncorrelated
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT name FROM drivers WHERE driver_id IN (SELECT driver_id FROM laps WHERE speed > 340)"
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT name FROM drivers WHERE driver_id NOT IN (SELECT driver_id FROM laps WHERE speed > 340)"
```

Every one of these has a SQLite answer, so they belong in the harness (Task 7),
not only in your shell history.

---

## Task 5 — Wiring `main.cc`: loading, ordering, and one runner per engine

### Why it matters

`main.cc` is the only place that knows which engine was selected and holds the
loaded data, so it is the only place the runner can be built. It is also where a
nested query's tables must be loaded — today's loader walks `stmt.from_table` and
`stmt.joins` only, so a subquery over a second table would fail with a raw
`std::out_of_range` from `table_rows.at(...)` rather than anything a user can act
on.

### Conceptual explanation

Four changes, in this order:

1. **Load and stat every table any nested query names** (Task 2's
   `collectQueryTables`). Statistics matter: the vectorized optimizer reads
   `TableStats` for the sub-plan too.
2. **Validate before materializing** (D6).
3. **Build a runner for the selected engine** and call the pass.
4. **Copy the tables the nested query scans.** Both scan nodes take their table
   by value. The outer query needs its copy too, so the sub-run gets its own.

The runner must honour `--no-optimize`. `compare_against_sqlite.py` runs the
vectorized suite twice, and that second run is the differential oracle for the
optimizer; a runner that always optimizes would make the second leg re-use an
optimized subquery result and quietly stop testing the sub-plan.

### Code

Loading (extends the existing loop, keeping its self-join guard and its stats
computation):

```cpp
            std::unordered_map<std::string, std::vector<Row>> table_rows;
            // Week 31: nested queries scan tables the outer FROM/JOIN list never
            // names. One walker (subquery_materialization.h) answers both this
            // and the rewrite, so the two cannot drift.
            std::vector<std::string> needed;
            collectQueryTables(stmt, needed);
            for (const auto& name : needed) {
                if (table_rows.count(name)) continue;          // self-join names one twice
                const TableMetadata& m = catalog.getTable(name);
                table_rows[name] = CSVLoader::load(m.filepath, m.schema);
                if (!catalog.hasStats(name)) {
                    catalog.setStats(name, TableStats::compute(table_rows[name], m.schema));
                }
            }
```

The two runners and the pass, after the columnar conversion and before the
`args.execution == "vectorized"` branch:

```cpp
            // Week 31. Diagnostics first: materialization TRUSTS the arity,
            // position and correlated-subquery rules, so a query that breaks one
            // must be refused before anything runs. validate() is pure, so the
            // planners calling it again below costs one walk.
            Validator::validate(stmt, catalog);

            SubqueryRunner run_subquery;
            if (args.execution == "vectorized") {
                run_subquery = [&](SelectStatement body) {
                    // Its own copies: both scan nodes take a table BY VALUE, and
                    // the outer query still needs the originals. Lowering's
                    // scan_uses counter already copies for a self-join — same
                    // cost model, and the reason a shared table representation is
                    // on the Week 37 list rather than this one.
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
            materializeSubqueries(stmt, run_subquery);
```

with two small local helpers next to `printResults`:

```cpp
// Plan and drain one statement to rows. Mirrors the main vectorized path's
// stage order exactly — pushdown, then join enumeration, then estimation — so a
// nested query is optimized by the same passes in the same order as a top-level
// one. --no-optimize is threaded through for the same reason it is at the top
// level: it is the differential oracle, not a debug switch.
static SubqueryResult runVectorizedToRows(SelectStatement stmt, const Catalog& catalog,
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
    SubqueryResult out{node->outputSchema(), {}};
    while (DataChunk* chunk = node->nextChunk()) { /* same drain as main() */ }
    node->close();
    return out;
}
```

### Implementation guidance

1. **Extract the drain loops rather than copying them.** `main()`'s vectorized
   drain (selection-vector aware, `valueAt` per cell) is a dozen lines and is
   already subtle; a second copy that forgets `chunk->filter_applied` returns
   filtered-out rows. Pull it into `drainVec(VecPlanNode*)` and call it from both.
2. `runVolcanoToRows` is the same shape: `Planner::plan(...)`, `open()`, `next()`
   until null, `close()`, and `plan->outputSchema()`.
3. **A three-or-more-relation body is refused on Volcano**, by the pre-existing
   Week 27 guard, with its existing message naming `--execution vectorized`. That
   is an ordinary capability difference, identical to the one that already exists
   for a top-level multi-way join — the harness has a pattern for it (Task 7).
   Do not add a special case.
4. Keep the pass **outside** the `--explain` early-return. `--explain` must go
   through it or it will print a plan the engine cannot build. This is the
   documented cost from D1.
5. Do not move the load block after the columnar conversion: `table_rows` is
   cleared there.
6. Gotcha: `catalog.setStats` is guarded by `hasStats` because statistics are
   process-scoped, not query-scoped. Keep the guard for nested tables too.
7. Gotcha: the vectorized runner reads `columnar_tables` *before* `main()` moves
   it into `VectorizedPlanBuilder::build`. Materialization happens first, so this
   is safe — but if anyone later reorders those two statements the map is
   moved-from and every nested scan reads an empty table. A comment at the move
   is cheaper than the bug.

### Verification

```bash
# a nested table the outer query never names — this is the load-path test
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT COUNT(*) FROM laps WHERE speed > (SELECT AVG(age) FROM drivers)"

# all four modes must agree, including --no-optimize
for m in "" "--storage columnar" "--execution vectorized --storage columnar" \
         "--execution vectorized --storage columnar --no-optimize"; do
  ./build/swiftql --catalog catalog.json --no-cache $m --query \
    "SELECT name FROM drivers WHERE driver_id IN (SELECT driver_id FROM laps WHERE speed > 340)"
done

# projection pushdown is back (Week 30's "first benchmark surprise" note):
# the scan must list only the columns the query needs, not all nine
./build/swiftql --catalog catalog.json --no-cache --explain \
  --execution vectorized --storage columnar --query \
  "SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)"
```

---

## Task 6 — Close dispatch sites 12 and 13, and the typeless NULL constant

### Why it matters

Week 30's note is explicit: `inferExprType` and `evaluate` "must close in the
*same* commit that lowers one". `inferExprType` is the contract the vectorized
path pre-allocates output columns from, and `evaluate()` is the semantic
reference — a disagreement between them is `bad_variant_access`, not a SQL error.

The closure under option B is not the one Week 30 anticipated, and saying so
plainly is part of the job: no `SubqueryExpr` ever reaches either site, so the
correct closure is (a) a reworded throw that says *why* reaching it is a bug, and
(b) the type rule that substitution genuinely does create — the null `Literal`.

### Conceptual explanation

Sites 12 and 13 are the backstop that makes site 19 (Task 2) a loud walker
instead of a silent one. Deleting their throws would convert "a missed `Expr`
subtype in the materialization walker" from a plan-time error into a wrong
answer. Keep them; change only the message, from a week number that has now
arrived to a statement of the invariant they enforce.

The null `Literal` is D5. One field, one branch, and two header comments whose
stated premises this week invalidates.

### Code

`src/parser/ast.h`:

```cpp
struct Literal : Expr {
    Value value;
    // Week 31. Only meaningful when `value` is null, which the grammar cannot
    // produce and constant folding refuses to produce: the sole source is an
    // uncorrelated scalar subquery that returned zero rows, or one NULL row.
    // Value has no typed null (Value::type() throws), and inferExprType must
    // answer for every node — so carry the type the subquery's own output
    // schema already gave us instead of inventing a convention. Defaulted, so
    // every existing construction is unchanged.
    TypeId null_type = TypeId::INT;
    explicit Literal(Value v) : value(std::move(v)) {}
};
```

`src/planner/logical_plan.cc`:

```cpp
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        // A null Literal is Week 31's materialized empty scalar subquery. Its
        // type came from the subquery's projection schema; Value::type() throws
        // on null and there is nothing to re-derive it from here.
        if (lit->value.isNull()) return lit->null_type;
        return lit->value.type();
    }
```

and, in the same file, site 12:

```cpp
    if (dynamic_cast<const SubqueryExpr*>(expr)) {
        // DISPATCH SITE 12, closed in Week 31 — as an INTERNAL invariant, not as
        // a feature. Every subquery is replaced by a constant before planning
        // (materializeSubqueries, called from main.cc before either planner), and
        // a correlated one is refused by Validator. Reaching this therefore means
        // the materialization walker (site 19) missed an Expr subtype, or the
        // pass was not run. This throw is what makes that walker LOUD; do not
        // delete it.
        throw std::runtime_error(
            "internal: a subquery reached type inference without being "
            "materialized (materializeSubqueries must run before planning)");
    }
```

`src/execution/evaluator.cc` (site 13) gets the same treatment with "reached
evaluation".

Two header comments must be corrected in the same commit, because both state a
premise this week breaks:

- `src/planner/constant_folding.h`: "there is no NULL literal in the grammar, and
  a `Literal` holding a null `Value` has no `type()` for `inferExprType` to
  report" → still true of *the grammar*, no longer true of *the tree*; the fold
  still declines, now for the stated reason that it needs no NULL literal of its
  own.
- `src/execution/expression_executor.cc`'s `Literal` case: its decline is now
  reachable, and its consequence (the enclosing predicate falls back to
  `evaluate()` per row) should say so.

### Implementation guidance

1. Set `null_type` at exactly one place — `buildReplacement`'s SCALAR branch.
   Anywhere else and it is a field two writers can disagree about.
2. Do **not** teach `compileNode` to broadcast a null constant column. Its
   decline is correct and is what keeps every kernel out of this. The cost is one
   predicate per query evaluated scalar-wise on a query that returns few rows by
   construction (the scalar was NULL, so the comparison is UNKNOWN for every row).
3. Do **not** add a NULL literal to the grammar. Nothing needs it, and it would
   re-open the three-valued `IN` rule Week 25 deliberately collapsed.
4. Check `Schema::column(0)` is safe: the Validator guarantees exactly one output
   column for a `SCALAR` body, and the sub-plan's output schema is that column.
   An `assert`-style comment naming the Validator rule is worth one line.

### Verification

```cpp
TEST(SubqueryMaterialization, AnEmptyScalarBecomesATypedNullLiteral) {
    // zero rows -> NULL, typed from the body's output column, so inferExprType
    // answers instead of throwing "Cannot get type of null Value"
    ...
    EXPECT_NO_THROW(inferExprType(stmt.where.get(), laps_schema));
}
```

```bash
# the STRING case, which the "just return INT" shortcut gets wrong
./build/swiftql --catalog catalog.json --no-cache --query \
  "SELECT lap_id FROM laps WHERE SUBSTRING((SELECT name FROM drivers WHERE driver_id = -1), 1, 2) = 'Ab'"
# 0 rows, no error   (not: SUBSTRING requires a STRING operand)
```

---

## Task 7 — Tests, the harness split, and the documents the next week reads

### Why it matters

The harness is the only place that runs four modes against an external oracle,
and this is the first week subqueries return rows to diff. `development.md`'s two
tables are what Weeks 32–34 will read as already-checked — a missing row is worse
than a wrong one, because the next week does not re-verify it.

### Conceptual explanation

Three bodies of work, all mechanical once the design is settled:

**a) Migrate the existing C++ tests.** Roughly ten tests in
`tests/test_logical_plan.cc` assert `"not yet executable (Week 31)"`. Split them:
- correlated inputs → assert the new Week 33 message;
- uncorrelated inputs that were only *reaching* the refusal → either delete (the
  behaviour they proved is now covered end-to-end) or convert to asserting the
  substituted tree, which is strictly better evidence;
- `SubqueryDispatch.TypeInferenceAndEvaluationAreLoudAndNamed` → keep, retargeted
  at the new internal message. It is now the backstop test for site 19.

**b) Split the harness suites.** `WEEK30_SUBQUERY_BINDS` currently mixes
correlated and uncorrelated queries under one "reaching the refusal" assertion.
It becomes three lists:
- `WEEK31_SUBQUERY_QUERIES` — uncorrelated, single-relation bodies: **diffed**
  against SQLite in all four modes, appended to `QUERIES`;
- `WEEK31_SUBQUERY_VEC_ONLY` — uncorrelated with a 3+ relation body: diffed in
  the two vectorized modes, asserted refused in the two Volcano ones, exactly as
  `MULTIWAY_QUERIES` / `MULTIWAY_VOLCANO_REJECTED` already are;
- `WEEK33_CORRELATED_BINDS` — the correlated ones, still a rejection suite, now
  expecting `"not yet executable (Week 33)"`.

`WEEK30_REJECTED_QUERIES` stays as it is: every entry must still fail earlier and
for its own reason, which is what stops the new refusal becoming a catch-all.

**c) Update the documents.** Three files, all in the same commit as the code they
describe.

### Code

Harness shape (`python_tools/compare_against_sqlite.py`):

```python
# Week 31. Uncorrelated subqueries EXECUTE, so these are diffed against SQLite
# rather than asserted to be refused — the first subquery rows this project has
# ever produced. Kept as its own list (rather than folded into QUERIES) so the
# suite that changed meaning this week is visible in the output.
WEEK31_SUBQUERY_QUERIES = [
    # scalar in WHERE (Q22's uncorrelated half)
    "SELECT COUNT(*) FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)",
    # scalar in HAVING (Q11's shape)
    "SELECT team, AVG(speed) FROM laps GROUP BY team "
    "HAVING AVG(speed) > (SELECT AVG(speed) FROM laps) ORDER BY team",
    # the scalar composes with arithmetic, and re-folding must keep the
    # ColumnRef-op-Literal shape the fast paths match on
    "SELECT COUNT(*) FROM laps WHERE speed > 0.5 * (SELECT AVG(speed) FROM laps)",
    # empty scalar -> NULL -> UNKNOWN -> no rows, and NOT a plan-time type error
    "SELECT team FROM laps WHERE speed > (SELECT speed FROM laps WHERE lap_id = -1)",
    # EXISTS / NOT EXISTS, uncorrelated, both truth values
    "SELECT COUNT(*) FROM drivers WHERE EXISTS (SELECT * FROM laps WHERE speed > 340)",
    "SELECT COUNT(*) FROM drivers WHERE NOT EXISTS (SELECT * FROM laps WHERE speed > 99999)",
    # IN / NOT IN (Q16/Q18's shape), and NOT IN against an empty set, which is TRUE
    "SELECT name FROM drivers WHERE driver_id IN "
    "(SELECT driver_id FROM laps WHERE speed > 340) ORDER BY name",
    "SELECT name FROM drivers WHERE driver_id NOT IN "
    "(SELECT driver_id FROM laps WHERE speed > 340) ORDER BY name",
    "SELECT name FROM drivers WHERE driver_id NOT IN "
    "(SELECT driver_id FROM laps WHERE speed > 99999) ORDER BY name",
    # a subquery over a table the outer query never names: the load-path test
    "SELECT COUNT(*) FROM laps WHERE speed > (SELECT AVG(age) FROM drivers)",
    # BETWEEN clones its left operand and cloneExpr SHARES the statement, so this
    # is two nodes over one statement — one run, two substitutions (D4)
    "SELECT COUNT(*) FROM laps WHERE "
    "(SELECT MAX(age) FROM drivers) BETWEEN 1 AND 99",
    # a subquery inside a CASE inside a WHERE: the walker must reach it
    "SELECT COUNT(*) FROM laps WHERE "
    "CASE WHEN speed > (SELECT AVG(speed) FROM laps) THEN 1 ELSE 0 END = 1",
    # a subquery on a query that also joins, so pushdown and the (now restored)
    # projection narrowing run on the same tree
    "SELECT l.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "WHERE l.speed > (SELECT AVG(speed) FROM laps) GROUP BY l.team ORDER BY l.team",
]
```

### Implementation guidance

1. A query with no `ORDER BY` is compared through `normalize()`, which is
   order-insensitive; give the aggregate/`GROUP BY` ones an `ORDER BY` anyway so
   a genuine ordering defect is not hidden. This is the trap Week 35's note
   records.
2. Add the two rows to `development.md`'s slot-consumer table even though both
   are "reads no slot" — the table's value is completeness:
   - `materializeSubqueries` / `buildReplacement` — **level-agnostic, safe by
     precondition**: it only ever meets an uncorrelated subquery, because
     `Validator::validate` refuses a correlated one first. Name the precondition,
     not the conclusion (the Week 29 `jc` lesson: a comment that states "Volcano
     builds exactly one join" without naming the refusal that guarantees it is
     how the coupling gets silently re-introduced).
   - `forEachSubquery` / `collectQueryTables` — reads no slot; descends into the
     body deliberately.
3. Rewrite that table's *preamble* for the new containment (D3): "`Validator`
   refuses any statement with `has_subquery`" is no longer true. The replacement
   sentence is: **a correlated reference is refused, and an uncorrelated body is
   planned as its own top-level query, so every consumer below still sees level-0
   refs from one range table.**
4. Add site 19 to the dispatch-site table and change "Eighteen functions" to
   "Nineteen". Say in its row that it is the one walker that descends into the
   body, and why, and that its failure mode is loud because of sites 12/13.
5. Update both tripwire rows to record that Week 31 checked and did **not** arm
   them, with the one-line reason. Otherwise Week 32 reads two guards with a note
   saying "Week 31 will arm these" and cannot tell whether the note is stale or
   the work was skipped.
6. README, in the same commit: the Week 31 section (checkpoint ✅, a "Shipped /
   Why it was required" table, and starting notes for Week 32), the Feature Scope
   subquery bullet (which currently says subqueries do not execute), the
   `Syntax Deliberately Not Supported` row for cardinality (it says the runtime
   check "is Week 31's" — it now exists), and Limitations: `--explain` executes
   subqueries, subquery time is charged to plan time, the `IN` materialization
   cap, correlated subqueries refused until Week 33, and the table copy per
   nested query.

### Verification

The full gate (the `verify` skill), from the repo root:

```bash
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)
cd build && ./tests/swiftql_tests && cd ..
python3 python_tools/compare_against_sqlite.py
python3 python_tools/test_new_queries.py
```

Done means: all four gates green, `WEEK31_SUBQUERY_QUERIES` diffing clean in four
modes, the correlated suite refused in four modes with the Week 33 message, and
`WEEK30_REJECTED_QUERIES` unchanged and still passing.

---

## Postscript — what implementation changed about this plan

Recorded here rather than edited into the tasks above, so the difference between
what was designed and what was found stays visible.

1. **`CardinalityEstimator::selectivity` was a live throw, not a hypothetical.**
   D5 predicted the null `Literal` would need a rule in `inferExprType` and
   nothing else. It also needed one in the estimator: both comparison branches
   call `lit->value.type()`, which throws on a null `Value`, so the **optimized**
   vectorized path died with `Cannot get type of null Value` on
   `WHERE speed > (SELECT speed FROM laps WHERE lap_id = -1)` — a query the
   `--no-optimize` leg answered correctly. It now returns `0.0`, which is exact
   rather than a fallback. The same sweep found two more readers worth touching:
   `cloneExpr` (site 11) dropped `null_type`, and `collectSimplePredicates` was
   correct only by the accident that `Value`'s comparisons are three-valued. The
   whole audit is written into `development.md` → *Null constants (Week 31)*, in
   the shape the slot-consumer table uses, because "find every reader of the
   state you just made possible" is the same discipline in a different
   dimension.
2. **The table loader had to stop being a diagnostic site.** Walking nested
   statements meant `catalog.getTable()` was called on a nested table before the
   Validator ran, so `EXISTS (SELECT * FROM nosuchtable)` reported
   `Table name does not exist` instead of `Table not found: 'nosuchtable'`. The
   loader now skips a table the catalog does not hold and leaves the message to
   the Validator a few lines below. Caught by the harness's Week 30 rejection
   suite, which exists for exactly this.
3. **The explicit `Validator::validate` call sits inside `if
   (stmt.has_subquery)`.** D6 put it unconditionally in the pipeline; that would
   validate every ordinary query twice for no benefit, since the ordering
   property exists only to protect the pass.
4. **The subquery's time had to be added to the plan timer explicitly.** D1 said
   it would land in `--explain-analyze`'s `Plan:` line; as first written it
   landed in *neither* line, because both plan timers start after the pass — the
   same gap the CSV load deliberately sits in. Excluding a nested query's
   execution from every clock would flatter Week 37's numbers on precisely the
   queries this week adds, so the pass is timed and its duration is added to
   `plan_us` on both paths.
5. Everything else landed as designed: both tripwires untouched and still
   unreached, no correlated reference lowered, and therefore no `ColumnId`
   prerequisite triggered.

## What to watch during the week

Ordered by how expensive the mistake is to find later.

1. **The `NOT IN` NULL rule.** No shipped CSV can produce a NULL (invariant 14),
   so the SQLite oracle cannot reach it from catalog data — a `LEFT JOIN` inside
   the subquery body can, and that is the cheapest way to get a NULL through the
   whole pipeline (Week 29's note). Write that query, or the rule is untested.
2. **Diagnostic ordering.** After the narrowed refusal, run the whole of
   `WEEK30_REJECTED_QUERIES` and confirm every message is unchanged. A refusal
   that moves earlier turns a real query defect into "correlated subqueries are
   not yet executable", which is the failure Week 26 established the placement
   discipline to prevent.
3. **`--explain` on a subquery query.** It must not crash and must not print a
   `SubqueryExpr`. It will print the materialized constant — that is the design,
   and it belongs in Limitations before anyone reports it as a bug.
4. **The optimizer differential.** `test_new_queries.py`'s invariant suite must
   see optimized ≡ `--no-optimize` for every new query. If the runner ignores
   `args.no_optimize`, both legs share an optimized sub-plan and the suite passes
   without testing anything.
5. **Volcano's multi-way refusal reaching a subquery body.** Expected and
   correct; make sure the message is the existing one naming
   `--execution vectorized`, not a new one invented for subqueries.
6. **Do not touch the two tripwires.** If a change appears to require arming one
   of them, stop: that means a correlated reference has reached a plan, which
   means the refusal in Task 1 is wrong — and it also means the `ColumnId`
   prerequisite has triggered and this week has become a different week.
