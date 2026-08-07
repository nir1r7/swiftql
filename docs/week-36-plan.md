# Week 36 — Query Coverage + Correctness

**README bullets:** port queries to the documented SwiftQL dialect; close
query-specific parser, execution and optimizer correctness gaps; document
supported scale and memory limits.
**Checkpoint:** supported TPC-H queries match reference results within numeric
tolerance — where "reference results" is **SQLite over the same `.tbl` files**,
never the published TPC-H answer set. `data/tpch/sf0.01/PROVENANCE.txt` states
why: `dbgen` was unavailable, the generator reproduces the spec's value
*domains* but not its *distributions*, so the published answers do not apply.
Every sentence this week writes says **"matches SQLite"**, never "correct" and
never "TPC-H compliant".

**The measured starting point,** from a full 22×4 run recorded in
`docs/tpch-baseline.json` and `docs/tpch-sf0.01-report.json`:

```
GATE tpch: PASS (19/22 meaningful vs SQLite: 5 in all four modes,
                 14 vectorized-only; 1 vacuous; 2 unported)
```

Week 36 is the first week whose goal is a **number that already exists**. The
fifth gate step re-measures it on every `verify` run and exits non-zero on any
regression, so nothing below can quietly cost coverage.

---

## Tasks

1. **Lift the constant-wrapper restriction so TPC-H's own Q17 text runs** — the
   one capability change that raises the headline figure, verified before
   planned on.
2. **The refusal sweep the lift obliges** — every comment, precondition,
   assertion, header, pinned needle and published count citing
   `found[0] == select_list[0]`.
3. **Q21: establish what it actually needs, and decide it in the open** — the
   correlated *inequality*, and the residual semi/anti join it implies.
4. **Mode coverage: settle Volcano semi/anti parity on the measured 34-cell
   breakdown** — say which of the two figures this week targets, with evidence.
5. **The dialect and divergence items three earlier weeks handed here** — the
   `SUBSTRING(d,1,4)` STRING year, the `ON` STRING-vs-numeric half-match, NaN
   groups, DOUBLE display, `SUM` in `double`.
6. **The small items owed** — `compare_against_sqlite.py`'s NaN/inf comparison,
   `random_diff.py`'s projection, `--time`, `--fingerprint-all`, per-query hand
   verification beyond q2/q18/q19.
7. **Document supported scale and memory limits** — the README bullet nobody has
   started, measured rather than asserted.
8. **Re-baseline, the correctness report, and the five-step gate** — the figure
   with its mode split, in the same commit as the capability that moved it.

**Target: 20/22 meaningful committed (Q17), 21/22 if Task 3 lands. All of it is
headline count, none of it is mode coverage — Task 4 shows why, with the
measurement that contradicts the assumption.**

---

*(Sections below are appended as each task is worked. This file is written
incrementally on purpose.)*

## Task 1 — Lift the constant-wrapper restriction so TPC-H's own Q17 text runs

### Why it matters

Week 34 built the correlated-scalar *mechanism*; Week 35 measured that the
mechanism does not run the query it was built for. `q17` is recorded UNPORTED in
`docs/tpch-baseline.json` (`"modes": {"q17": 0}`), and it is the single cheapest
cell in the whole 22×4 matrix to turn green: everything downstream of the
rewrite — `LogicalDerived`, the LEFT join, the merged schema, the COUNT `CASE`
wrapper, `VecDerived` — already exists and already matches SQLite on the
semantically identical constant-**outside** form.

Downstream impact is narrow by construction and that is the point of doing it
this way: nothing new reaches the optimizer, the executors or storage. What does
change is a **refusal**, and this project has a rule about that (Task 2).

### Verify the inherited claim before planning on it

The README asserts the restriction is `found[0] == body.select_list[0]`. Do not
take it on trust — Week 35 wrote it from a reading, not a run. Both legs
reproduce on the committed sf0.01 data:

```
$ ./build/swiftql --catalog data/tpch/sf0.01/catalog.json \
      --storage columnar --execution vectorized --format tsv \
      --query "... AND l_quantity < (SELECT 0.2 * AVG(l_quantity) FROM lineitem
                                     WHERE l_partkey = p_partkey)"
Error: correlated subquery: a correlated scalar subquery is decorrelated only
when its select list is a single aggregate (...)

$ ./build/swiftql ... --query "... AND l_quantity < 0.2 * (SELECT AVG(l_quantity)
                                     FROM lineitem WHERE l_partkey = p_partkey)"
avg_yearly
2732.80428571429            # SQLite: 2732.804285714286  → within rel_tol=1e-9
```

Three further facts settle that the lift is worth a week's opening task, and all
three are measurements, not arguments:

1. **The refusal is the only thing in the way.** The constant-outside form
   produces this plan, which is exactly the tree the rewrite would build from the
   spec text:

   ```
   LogicalAggregate [SUM(l_extendedprice)]
     LogicalFilter [(lineitem.l_quantity < (0.2 * AVG(l_quantity)))]
       LogicalLeftJoin [p_partkey = l_partkey] join-ordering=skipped (outer join)
         LogicalJoin [l_partkey = p_partkey]  ...
         LogicalDerived [$scalar0, 2 columns]
           LogicalProject [l_partkey, AVG(l_quantity)]
             LogicalAggregate [l_partkey | AVG(l_quantity)]
   ```

