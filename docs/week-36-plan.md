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