2. **The data will actually test it.** Week 35's mutation check gives a verdict
   even for a query the engine refuses, precisely so this week knows in advance:

   ```
   >>> mutation_check(conn, "q17")
   ('DISCRIMINATING', 'the correlated 0.2 * AVG(l_quantity) scalar subquery', '1 rows -> 1')
   ```

   So q17 will count as **meaningful**, not merely answered — unlike q18, and
   unlike q2/q19 before their parameters were re-chosen. Note the row counts are
   equal (`1 rows -> 1`): the discrimination is in the **value**, which is why
   `mutation_check` compares answers as a multiset rather than counting rows.

3. **It buys 2 cells, not 4.** q17's outer query is `lineitem JOIN part` plus the
   decorrelated LEFT join — two joins — so `Planner::plan`'s `stmt.joins.size() > 1`
   refusal keeps Volcano out regardless. q17 goes 0 modes → **2**, and the two
   Volcano cells stay `REFUSED_EXPECTED` with the message they already carry.

### Conceptual explanation

The body of a correlated scalar subquery is rewritten into a grouped derived
relation, LEFT-joined back on the correlation key, and the `SubqueryExpr` is
replaced in place by a reference to the aggregate's output column. The
restriction says the body's one select-list item must **be** the aggregate node.
TPC-H writes an expression *around* it.

There are two ways to accept the spec's shape, and they are not equally safe.

**Option A — keep the wrapper inside the body.** Push `0.2 * AVG(l_quantity)`
into the body's select list unchanged and name the derived column
`exprToString` of the whole `BinaryExpr`. This works at the language level; the
engine already supports an aggregate under an expression in a grouped select
list:

```
$ ./build/swiftql --catalog catalog.json --format tsv --execution vectorized \
      --query "SELECT team, 0.2 * AVG(speed) AS s FROM laps GROUP BY team"
team         s
AlphaTauri   62.6503070624361
```

**But Option A breaks the COUNT rule silently.** The zero-row case has no group
row at all, so the LEFT join null-extends, and the substitution site wraps a
`COUNT` body in `CASE WHEN ref IS NULL THEN 0 ELSE ref END`. Under Option A the
`ref` names the *whole wrapper*, so a body of `1 + COUNT(*)` over an empty
correlation group yields **0** where SQL says **1**. Fixing that means
re-evaluating the wrapper with the aggregate replaced by `0`, i.e. building a
second copy of the tree — new code, new correctness surface, on the exact rule
that already shipped one silent wrong answer (Week 34 audit F1).

**Option B — move the wrapper outside.** Keep the body's select list as the bare
aggregate, exactly as today, and re-attach the wrapper **around the substituted
reference** in the outer expression. This is legal only when the wrapper is
**constant apart from the aggregate**: for a fixed correlation group the
aggregate is one value and every other leaf is the same value for every outer
row, so evaluating `f(agg)` per group inside and `f(agg_column)` per outer row
outside are the same function of the same argument.

Option B is the minimum change, and it fixes COUNT for free: the `CASE` lands at
the aggregate's position *inside* the wrapper, so `1 + COUNT(*)` becomes
`1 + CASE WHEN ref IS NULL THEN 0 ELSE ref END` = 1. It is also, literally, the
form already proven against SQLite — after the rewrite the spec text and the
constant-outside text produce the *same tree*, which is Task 1's strongest
verification (below).

**Take Option B.** State Option A and why it was declined in the header, so the
next reader does not re-derive it.

**What "constant apart from the aggregate" must exclude, and why each:**

| Node in the wrapper | Verdict | Reason |
|---|---|---|
| `Literal` | allow | the whole point |
| `BinaryExpr`, `UnaryExpr` over allowed children | allow | arithmetic; constant-folds or evaluates identically inside or outside |
| the one `AggregateExpr` | allow, exactly once | it is what is being lifted |
| `ColumnRef` | **refuse** | a body-local ref outside the aggregate is an ungrouped reference (a different query); an *outer* ref would be a correlated ref in the SELECT list, which `refuseSurvivingCorrelatedRefs` already declines by name. Do not silently widen that |
| a second `AggregateExpr` | **refuse** | `AVG(x) / COUNT(*)` needs two output columns and two zero-row rules; the `CASE` wrapper is written for one |
| `SubqueryExpr` | **refuse** | a nested body inside the wrapper is a scope question this rewrite does not answer |
| `CaseExpr`, `SubstringExpr`, `LikeExpr`, `InExpr`, `IsNullExpr`, `IntervalLiteral` | **refuse** | none is needed by any TPC-H query, each adds a type or NULL question, and `IntervalLiteral` must not survive planning at all (`foldNode` rewrites `date ± interval`; one that reaches `inferExprType` throws) |

That whitelist is deliberately narrower than "no column references". Minimum
code that solves the problem: Q17 needs `Literal * AggregateExpr`.

### Code snippets (illustrative)

One function validates **and** locates, in `subquery_decorrelation.cc`'s
anonymous namespace. Two functions — a `const` validator and a mutating locator
— would be two walkers that must agree, which is the drift this codebase has
undone three times.

```cpp
// Week 36 — the CONSTANT WRAPPER around the body's aggregate.
//
// TPC-H Q17 writes `(SELECT 0.2 * AVG(l_quantity) ...)`: the select-list item is
// a BinaryExpr WRAPPING the aggregate, not the aggregate. Week 34 required
// found[0] == select_list[0] and refused the spec's own text.
//
// The wrapper is lifted OUT of the body rather than pushed through it: the body
// still selects the bare aggregate, and the wrapper is re-attached around the
// SUBSTITUTED reference in the outer expression. Legal exactly when every leaf
// other than the aggregate is a constant -- then f(agg) per group and
// f(agg_column) per outer row are the same function of the same argument.
//
// !! WHY NOT KEEP THE WRAPPER IN THE BODY. Naming the derived column
// exprToString(wrapper) also runs, but it breaks the COUNT rule: the zero-row
// CASE would substitute 0 for the WHOLE wrapper, so `1 + COUNT(*)` over an empty
// group answers 0 where SQL says 1. Lifting puts the CASE at the aggregate's own
// position, where it is correct by construction.
//
// Returns the OWNING SLOT of the aggregate so the caller can move the aggregate
// out and assign the substitution back in. Refuses by name otherwise; the
// whitelist below IS the walker's domain, so a node the guard admits can never
// be one the walker fails to descend into.
std::unique_ptr<Expr>* constantWrapperAggregateSlot(std::unique_ptr<Expr>& item) {
    if (dynamic_cast<AggregateExpr*>(item.get()))
        return &item;                       // Week 34's shape, now a special case

    if (auto* bin = dynamic_cast<BinaryExpr*>(item.get())) {
        auto* l = constantOnly(bin->left.get())  ? nullptr
                                                 : constantWrapperAggregateSlot(bin->left);
        auto* r = constantOnly(bin->right.get()) ? nullptr
                                                 : constantWrapperAggregateSlot(bin->right);
        // Exactly one side may carry the aggregate: two would need two output
        // columns and two zero-row rules, and the COUNT CASE is written for one.
        if (l && r) refuse("...two aggregates...");
        return l ? l : r;
    }
    if (auto* un = dynamic_cast<UnaryExpr*>(item.get()))
        return constantWrapperAggregateSlot(un->operand);

    refuse("a correlated scalar subquery's select list must be an aggregate, "
           "optionally wrapped in constant arithmetic (TPC-H Q17's "
           "`0.2 * AVG(...)`); this wrapper is not constant");
}
```

`constantOnly` is the same whitelist read the other way — `Literal`,
`BinaryExpr`/`UnaryExpr` over `constantOnly` children, nothing else — and it is
what makes the "no `ColumnRef`, no second aggregate, no subquery" rule a single
statement rather than a list of `dynamic_cast`s to keep in sync. Write the two as
neighbours with one comment covering both.

The call site changes from three lines to five. Note the order carefully:

```cpp
// The guard (unchanged checks: LIMIT / DISTINCT / HAVING / own GROUP BY /
// exactly one select item) now also locates the aggregate inside its wrapper,
// so validation and location cannot disagree.
std::unique_ptr<Expr>* agg_slot = requireDecorrelatableScalarBody(body);
...
splitCorrelation(...);                 // touches body.where only
body.where = conjoinAll(std::move(local));
refuseSurvivingCorrelatedRefs(body);   // still reads body.select_list -- do not
                                       // empty it before this runs
...
// Take the WHOLE item; agg_slot points at a unique_ptr INSIDE it (or at the
// local itself, when the wrapper is the aggregate -- Week 34's shape).
auto wrapper = std::move(body.select_list[0]);
// !! Defensive: agg_slot was taken before splitCorrelation ran. Nothing between
// mutates select_list, and this makes that precondition LOUD rather than a
// dangling read if some later pass starts to.
if (!*agg_slot || !dynamic_cast<AggregateExpr*>(agg_slot->get()))
    throw std::runtime_error("internal: the located aggregate slot moved");

auto agg_expr = std::move(*agg_slot);        // slot is now empty
const auto* agg = static_cast<const AggregateExpr*>(agg_expr.get());
const std::string agg_name = aggregateOutputName(agg);
const bool count_body = agg->function_name == "COUNT";
// ... body.select_list rebuilt as [group keys..., agg_expr] exactly as today ...
```

and the substitution, which is where Option B pays off — **one** code path, and
the existing `slot = std::move(ref)` becomes an assignment into the slot:

```cpp
// Was: slot = std::move(ref);  /  slot = std::move(case_expr);
// Now: put the substitution back where the aggregate was, then hand the whole
// wrapper to the conjunct's slot. When the wrapper IS the aggregate, agg_slot
// == &wrapper and this refills `wrapper` -- one path, no branch, and Week 34's
// shape is a special case rather than a second production.
*agg_slot = count_body ? std::move(case_expr) : std::move(ref);
slot = std::move(wrapper);
```

### Implementation guidance

1. **Take the guard non-`const` and give it a return type.** It is
   `void requireDecorrelatableScalarBody(const SelectStatement&)` today. Change
   the signature, not the call order: the LIMIT / DISTINCT / HAVING /
   `GROUP BY` / select-count messages must keep firing **before** the wrapper
   message, or a pinned suite entry that expects "LIMIT cannot be decorrelated"
   starts seeing the wrapper message instead.
2. **Do not empty `body.select_list` before `refuseSurvivingCorrelatedRefs`.**
   That function's SELECT-list arm is what refuses a correlated reference in the
   body's projection. Moving the item out early turns a live guard into one that
   inspects an empty vector — the failure mode this codebase names in three
   separate headers.
3. **Do not touch `requireDecorrelatableBody`** (the EXISTS guard). Its header
   already explains at length why it is not shared, and Task 3 is the only thing
   this week that may change it.
4. **`aggregateOutputName` is `exprToString`,** so the derived column is still
   named `AVG(l_quantity)`. The wrapper is *not* part of the name, which is
   exactly what makes the resulting tree identical to the constant-outside form.
5. **Expect `join-ordering=skipped (outer join)`.** The LEFT join makes the
   enumerator decline the whole tree, and it *reports* it. That is Week 29's
   documented cost, not a Q17 regression — but it means Q17's join order is
   whatever the builder writes, so do not read the `est=` numbers as an optimizer
   result.
6. **Two lowerings in one query still work by slot arithmetic**
   (`derived_slot = range_table_size + out.lowered`). Nothing in this task
   changes that, but Task 3's Q21 will lean on it, so do not "simplify" it.

### Verification

Success criteria, in order of strength:

- **Plan equality.** The spec text and the constant-outside text must produce the
  **same** logical plan. Diff them:
  ```bash
  ./build/swiftql --catalog data/tpch/sf0.01/catalog.json --execution vectorized \
    --explain --query "<spec form>"    > /tmp/spec.txt
  ./build/swiftql --catalog data/tpch/sf0.01/catalog.json --execution vectorized \
    --explain --query "<const-outside>" > /tmp/outside.txt
  diff /tmp/spec.txt /tmp/outside.txt     # must be empty
  ```
  This is stronger than a result match: it proves the lift produced the tree
  already diffed against SQLite, rather than a different tree that happens to
  agree on one dataset.
- **Result.** `avg_yearly` = `2732.80428571429` against SQLite's
  `2732.804285714286` — inside the TPC-H relative tolerance
  (`rel_tol=1e-9`), not the absolute one.
- **The COUNT rule, which plan equality does not cover.** Add diffed queries on
  the F1 catalog for a wrapped COUNT over a zero-row group, in both directions:
  `d.age > 1 + (SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id AND
  l.speed > 999)` must answer as SQLite does (the wrapper sees 0, not NULL), and
  the same shape with `AVG` must answer NULL-propagating. Week 34 shipped the
  unwrapped pair; this task must ship the wrapped pair or the fix is untested on
  the one rule it interacts with.
- **The refusals that must survive.** A non-constant wrapper
  (`(SELECT AVG(l2.speed) + l2.lap_id ...)`), a two-aggregate wrapper
  (`(SELECT AVG(l2.speed) / COUNT(*) ...)`), a `CASE` wrapper, and a
  non-aggregate body (`(SELECT l2.speed ...)`) must each still refuse **by their
  own message**. The last one is already pinned at
  `compare_against_sqlite.py:1294`; the first three are new entries.
- **Gate.** `run_tpch.py` reports `q17` MATCH in `col-vec` and `col-vec-noopt`,
  `REFUSED_EXPECTED` in the two Volcano modes, and the gate line reads
  `20/22 meaningful ... 5 in all four modes, 15 vectorized-only; 1 vacuous;
  1 unported` — and asks for the baseline refresh (Task 8).

---

## Task 2 — The refusal sweep the lift obliges

### Why it matters

This is the project's most expensive recurring lesson, stated as a rule in the
README: *when a refusal, guard or invariant is removed or changed, sweep every
comment, precondition, assertion and header citing it.* Week 33 shipped **three
silent wrong answers** from exactly this shape, and Week 34's audit found seven
stale preconditions after it. Task 1 lifts a restriction cited in at least
eleven places, four of which are **published counts** that will now be wrong.

The sweep is not documentation hygiene. Two of the sites are *executable*: a
pinned needle in a rejection suite and a recorded baseline. If they are not
swept in the same commit, the gate either fails for the wrong reason or passes
while claiming the old figure.

### Conceptual explanation

Sites divide into four kinds, and only the first is found by grep:

1. **Textual citations** of the restriction — comments and prose that state
   "the select list must *be* the aggregate".
2. **Executable pins** — rejection-suite entries whose needle is the refusal's
   message, and the recorded TPC-H baseline.
3. **Assertions and preconditions** stated in headers as invariants of *other*
   functions, which the lift may or may not still satisfy.
4. **Published counts** — every place the figure 19/22, "2 unported", or "14
   vectorized-only" appears in prose.

Kind 3 is the dangerous one, because grep for the message text does not find it.
The question to ask at each is not "does this mention Q17" but "does this
sentence remain **true**".

### The sweep list (verified present; re-derive rather than trust)

```
src/planner/subquery_decorrelation.h     Week 34 header, points 2 and 3 —
                                          point 3 says the node "sits arbitrarily
                                          deep inside a conjunct"; it must now
                                          also say the aggregate sits inside a
                                          constant wrapper, and why lifting beats
                                          pushing through
src/planner/subquery_decorrelation.cc    requireDecorrelatableScalarBody's
                                          "THE LOAD-BEARING ONE" comment and the
                                          refusal string itself
src/planner/logical_plan.cc:1021         the lowerCorrelatedScalars call site
python_tools/compare_against_sqlite.py   :1291 WEEK34_CORRELATED_SCALAR_REFUSED —
                                          the "single aggregate" entry (a
                                          non-aggregate body: still refuses, keep)
                                          plus NEW entries for the wrapper
                                          boundary, and a NEW diffed entry in
                                          WEEK34_CORRELATED_SCALAR_VEC_ONLY for
                                          the wrapped shape that now runs
python_tools/tpch_queries.py             the module docstring ("the list of them
                                          IS Week 36's worklist") and q17's own
                                          comment ("Week 34's headline")
docs/tpch-baseline.json                  q17: 0 -> 2 modes, "unported" loses q17,
                                          "meaningful" gains it  (Task 8)
.claude/skills/verify/SKILL.md:53         the verbatim GATE tpch: line
README.md:70                              Feature Scope — the long subquery bullet
README.md:115                             the dialect refusal table row
README.md:1781                            Week 34's corrected-in-Week-35 paragraph
README.md:2287                            Limitations — the correlated bullet
development.md                            check "Relation slots and query levels"
                                          and any expression-dispatch list for a
                                          statement of the old rule
```

**The two `README.md` rows are not deletions.** The refusal *narrows*; it does
not vanish. A non-aggregate body, a non-constant wrapper and a two-aggregate
wrapper are all still refused, so the dialect table row must be **rewritten to
the new boundary**, not removed. A removed row is how a limitation becomes a
silent claim of support.

### Implementation guidance

1. **Sweep in the same commit as the lift.** A commit that changes behaviour and
   leaves the counts stale is a commit whose gate line is a lie for as long as it
   sits on the branch.
2. **Grep is the start, not the sweep.** Run it on three different strings, not
   one: `single aggregate`, `select_list\[0\]`, `q17` / `Q17`. Then re-read
   `subquery_decorrelation.h` end to end, because the header's argument structure
   ("three things make this harder than the EXISTS case") is the kind-3 site grep
   cannot find.
3. **Do not add a new dispatch site.** `constantWrapperAggregateSlot` walks only
   `BinaryExpr` and `UnaryExpr` and refuses everything else, so it is not a
   general expression walker and does not join the 17-site dispatch checklist in
   `development.md → Extending the expression language`. Say so in its header, or
   the next person adding an expression node will wonder whether they owe it a
   case. (If it is ever widened to more node types, it **does** join the list —
   write that condition down now.)
4. **Check `WEEK34_CORRELATED_SCALAR_VOLCANO_REJECTED`,** which is generated by a
   comprehension over `WEEK34_CORRELATED_SCALAR_VEC_ONLY`. Adding the wrapped
   shape to the vec-only list adds a Volcano entry for free — confirm the message
   it picks (`VOLCANO_CORRELATED`, not `VOLCANO_IN`) is the one that actually
   fires, by running it.

### Verification

- **The rejection sweep must find the new entries.** Week 35's sweep is
  behavioural and matches `_REJECTED`/`_REFUSED` as a **substring**; it reports
  the suite count and the number of entries executed (17 suites, 157 entries at
  the close of Week 35). After this task the entry count must go **up** by
  exactly the number of entries added, and every new entry must reach a refusal
  whose message it pins.
- **Mutate to prove the new needles bite.** Temporarily make
  `constantWrapperAggregateSlot` accept a `ColumnRef` and confirm the
  non-constant-wrapper entry **fails**. An entry that passes with the guard
  disabled is pinning nothing — this is the discipline Week 35 applied to its own
  new machinery, and the lift is this week's new machinery.
- **No count left stale.** `grep -rn "19/22\|2 unported\|14 vectorized-only"`
  over `README.md`, `docs/`, `.claude/` and `python_tools/` must return nothing
  outside historical Week 35 narrative — and where it is historical narrative,
  it must read as a record of Week 35's measurement, not as the current figure.

---

## Task 3 — Q21: establish what it actually needs, and decide it in the open

### Why it matters

q21 is the other 0-mode query. Unlike q17, nothing about it was left to this week
"because it is a capability change and not harness tidying" — it was left because
**nobody had established what it needs**. Establishing that is the deliverable;
implementing it is a decision that follows, and this task is written so the week
still delivers if the answer is "not this week".

### What it actually needs — measured, not inferred

```
$ ./build/swiftql --catalog data/tpch/sf0.01/catalog.json \
      --execution vectorized --query "<q21 with :NATION = SAUDI ARABIA>"
Error: correlated subquery: only an equality between two columns can become a
join key (a correlated inequality has no equi-join to lower to)
```

That is `splitCorrelation` in `subquery_decorrelation.cc`, and it fires twice:

```sql
EXISTS     (SELECT * FROM lineitem l2
            WHERE l2.l_orderkey = l1.l_orderkey     -- an equality: a join key
              AND l2.l_suppkey != l1.l_suppkey)     -- a correlated INEQUALITY
NOT EXISTS (SELECT * FROM lineitem l3
            WHERE l3.l_orderkey = l1.l_orderkey     -- a join key
              AND l3.l_suppkey != l1.l_suppkey      -- a correlated INEQUALITY
              AND l3.l_receiptdate > l3.l_commitdate)   -- body-local, fine
```

So q21 is **not** blocked by anything Week 33 declined for lack of a key: both
bodies *have* a key. It is blocked by the residual that sits beside it. The
requirement is precisely: **a semi/anti join that carries an `ON` residual over
both sides.**

The mutation check confirms the data will test it:
`('DISCRIMINATING', 'the NOT EXISTS anti-join', '3 rows -> 3')`, and SQLite's
answer is 3 rows (`Supplier#000000044 | 9`, `...054 | 7`, `...013 | 4`).

### Conceptual explanation

`R ⋉_(p ∧ q) S ≡ R ⋉ over σ… ` does **not** decompose the way an inner join's
does. For an INNER join, `R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S)` — which is the identity
`join_condition.h` relies on to fold residuals into the `WHERE`. For a **semi**
join the identity fails, because the semi join has already collapsed the
matching build rows to a yes/no answer: by the time `q` could be applied, the row
that would satisfy it is gone. The residual must be evaluated **inside the
probe**, against a probe⊕build pair, exactly like an outer join's residual —
which the operator already does.

Three properties of the current operator are what make this a real change rather
than a flag:

1. **The semi/anti build side keeps keys, not rows.** `VecHashJoinNode::open`
   inserts into `build_keys_` (an `unordered_set<std::string>` of serialized
   keys) for `semantics_ != STANDARD`, and into `hash_table_`
   (`key -> vector<Row>`) otherwise. A residual needs the *rows*. The machinery
   exists — it is the STANDARD path's — so this is a routing change, not a new
   data structure.
2. **The probe's residual is evaluated against `output_schema_`, which for a
   semi/anti join IS the probe schema.** `passes()` calls
   `evaluate(on_residual_.get(), row, output_schema_)`. A residual naming a body
   column cannot resolve there. It needs a **private** `residual_schema_` =
   probe schema ⊕ projected-body schema, and a concatenated row to evaluate
   against. The output schema must **not** move: that it is the probe's schema
   is the containment Week 32 established and Week 33 preserved, and widening it
   would put body columns in scope above the join.
3. **The body is projected to its key columns only.** `lowerExistsSubqueries`
   does `body.select_list = std::move(body_key_refs)` — deliberately, and for
   three named defects (round 1 H-1/H-2/M-3). A residual's body-side columns must
   therefore be **added to that projection**, after the keys, so the right key
   indices stay positional `0..k-1`. Appending, never inserting.

And one hazard that is specific to q21 and easy to miss: the residual compares
`l3.l_suppkey` with `l1.l_suppkey` — **two columns with the same name** from two
aliases of the same table. In a merged residual schema, `indexOf(name)` takes the
first match. This is exactly the wrong-relation class `ColumnId` exists to
prevent, and it is the reason the residual's refs must be **restamped by slot**
rather than left to resolve by name. Getting this wrong produces wrong rows, no
error, and an identical `--explain` — the H-1 failure shape, verbatim.

### Code snippets (illustrative)

`splitCorrelation` currently refuses a correlated non-equality outright. The
change routes it instead, and only when a key survives:

```cpp
// Week 36 — a correlated conjunct that is NOT an equality becomes an ON RESIDUAL
// on the semi/anti join, evaluated inside the probe against a probe(+)build pair.
// TPC-H Q21 needs exactly this: `l2.l_suppkey != l1.l_suppkey` beside the key
// `l2.l_orderkey = l1.l_orderkey`.
//
// !! STILL REFUSED when NO equality survives (the `keys.empty()` check at the
// call site): a body correlated only by inequalities has no hash key at all, and
// the fallback would be a cross product this engine has no operator for. The
// refusal narrows; it does not disappear.
if (!bin || bin->op != "=") {
    correlated_residuals.push_back(std::move(c));   // was: refuse(...)
    continue;
}
```

The operator's constructor assertion is the site the standing rule points at:

```cpp
if (semantics_ != JoinSemantics::STANDARD) {
    ...
    // WAS: if (on_residual_) throw "a semi/anti join takes no ON residual";
    // Week 36: a SEMI/ANTI join MAY carry a residual (Q21). ANTI_NOT_IN may NOT:
    // its build_had_unmatchable_key_ short-circuit answers "S contains a NULL, so
    // `x NOT IN S` is never TRUE" -- a statement about the KEY column that a
    // residual makes untrue, because a build row with a NULL key can no longer
    // stand for "some row matched". `NOT IN` never produces a residual, so this
    // is a containment, not a limitation.
    if (on_residual_ && semantics_ == JoinSemantics::ANTI_NOT_IN)
        throw std::runtime_error(
            "internal: a NOT IN anti-join takes no ON residual");
    // The output schema assertion STAYS, unchanged and load-bearing: the residual
    // is evaluated against residual_schema_, which is PRIVATE. No body column
    // enters output_schema_, so nothing above the join can name one.
    if (output_schema_.size() != probe_child_->outputSchema().size()) ...
}
```

and the probe, where SEMI and ANTI part company:

```cpp
// SEMI  — emit the probe row iff SOME build row with this key passes the residual.
// ANTI  — emit it iff NO build row with this key passes. Not the negation of a
//         boolean computed elsewhere: it is the same scan with the opposite
//         verdict, so a residual that is UNKNOWN (NULL) counts as NOT passing on
//         BOTH sides. EXISTS is two-valued; there is no third answer to carry.
bool matched = false;
auto it = hash_table_.find(key_buf_);
if (it != hash_table_.end()) {
    for (const Row& build_row : it->second) {
        if (residualPasses(probe_row, build_row)) { matched = true; break; }
    }
}
if (matched != (semantics_ == JoinSemantics::SEMI)) continue;
```

Note the early `break`: a semi join emits each probe row **at most once**, which
is the whole reason the operator exists, and the loop must not turn into an
inner join's multiply-emitting one.

### Implementation guidance

1. **Stage it, and put the decision gate first.** Task 3a — reproduce the
   refusal, write the requirement down (this section), and confirm the mutation
   verdict — is **mandatory** and cheap. Task 3b — implement it — is the stretch.
   If 3b does not land, 3a still ships: the refusal message gets sharpened to
   name what it declined (`a correlated inequality beside a valid key is an ON
   residual on the semi/anti join, which is not implemented`), the boundary is
   pinned in the rejection suite, and q21 stays UNPORTED with a recorded reason
   instead of an unexplained zero.
2. **Do not touch `requireDecorrelatableBody`'s conditions 1, 3 or 4.** Only
   condition 2 (every correlated conjunct is an equality) narrows. The header
   spells out all four; rewrite condition 2 there in the same edit — the same
   rule as Task 2.
3. **Order of work inside 3b:** guard first (routing), then the logical join
   (carry `on_residual` on a `SEMI`/`ANTI` node, keep `join_slot = -1`), then the
   body projection (append residual columns after the keys), then
   `VectorizedPlanBuilder` (pass the residual through, build `residual_schema_`),
   then the operator. Build and run the existing suites after **each** step: the
   semi/anti path is shared with `IN` lowering (q16, q18, q20) and Week 33's
   `EXISTS` (q4, q22), and a regression there costs more than q21 gains.
4. **The `build_had_unmatchable_key_` flag is not the only NULL question.** With
   a residual, a build row whose *key* is NULL is still unmatchable (correct,
   unchanged), but a build row whose *residual column* is NULL now makes the
   residual UNKNOWN — which `passes()` already treats as not-a-match via
   `!v.isNull() && v.asInt() != 0`. Confirm that by test, do not assume it: for
   `NOT EXISTS`, "the residual was UNKNOWN" must keep the outer row, and it is
   one sign flip away from dropping every row (the Week 33 round-2 failure).
5. **Watch the join count.** After both lowerings q21 has three base joins plus a
   semi plus an anti — five joins in one vectorized plan, more than any query
   shipped so far. Check `join-ordering=` in `--explain` says something sensible
   and that `hasSlotOutsideRangeTable` does not silently decline the tree: a
   `join_slot = -1` right child is expected and handled, but five joins is a new
   scale for it.
6. **Do not convert the semi/anti probe's row copy to a `SelectionVector` here.**
   That is a *measurement* question (README's starting note under Week 37) and
   changing it in the same week as the residual would confuse a correctness
   change with a performance one.

### Verification

- **The refusal narrows correctly.** A body correlated **only** by an inequality
  (`WHERE l2.speed > l.speed`) must still refuse, with the `keys.empty()`
  message. That entry already exists in `WEEK34_CORRELATED_SCALAR_REFUSED`;
  confirm it still fires and now reports the *no key* message rather than the
  *no equality* one — the message changes, so the pin must change with it.
- **q21 matches SQLite** in both vectorized modes: three rows, `numwait`
  9 / 7 / 4 for `Supplier#000000044 / 054 / 013`, ordered
  `numwait DESC, s_name`.
- **The semi/anti family does not regress.** q4, q16, q18, q20, q22 must all
  still be MATCH in their two modes, and the twelve rejection suites must still
  report every entry reaching its own guard. This is the check that matters most:
  q21 is worth one cell, and the shared operator is worth ten.
- **Hand-simulate the ANTI residual on paper before trusting the run.** Take one
  order with two suppliers, one late and one not, and walk the four combinations
  (key match / no key match) × (residual passes / fails) through the probe.
  `operator-correctness`'s NULL tables are the reference. A semi/anti join is the
  one operator in this engine where a wrong answer is a *missing* row, and a
  missing row is invisible in a spot check.

---

## Task 4 — Mode coverage: settle Volcano semi/anti parity on the measured breakdown

### Why it matters

The report distinguishes two figures — **how many queries are meaningfully
answered** and **in how many of the four modes** — because collapsing them is how
a documented limitation becomes a silent claim of full coverage. Week 36 has to
say which of the two it is moving, and back it with a measurement rather than an
expectation. The standing expectation, carried since Week 33, is that *Volcano
semi/anti parity would move many queries from two modes to four*. **That
expectation is wrong, and the report already contains the data that disproves
it.**

### The measurement

34 of 88 cells are Volcano refusals. Classified by the message each cell actually
recorded in `docs/tpch-sf0.01-report.json` — not by which feature the query
"is about":

| Guard that fired | Queries | Cells |
|---|---|---|
| `multi-way joins are not supported on the Volcano path` | q2 q3 q5 q10 q11 q18 q21 | **14** |
| `derived tables (FROM (subquery)) ...` | q7 q8 q9 q13 q15 q22 | **12** |
| `IN subqueries are lowered to a semi-join ...` | q16 q20 | **4** |
| `correlated subqueries are decorrelated to a semi-join ...` | q4 q17 | **4** |
| | | **34** |

Reproduce it before quoting it:

```python
import json
d = json.load(open("docs/tpch-sf0.01-report.json"))["details"]
for k, v in sorted(d.items()):
    if "volcano" in k:
        print(k, "::", str(v)[:80])
```

So the semi/anti family is **8 cells of 34**, not most of them. And of those 8,
six are unreachable even with a Volcano semi-join, because the query needs a
*second* join that `Planner::plan` cannot express:

- **q16** — `partsupp JOIN part` **plus** the `NOT IN` anti-join.
- **q20** — `supplier JOIN nation` **plus** two nested `IN` semi-joins.
- **q17** (after Task 1) — `lineitem JOIN part` **plus** the decorrelated LEFT
  join. It would hit `stmt.joins.size() > 1` first anyway.
- **q4** — `FROM orders` with no explicit join at all. The decorrelated semi-join
  is the **only** join. This is the one reachable case: **2 cells**.

### Conceptual explanation — what those 2 cells would cost

`Planner::plan` is the Volcano entry point and it builds exactly one
`HashJoinNode` out of `stmt.joins`. `HashJoinNode` has **no `JoinSemantics`
parameter** — `plan_nodes.h` says so explicitly and says why. And critically,
this path does **not** run `LogicalPlanBuilder`, which is where decorrelation
grafts the body's subtree. So closing q4's two cells means:

1. adding `JoinSemantics` to `HashJoinNode`, and with it the three-valued
   `SEMI` / `ANTI` / `ANTI_NOT_IN` rules, the `build_had_unmatchable_key_`
   short-circuit, and the empty-build-side case;
2. a **second decorrelation production** on a path with no logical layer, which
   must agree with the first on `NOT EXISTS`'s NULL semantics.

Point 2 is, in this project's own words, "the two-paths drift Weeks 26/28/30 each
had to undo". The cost is two engine paths that must agree forever; the benefit
is 2 cells of 88.

### The decision, stated so it does not have to be re-argued

**Week 36 targets the headline count, not mode coverage.** Volcano semi/anti
parity is re-declined, now on a measurement rather than on an estimate of
difficulty:

- it addresses **8 of 34** Volcano refusal cells, not most of them;
- **6 of those 8** are additionally blocked by a plan shape `Planner::plan`
  cannot express, so parity alone changes nothing for them;
- the reachable remainder is **q4's 2 cells**, at the price of a second
  execution production for a family whose NULL rules already produced one
  whole-result-set defect (Week 33 round 2).

The two guards that actually dominate — multi-way joins (14 cells) and derived
tables (12 cells) — are both **deliberately** vectorized-only, stated in
`planner.cc` as "Volcano is the correctness baseline, not the feature-complete
path". Nothing this week changes that, and nothing should: Volcano's value is
being a *different* implementation to diff against, and the 54 cells it does
cover are what make that oracle meaningful.

### Implementation guidance

There is no code in this task. What it produces is:

1. **The table above, in the Week 36 report**, so the next reader inherits the
   breakdown instead of the expectation.
2. **A correction to the carried-forward note.** README's Week 33/34 hand-forward
   says parity "would move many queries from two modes to four". Rewrite it to
   the measured claim. Leaving it is how a wrong expectation survives three more
   weeks.
3. **A restatement in `planner.cc`'s cost paragraph.** It currently says "three
   families of query the four-mode oracle does not cover, and the count is stated
   in README Limitations so the week it tips is visible". Add which family costs
   what — 14 / 12 / 8 — so "the week it tips" is a number, not a feeling.

### Verification

- The four counts must sum to 34, and 34 + 54 must equal 88. Arithmetic, but it
  is the arithmetic the whole claim rests on.
- Re-run the classification **after** Tasks 1 and 3, not before: q17 leaves the
  `correlated` row and q21 leaves the `multi-way` row only if q21 lands. The
  table published in the report must be the post-change one.
