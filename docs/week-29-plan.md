# Week 29 — Outer Join

> Teaching plan. Nothing here is written into `src/`; every snippet is
> illustrative and matches the naming, headers and conventions already in the
> tree. Read the whole "Design" half before writing a line: the hard part of
> this week is not the operator, it is the four *existing* passes whose
> correctness arguments were written for inner joins and stop holding.

README bullets:

- Add logical and vectorized left outer hash join
- Preserve unmatched rows and stable output slots

**Checkpoint:** TPC-H Q13 join semantics are supported.

Q13, for reference, is the reason the checkpoint is worded that way:

```sql
select c_count, count(*) as custdist
from ( select c_custkey, count(o_orderkey)
       from customer left outer join orders
         on c_custkey = o_custkey
        and o_comment not like '%special%requests%'
       group by c_custkey ) as c_orders (c_custkey, c_count)
group by c_count order by custdist desc, c_count desc;
```

Three separable things live in that query. **This week owns exactly one of
them** — `left outer join ... on <equi-key> and <residual>`, with unmatched
left rows preserved and `count(o_orderkey)` returning 0 (not 1) for them. The
derived table is Week 34. The `(c_custkey, c_count)` column list is Week 34.
Do not build either.

The F1-schema analogue, which is what you will actually test against SQLite:

```sql
SELECT d.name AS n, COUNT(l.lap_id) AS c
FROM drivers d LEFT JOIN laps l
  ON d.driver_id = l.driver_id AND l.speed > 340
GROUP BY d.name ORDER BY c DESC, n
```

Every driver appears. A driver with no lap over 340 appears once with `c = 0`.
Move the `l.speed > 340` conjunct into a `WHERE` and that driver disappears —
that is the semantic difference this week exists to represent, and it is one
token wide in the SQL text.

---

## What Week 29 must deliver

1. `LEFT [OUTER] JOIN` parses, binds and validates, at the same relation counts
   inner joins already reach on each path.
2. Unmatched preserved-side rows are emitted once, null-extended across the
   null-supplying side's columns, in the fixed `[FROM, JOIN]` schema order
   (invariant 1).
3. A preserved-side row whose join key is NULL (or NaN) is *still emitted* —
   it matches nothing, which is precisely the outer-join case.
4. `ON` residuals on an outer join filter the **match test**, not the result.
5. Predicate pushdown never crosses to the null-supplying side.
6. Join enumeration declines any tree containing an outer join.
7. `EXPLAIN` names the join type; `EXPLAIN ANALYZE`'s row counts stay honest.
8. Optimized ≡ `--no-optimize` ≡ SQLite on every new shape (invariant 11).

## What not to build

| Not this week | Why |
|---|---|
| `RIGHT` / `FULL OUTER` joins | No TPC-H query in the documented dialect needs them. A `RIGHT` join is a `LEFT` with the operands swapped, and swapping operands changes the merged schema's column order — invariant 1 says output order never follows the physical side, so `RIGHT` needs its own null-extension direction, not a flag flip. Nothing to gain, one more shape for every pass below to be right about |
| Outer-join-aware reordering (Moerkotte conflict/eligibility sets, TES/SES) | The DP would need per-edge conflict rules before it can legally move anything. That is a week of its own, and it buys nothing until a supported query has an outer join *inside* a reorderable block. Declining is sound and costs one function (Task 5) |
| Null-rejecting-predicate simplification (`σ_p(R ⟕ S) ≡ σ_p(R ⋈ S)` for null-rejecting `p` over `S`) | A real optimization and the standard answer to "why is my LEFT JOIN slow", but it is an *added* rewrite on top of a correct implementation, and every rewrite here is a chance to produce rows nobody asked for. Note it in the plan file, not in the code |
| Pushing an outer join's `ON` residual down to the null-supplying scan | Legal for a residual referencing only that relation (it only ever removes *matches*, and unmatched left rows are preserved either way), but it is an optimization with no measurement behind it and a second place to be right about residual placement. Task 2 keeps all outer residuals on the join |
| Subqueries, semi-joins, derived tables | Weeks 30–34. Q13's outer half is this week; its derived table is not |
| Giving `ColumnarTable` a NULL representation | Invariant 14. Task 8b explains why the NaN-at-load note cannot be closed without it, and why that is a storage week's work |

---

## Prerequisite knowledge

Flag these to yourself before starting; the tasks assume all five.

1. **SQL three-valued logic.** `NULL > 30` is UNKNOWN, and a `WHERE` keeps a
   row only on TRUE. The project's convention is boolean-as-INT with an
   explicit null test: `!v.isNull() && v.asInt() != 0` (see `columnar_eval.cc`
   line 124 and `evaluate()`'s filter callers). UNKNOWN is not a match.
2. **Null extension.** `R ⟕_p S` = `(R ⋈_p S)` ∪ `{ r ++ NULL^|S| : r ∈ R,
   ¬∃s ∈ S. p(r,s) }`. Read it twice: the second term is quantified over *all*
   of `S` and *all* of `p`, which is why a residual inside `p` and a residual
   above the join are different queries.
3. **The project's slot arithmetic.** `relation_slot` is the binder range-table
   position (`joins[i]` → slot `i+1`); `JoinKey::from_slot` is the slot *as
   presented by the left child's own schema* (`join_condition.h`); a merged join
   schema stamps each block with its relation's slot. Nothing this week changes
   any of it — but Task 4 and Task 5 both turn on which side of a join a slot
   sits on.
4. **Build/probe is not left/right.** `swapped_` is a physical cost decision
   (Week 22); `[FROM, JOIN]` output order is logical and fixed (invariant 1).
   For a left outer join the two stop being independent — Task 6.
5. **Where NULL is already modelled.** Next section. Verify it yourself before
   relying on it; the README's claim is true but not complete.

---

## Design — the identities, and which ones break

This is the load-bearing part of the week. Six identities; the engine's existing
passes each rest on one of them, and the right-hand column is where the
inner-join proof stops applying.

| # | Identity (inner) | Holds for `⟕`? | Mechanism that rests on it |
|---|---|---|---|
| I1 | `R ⋈ S ≡ S ⋈ R` (commutativity) | **No** | `JoinEnumeration` — the DP's transitions add a relation to a subset in any order |
| I2 | `(R ⋈ S) ⋈ T ≡ R ⋈ (S ⋈ T)` (associativity) | **No, in general** | `JoinEnumeration::rebuild` — re-folding the same edge set in a different order |
| I3 | `σ_p(R ⋈ S) ≡ σ_p(R) ⋈ S`, `p` over `R` | **Yes for the preserved side, no for the null-supplying side** | `PredicatePushdown::distribute` |
| I4 | `R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S)` (ON ≡ WHERE) | **No** | The residual fold in `LogicalPlanBuilder::build` and `Planner::plan` |
| I5 | A NULL key matches nothing, so drop the row | **Half** — it still matches nothing, but the row must be emitted | `serializeKey(...) == false → continue` in both hash joins |
| I6 | Build side is a free cost choice | **No** | Week 22's `from_builds` decision in `VectorizedPlanBuilder` |

### I1 / I2 — why enumeration must decline, not adapt

`R ⟕ S` keeps every row of `R`; `S ⟕ R` keeps every row of `S`. They are not
the same relation and generally not even the same arity ordering. Associativity
fails too: with `p` over `R,S` and `q` over `S,T`,

```
(R ⟕_p S) ⋈_q T   ≠   R ⟕_p (S ⋈_q T)
```

— an `r` with a match in `S` that fails `q` is eliminated on the left and
null-extended on the right.

Week 28's DP is built on the assumption that a subset's plan can be finished in
any order (`join_enumeration.h`, approximation 3). That assumption is not
repairable by a better cost model; it is a *legality* question, and the standard
answer (Moerkotte's conflict/eligibility sets) is a different algorithm. The
Week 28 Starting note says this exactly: "an unguarded outer join in the graph
is a wrong answer, not a bad plan." So: **decline the tree** (Task 5). Declining
is already the file's idiom — it is what `n < MIN_ENUMERATED_RELATIONS` and
`n > 32` do, and it keeps optimized ≡ `--no-optimize` intact by construction.

### I3 — the pushdown asymmetry, concretely

`predicate_pushdown.h` already carries the warning ("Week 29's outer join must
revisit this — a predicate on the null-supplying side cannot be pushed through
an outer join"). Here is the row that proves it, on the shipped catalog:

```sql
SELECT COUNT(*) FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id
WHERE l.season = 2024
```

- **Correct:** join, null-extend the drivers with no lap, then apply
  `l.season = 2024`. A null-extended row has `l.season = NULL`, `NULL = 2024` is
  UNKNOWN, the row is dropped. Result: only real 2024 laps.
- **Pushed:** filter `laps` to season 2024 *first*. A driver whose only laps are
  from other seasons now has **no** match, so the outer join null-extends it —
  and there is no longer a filter above to drop it. Result: extra rows, every
  one of them a driver the query asked to exclude.

More rows, no error, and both plans look reasonable in `--explain`. The
preserved side is fine — `σ_p(R) ⟕ S ≡ σ_p(R ⟕ S)` for `p` over `R`, because
null-extension never touches `R`'s columns and never invents an `R` row.

### I4 — ON vs WHERE, which is Q13

For an inner join the two are interchangeable, which is why Week 27 could fold
`ON` residuals into the `WHERE` conjunction and get pushdown for free
(`logical_plan.cc`, and the `!! Week 29` marker in `join_condition.h`). For a
left outer join:

```
R ⟕_(p∧q) S     — a row failing q is NOT a match, so r is null-extended
σ_q(R ⟕_p S)    — a row failing q was a match, so r is not null-extended,
                  and the joined row is then deleted: r disappears entirely
```

Q13's `o_comment not like '%special%requests%'` is exactly `q`. Under the fold,
a customer whose only orders are "special requests" would vanish from the
result; under correct semantics they appear with `c_count = 0`, which is the
biggest bucket in the reference answer. **The fold is the bug**, and Week 28's
Starting note demands it be split apart *before* enumeration — which Task 5
answers by not enumerating at all when an outer join is present.

### I5 — the NULL key, which is the one-line wrong answer

Both hash joins do this today:

```cpp
if (!serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_)) continue;   // NULL matches nothing
```

The comment is *true* and the action is *wrong* for an outer join. "Matches
nothing" is the definition of the row that must be null-extended. `key_encoding.h`
was written to make this the caller's decision ("NULL is where the three uses
legitimately differ, so it stays the caller's decision"), and this week is the
first caller that needs the third answer: **unmatchable, but emitted**. Do not
touch `key_encoding.h`; touch the two `continue`s.

Note that the same line covers NaN via `isUnmatchableKey`. A NaN preserved-side
key is emitted null-extended, which is also what SQLite does (it stores NaN as
NULL, and a NULL key is unmatchable) — so the outer join *narrows* the NaN
divergence rather than widening it. See Task 8b.

### I6 — the build side stops being free

Week 22 chooses which input builds the hash table from cost. A left outer join
has two different algorithms depending on the answer:

- **preserved side probes** (`swapped_ == false`): on a miss, emit
  null-extended. One extra branch, no extra state.
- **preserved side builds** (`swapped_ == true`): the probe stream is the
  null-supplying side, so unmatched preserved rows are only known at
  end-of-probe — you need a per-build-row matched flag and a drain pass, and the
  drain has to emit in a stable order.

Minimum code that solves the problem: **force the preserved side to probe** and
skip the build-side decision for outer joins. The cost: when the null-supplying
side is much larger than the preserved side, we hash the large side. That is a
real, statable regression on a shape no supported query has yet (Q13's `orders`
is larger than `customer`, and that is the *forced* direction anyway — customer
probes, orders builds is wrong; customer probes means **orders builds**, which
is the large side. Say so honestly in the plan and in `--explain`.). Revisit
under measurement in Week 37, not on a hunch now.

### Verifying the Week 24 validity-mask claim

The README asserts Week 24 modelled NULL natively "specifically to make the
Week 29 outer join implementable". Checked against the tree, that is true, and
here is the evidence — plus the part it does not cover, which you must not
assume away.

**True, representation side** (`src/execution/vec_types.h`):
`ColumnVector::all_valid` + `validity`, with the invariant that `all_valid`
means the mask is empty. `appendColumnValue` back-fills the all-valid prefix on
the first NULL and pushes a placeholder into the dense typed vector; `valueAt` /
`readColumnValue` read through the mask. **`VecHashJoinNode::fillOutChunk`
already builds its output columns with `appendColumnValue`** — so a
`Value::null()` sitting in `output_buffer_` becomes a real NULL in the emitted
chunk with *zero* changes to the materialization path. That is the claim, and it
holds.

**True, consumer side** — the operators a null-extended row flows into:

| Consumer | Evidence |
|---|---|
| `ExpressionExecutor` | `propagateNulls` / `markNull` (`expression_executor.cc` ~89–104); every typed kernel tests `n.out.isNull(i)` before writing |
| `columnar_eval::scanColumn` | branches on `cv.all_valid` and tests `cv.validity[r]` in the slow path (lines 17–40) |
| Predicate → selection vector | `!out.isNull(i) && flags[i] != 0` (line 110) — UNKNOWN is not selected, which is what makes "don't push the predicate" produce the right answer |
| `VecHashAggregateNode` | `if (val.isNull()) continue;` then `COUNT` reports `sa.non_null_count` — this is precisely why Q13's `count(o_orderkey)` is 0 and `count(*)` is 1 for an unmatched row |
| `VecSortNode` | `compareForSort`, a total order over NULL (Week 24) |
| `VecDistinctNode` | `appendGroupKeyField(key, valueAt(cv, r))` — NULL is its own group, marked `'N'` |
| `VecLimitNode` | truncates `validity` alongside the data (line 45) |

**What the claim does not cover — three things:**

1. **The join's own key handling.** `key_encoding.h` deliberately leaves the
   NULL policy to the caller, and the current caller drops the row (I5). The
   mask does not help you here; the branch does.
2. **Invariant 14.** `ColumnarTable` cannot express NULL, so until this week no
   NULL could enter a query from ordinary catalog data. Every NULL path was
   exercised only by in-memory operator tests and by NULL-*producing*
   expressions (`x / 0`, `CASE` with no `ELSE`). **This week is the first time
   `compare_against_sqlite.py` can be a NULL oracle on real data** — which makes
   the harness entries in Task 10 more valuable than the unit tests.
3. **Volcano's Row path** carries NULL through `Value::null()` and always has,
   but its `DISTINCT` key serializer only learned the NULL marker in Week 27.
   If you implement Task 7, you are putting real NULLs onto the Volcano baseline
   for the first time too.

---

## The Starting notes, and where each is answered

### From Week 27's foundations

| Bullet | Answer |
|---|---|
| An `ON` clause comparing a STRING column to a numeric one is accepted and silently half-matches, while the identical predicate in `WHERE` throws | **Task 8a** — a plan-time key-type check in `Validator`, covering both engines from one place. Landed now because an outer join turns a half-match into *null-extended output that looks legitimate*: the rows the type confusion loses come back as NULL-padded rows instead of being merely missing |
| NaN reaches keys as its own group where SQLite has none; converting at load in `CSVLoader` would align all three | **Task 8b — deferred, with the reason made concrete.** `CSVLoader::parseField` returning `Value::null()` for NaN cannot work today: `CSVToColumnar::convert` calls `row[c].asDouble()` unconditionally, and `ColumnarTable` has no NULL representation (invariant 14). Closing this means giving columnar storage a validity mask — a storage week's work that touches every scan. Week 35 owns the loader and a second storage format; it belongs there. The outer join does not make it more urgent — see I5 |
| Both are dialect facts until closed, listed in Limitations, inherited by the Week 36 report | **Task 8** updates Limitations for whichever of the two you close, and leaves the other's bullet in place with the sharper reason |

### From Week 28's foundations

| Bullet | Answer |
|---|---|
| `cost=` and `est=` on the same `--explain` line are deliberately not reconcilable, and `join_enumeration.h`'s approximation list does not say so | **Task 5** adds the one line to approximation 3 — and Task 5's own new rule follows the same discipline: the outer-join `max(rows, left_rows)` clamp goes at the **stamping** site only, never inside `joinCardinality`, because it is not multiplicative |
| The two written-order-bound assertions cannot fail; assert `method=dp` instead, and fix it before adding cases | **Task 9** — fixed *first*, because this week adds the case where the correct outcome is that no `order=` line is printed at all, and a tautological assertion cannot tell that from a decline that never fired |
| The unbound-key throw is the one place the optimized path can fail on input `--no-optimize` accepts; Week 30 owns it | **Task 5** does not touch it. It does add a second decline, in the `return node;` shape the note recommends, so the precedent Week 30 needs is in the file next to it |
| `method=written-floor` has never executed; one stats-less-table fixture exercises it and `joinCardinality`'s `max(l, r)` branch | **Task 9** adds that fixture while it is already in `test_join_enumeration.cc` fixing the assertion. Same 20 lines cover the new outer-join decline |

---

## Task 1 — `LEFT [OUTER] JOIN` in the lexer, parser and AST

### Why it matters

Everything downstream keys off one bit. Getting it into the AST *as a
per-clause property* — not a statement-level flag — is what lets a query mix
inner and outer joins (`A JOIN B ... LEFT JOIN C ...`), which is the shape Task
4 and Task 5 have to reason about. A statement-level flag would be the same
mistake `std::optional<JoinClause>` was before Week 26: a type-level encoding of
an assumption that does not survive the next week.

Downstream: `Binder` needs no change at all (the range table is positional and
join type does not affect name resolution); `Validator` needs no change for
parsing but gains Task 8a; `LogicalPlanBuilder` and `Planner` read the new
field in Task 3 and Task 7.

### Conceptual explanation

`LEFT` and `OUTER` become reserved keywords. The lexer's `KEYWORDS` map carries
the project's own warning: "Every keyword added here is a column name taken
away." Check it: neither collides with `catalog.json` (`lap_id, driver_id, team,
speed, sector_1..3, season, round, name, nationality, age`) nor with the TPC-H
schema, whose columns are all prefixed (`l_`, `o_`, `c_`, `ps_`, …).

The alternative — matching `LEFT` as IDENTIFIER text, the way interval units
(`day`/`month`/`year`) are matched so `year` stays a usable column name — is
**wrong here**, and for a specific reason. `Parser::parseSelect`'s bare-alias
branch is:

```cpp
} else if (check(TokenType::IDENTIFIER) &&
    !check(TokenType::JOIN) && !check(TokenType::WHERE) && ... ) {
    stmt.from_alias = consume().value;
}
```

If `LEFT` lexed as an IDENTIFIER, `FROM laps LEFT JOIN drivers ...` would
consume `LEFT` as the FROM table's **alias**, and then fail at `JOIN` with a
message about neither. As a keyword, `check(TokenType::IDENTIFIER)` is already
false and the branch does not fire — no exclusion-list edit needed. (The
existing `!check(TokenType::JOIN)` clauses in that list are dead for the same
reason; leave them, they are not yours to clean up.)

`INNER JOIN` is deliberately **not** added: nothing needs it, and it would take
a third identifier away for zero behaviour.

### Code

```cpp
// src/parser/token.h — in the keyword block
    // Week 29. LEFT/OUTER are reserved rather than matched as identifier text
    // (the way interval units are) because Parser::parseSelect's bare-alias
    // branch would otherwise read `FROM laps LEFT JOIN ...` as the alias `LEFT`.
    // Verified against catalog.json and the TPC-H column names: no collision.
    // INNER is deliberately absent — nothing needs it, and it is one more
    // identifier taken away.
    LEFT, OUTER,
```

```cpp
// src/parser/lexer.cc — Lexer::KEYWORDS
    {"LEFT",   TokenType::LEFT},
    {"OUTER",  TokenType::OUTER},
```

```cpp
// src/parser/ast.h

// Week 29. Per-CLAUSE, not per-statement: a query may mix inner and outer joins
// (A JOIN B ... LEFT JOIN C ...), and predicate pushdown and join enumeration
// both have to ask the question one join at a time. INNER is the default so
// every existing brace-init and every hand-built test tree keeps its meaning.
enum class JoinType { INNER, LEFT };

struct SelectStatement {
    ...
    struct JoinClause {
        std::string join_table;
        std::string alias;
        std::unique_ptr<Expr> condition;
        // LAST field, so positional brace-inits that predate Week 29 stay valid
        // — the same discipline AggregateSpec::argument and GroupByColumn::expr
        // follow.
        JoinType type = JoinType::INNER;
    };
```

```cpp
// src/parser/parser.cc — replace `while (match(TokenType::JOIN))`

// `check`, not `match`: the loop head now has two entry tokens. JOIN / LEFT are
// both their own TokenTypes, so no keyword-exclusion list is needed anywhere.
while (check(TokenType::JOIN) || check(TokenType::LEFT)) {
    SelectStatement::JoinClause join;

    if (match(TokenType::LEFT)) {
        match(TokenType::OUTER);            // optional noise word, as in SQL
        expect(TokenType::JOIN, "JOIN after LEFT [OUTER]");
        join.type = JoinType::LEFT;
    } else {
        consume();                          // the JOIN token
    }

    join.join_table = expect(TokenType::IDENTIFIER, "join table name").value;
    ... // alias, ON, condition — unchanged
    stmt.joins.push_back(std::move(join));
}
```

### Implementation guidance

1. Add the tokens and keywords first and run the existing test suite. Reserving
   two words is the one change here that can break an unrelated query; if
   anything fails, it is a name collision and you want to know before writing
   the parser branch.
2. `match(TokenType::OUTER)` with no `expect` afterwards is intentional — SQL's
   `OUTER` is a noise word. `LEFT OUTER JOIN`, `LEFT JOIN` and (rejected)
   `LEFT drivers` must all behave: the third is a parse error naming `JOIN`.
3. Do **not** thread `JoinType` into `Binder`. Resolution is positional and
   unaffected. Resist the urge to "pass it along for later".
4. Gotcha: `Parser::parse` requires end-of-input (Week 25). A typo like
   `RIGHT JOIN` lexes `RIGHT` as an IDENTIFIER and dies with
   `unexpected trailing input after the end of the query` rather than
   "RIGHT JOIN is not supported". That is acceptable (SwiftQL's non-support
   messages are elsewhere too), but if you want a better message, the place is a
   check in `parseSelect` for an IDENTIFIER whose upper-cased text is
   `RIGHT`/`FULL` immediately before a `JOIN` token — optional, and say so in
   Limitations either way.

### Verification

```bash
cmake --build build -j$(nproc)

# parses, three spellings
./build/swiftql --catalog catalog.json --no-cache --storage columnar \
  --execution vectorized --explain \
  --query "SELECT d.name AS n FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id"
./build/swiftql ... --query "... d LEFT OUTER JOIN laps l ON ..."

# still an ordinary inner join
./build/swiftql ... --query "... d JOIN laps l ON ..."

# LEFT is not an alias
./build/swiftql ... --query "SELECT * FROM laps LEFT JOIN drivers ON ..."   # must plan, not error on `LEFT`
```

Add to `tests/test_parser.cc`: one test asserting `stmt.joins[0].type ==
JoinType::LEFT` for both spellings, one asserting `INNER` for a bare `JOIN`, and
one asserting `LEFT drivers d ON ...` (no `JOIN`) raises a `ParseError` naming
`JOIN`.

---

## Task 2 — Stop folding `ON` residuals into `WHERE` for an outer join

### Why it matters

This is I4, and it is the difference between Q13's answer and a different
query's answer. Week 27 folded residuals into the `WHERE` conjunction for a
reason that is still valid *for inner joins* and is written down in
`logical_plan.cc`: `PredicatePushdown` only rewrites a `FILTER` whose **direct**
child is a `JOIN`, so a second stacked filter node would leave every joined
query's `WHERE` unpushed. Keep that. Add the outer-join case beside it.

Downstream: Task 3 stores the outer residual on the `LogicalJoin`; Task 6
evaluates it inside the probe loop; Task 4 never sees it (it is not a `WHERE`
conjunct any more); Task 5 declines the tree anyway.

### Conceptual explanation

`classifyJoinCondition` already returns keys and residuals **separately** —
which means **no change to `join_condition.cc` is required at all**. The header
comment marked `!! Week 29 (outer join) must revisit this` is pointing at the
*policy*, and the policy lives in the two callers:

- `LogicalPlanBuilder::build` (`logical_plan.cc`) — vectorized path
- `Planner::plan` (`planner.cc`) — Volcano path

For an inner join: clone residuals into `on_residuals`, fold into `stmt.where`
(unchanged). For an outer join: clone them into one conjunction and hand it to
the join node, where it becomes part of the match test.

Two properties worth stating explicitly, because both are easy to get wrong:

- A residual referencing the **preserved** side (`ON k = k AND d.age > 30` on
  `drivers d LEFT JOIN laps l`) also stays in the `ON`. It filters which
  `(d, l)` pairs count as matches; a `d` failing it is *still emitted*,
  null-extended. Do not special-case it.
- A residual referencing **two** relations behaves the same way. There is no
  slot routing here at all — the outer residual has exactly one home.

The forward-reference rule (`classifyJoinCondition` step 1) and the
"at least one equi-key" rule are unchanged. A `LEFT JOIN` with no equi-key is
still refused, for the same reason: there is no cross-product operator to run it
on.

### Code

```cpp
// src/planner/join_condition.h — replace the `!! Week 29` marker with the
// resolution, so the next reader is not sent to a decision already made.

// !! Week 29 resolved this, in the CALLERS rather than here. For a LEFT OUTER
// join an ON predicate filters the match test — a left row whose only candidate
// fails it is null-extended, not deleted — while a WHERE predicate filters the
// result. So `residuals` are folded into the WHERE conjunction only for an
// INNER join; for an outer join they are conjoined onto LogicalJoin::on_residual
// and evaluated inside the probe loop. This function's decomposition is
// identical either way, which is why it takes no join type.
```

```cpp
// src/planner/logical_plan.cc — inside LogicalPlanBuilder::build's join loop

JoinCondition on = classifyJoinCondition(jc.condition.get(), join_slot);
std::vector<JoinKey> keys = std::move(on.keys);

// Week 29: where the residuals go is decided by the join type, and only here.
// INNER: R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S), so they join the WHERE conjunction below
// and inherit pushdown for free (Week 27).
// LEFT:  that identity is false — moving q out of the ON deletes the left rows
// it rejects instead of null-extending them, which is exactly TPC-H Q13's
// `o_comment not like ...`. They stay attached to this join.
std::unique_ptr<Expr> on_pred;
if (jc.type == JoinType::LEFT) {
    std::vector<std::unique_ptr<Expr>> parts;
    for (const Expr* r : on.residuals) parts.push_back(cloneExpr(r));
    if (!parts.empty()) on_pred = conjoinAll(std::move(parts));
} else {
    for (const Expr* r : on.residuals) on_residuals.push_back(cloneExpr(r));
}
```

### Implementation guidance

1. `on.residuals` are **borrowed** pointers into the statement's ON trees, which
   die with `stmt` at the end of `build`. Clone (`cloneExpr`, dispatch site 11)
   exactly as the inner path already does. Forgetting this is a use-after-free
   that will usually *not* crash in a debug run.
2. `conjoinAll` takes ownership and moves; do not reuse the vector afterwards.
3. Keep the inner path byte-identical. If you find yourself restructuring the
   inner branch, stop — every `--explain` string and every harness result for
   inner joins must be unchanged after this week.
4. Do the same in `Planner::plan` **only if** you implement Task 7. If you take
   the refusal option there, `Planner::plan` never sees an outer join and its
   residual fold stays as-is.
5. Gotcha: `buildScanSchema` already collects columns from every ON condition
   (`for (const auto& j : stmt.joins) collectCols(j.condition.get(), required)`),
   so an outer residual's columns are in the scan schemas already. No change —
   but if you ever see `column not found` from a residual, that loop is the
   first place to look.

### Verification

The residual's placement is only observable as a **row count**, so this task
cannot be verified by `--explain` alone. Two queries whose answers differ by
exactly the fold:

```bash
# ON residual: every driver appears; drivers with no fast lap get c = 0
./build/swiftql --catalog catalog.json --no-cache --storage columnar --execution vectorized \
  --query "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id AND l.speed > 340"

# WHERE version: those drivers disappear. The two numbers MUST differ.
./build/swiftql ... \
  --query "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id WHERE l.speed > 340"
```

Both go into `compare_against_sqlite.py` (Task 10) — SQLite is the only thing
that can say which number is right.

---

## Task 3 — `LogicalJoin` learns its type and carries its `ON` predicate

### Why it matters

`LogicalJoin` is the single structure four passes read: pushdown routes by
`join_slot`, enumeration decomposes by `keys`, the estimator stamps
`estimated_rows`, and lowering resolves key indices and picks a build side. Each
of them needs to ask "is this an outer join?" and one of them needs the residual.
Putting both on the node — rather than re-deriving from the AST, which is gone
by then — is what keeps every pass reading one truth.

Downstream: Tasks 4, 5, 6 and 7 all read `join_type`; Task 6 consumes
`on_residual`.

### Conceptual explanation

Two new fields, both defaulted, both set *after* construction — exactly the
discipline `order_decision` follows (`logical_plan.h`: "set by JoinEnumeration
on the TOP join of an enumerated tree, and nowhere else"). Post-construction
assignment keeps the existing five-argument constructor, so every hand-built
test tree in `tests/` keeps compiling and every inner-join `explain()` string
stays byte-identical.

The merged-schema construction, the slot stamping and the `JoinKey::from_slot`
contract are **unchanged**. An outer join's output schema is the same
`[left block] ++ [this relation's columns stamped join_slot]` it always was; the
only difference is that the right block's *values* may be NULL. This is what
"preserve … stable output slots" in the README bullet means, and it is free —
you get it by not touching anything.

Type-check the residual against the merged schema at plan time, the same way
`stmt.where` is checked (`inferExprType(stmt.where.get(), node->output_schema)`),
so an ill-typed `ON` residual is a plan-time SQL error rather than a per-row
throw from inside the probe loop.

### Code

```cpp
// src/planner/logical_plan.h

// Equi-join, INNER by default. Week 29 adds LEFT OUTER: keys and merged schema
// are identical, and children[1] is still exactly one relation — what changes is
// that an unmatched children[0] row is emitted with NULLs across children[1]'s
// block, and that four passes must now ask which kind of join this is.
struct LogicalJoin : LogicalPlanNode {
    std::vector<JoinKey> keys;
    int join_slot;
    std::string order_decision;

    // Week 29. Set after construction (like order_decision) so the five-argument
    // constructor — and every hand-built test tree that calls it — is unchanged.
    JoinType join_type = JoinType::INNER;

    // Non-key ON conjuncts, conjoined. NON-NULL ONLY FOR AN OUTER JOIN: an inner
    // join's residuals are folded into the WHERE conjunction instead (Week 27),
    // because for an inner join ON and WHERE are interchangeable and the fold
    // buys pushdown. For an outer join they are part of the MATCH TEST: a left
    // row whose only candidate fails this predicate is null-extended, not
    // deleted. Resolves against THIS node's merged output_schema.
    std::unique_ptr<Expr> on_residual;
    ...
};
```

```cpp
// src/planner/logical_plan.cc — LogicalJoin::explain()

// Week 29: the node NAME carries the join type. A suffix or a bracketed flag
// would have changed inner-join plan strings; a distinct name leaves all of them
// byte-identical and is unmissable in a plan dump. `residual=` prints only when
// present, same rule as order_decision.
std::string s = (join_type == JoinType::LEFT ? "LogicalLeftJoin [" : "LogicalJoin [");
for (size_t i = 0; i < keys.size(); ++i) { ... unchanged ... }
s += "]";
if (on_residual) s += " residual=" + exprToString(on_residual.get());
if (!order_decision.empty()) s += " " + order_decision;
return s;
```

```cpp
// src/planner/logical_plan.cc — LogicalPlanBuilder::build, after the join is made

node = std::make_unique<LogicalJoin>(std::move(node), std::move(join_scan),
                                     std::move(keys), join_slot, Schema(merged_cols));
if (jc.type == JoinType::LEFT) {
    auto* lj = static_cast<LogicalJoin*>(node.get());
    lj->join_type = JoinType::LEFT;
    if (on_pred) {
        // plan-time type check against the MERGED schema, exactly as the WHERE
        // conjunction is checked below — a STRING/numeric residual must fail here,
        // not per row inside the probe loop
        inferExprType(on_pred.get(), lj->output_schema);
        lj->on_residual = std::move(on_pred);
    }
}
```

### Implementation guidance

1. Order matters: build the merged schema, construct the join, *then*
   `inferExprType` against `lj->output_schema`. Checking against the scan schema
   would reject any residual spanning both sides.
2. Do not add a constructor overload. Two more parameters on a five-parameter
   constructor is where the next reader gets the argument order wrong; the
   post-construction assignment is the file's existing idiom.
3. `on_residual` is a `unique_ptr<Expr>` owned by the node and **moved into the
   physical operator** at lowering (Task 6), the same way
   `LogicalFilter::predicate` is moved into `VecFilterNode`. After lowering the
   logical node is dead; do not read from it.
4. Gotcha — the python harness greps plan lines for the substring `Join`
   (`joinRowsMaterialized` in `test_new_queries.py`: `if "Join" not in line:
   continue`). `LogicalLeftJoin` and `VecLeftHashJoin` both contain it. Any other
   naming (`LeftOuter[…]`) silently drops outer joins out of that measurement.

### Verification

```bash
./build/swiftql --catalog catalog.json --no-cache --storage columnar --execution vectorized --explain \
  --query "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id AND l.speed > 340"
# Logical Plan must show:  LogicalLeftJoin [driver_id = driver_id] residual=speed > 340

# and an inner join's plan string must be BYTE-IDENTICAL to what it was before this week:
git stash && ./build/swiftql ... --explain --query "<any existing join query>" > /tmp/before.txt
git stash pop && cmake --build build -j$(nproc) && ./build/swiftql ... > /tmp/after.txt && diff /tmp/before.txt /tmp/after.txt
```

The `diff` being empty is the real assertion of this task. Add a
`tests/test_logical_plan.cc` case asserting the `LogicalLeftJoin` prefix and one
asserting an ill-typed residual (`ON d.driver_id = l.driver_id AND d.name > 3`)
throws at plan time.

---

## Task 4 — Predicate pushdown must not cross to the null-supplying side

### Why it matters

This is I3, and it is a **wrong answer** — more rows, no error, on a query that
returns the right answer under `--no-optimize`. It is also the failure mode with
the worst debugging story in the project: the optimized and unoptimized plans
both look sensible, both run, and only the row count differs. `test_new_queries.py`'s
invariant suite (optimized ≡ `--no-optimize`, invariant 11) is what catches it,
which is why Task 10 adds outer-join shapes there specifically.

Downstream: nothing reads this; it is a constraint on an existing pass.

### Conceptual explanation

`PredicatePushdown::pushIntoJoin` splits the `WHERE` into conjuncts, buckets
each by `soleSlot()`, and `distribute()` walks the left-deep spine handing each
bucket to the subtree that owns its relation. In a left-deep tree, at each
`JOIN` node `children[1]` **is** relation `join_slot`, and every lower slot is
in `children[0]`.

So the rule is exactly one sentence: **at a `LEFT` join, do not hand the bucket
to `children[1]`.** Leave it in `by_slot`. `pushIntoJoin`'s existing leftover
loop then lifts it into `residual`, and `filterOnto` attaches it above the whole
join tree — which is where `WHERE` semantics put it anyway. The pass already has
this degradation path, with the comment "Anything unclaimed stays above the join
— correct, just slower — so a future tree shape degrades instead of dropping a
predicate." This week is that future tree shape.

Recursion into `children[0]` stays unconditional and is **still correct at every
depth**, because the test is re-applied at each join on the way down. A conjunct
owned by relation `R` nested three joins deep passes each join it descends
through; if any of those is a `LEFT` join with `R` on its `children[1]`, the
descent stops there — which is precisely the condition "R is null-supplied by
some join between it and the WHERE".

> ⚠️ **RETRACTED IN WEEK 37 — the recursion is no longer unconditional, and
> "correct" was the wrong axis.** *(Annotation added by the Week 37 doc sweep;
> the paragraph above is left in its Week-29 wording under the dated-record
> policy in README's "Documentation conventions". Two sweeps have now argued
> about whether to delete it. The answer is: leave it, mark it — and this note
> is the mark, so it need not be argued again.)*
>
> Week 29 checked **result preservation** and got it right: the row sets are
> equal at every depth, exactly as argued. What it did not check is **error
> behaviour**. Per-row evaluation can throw, so which rows an expression is
> evaluated on decides whether the query errors — and pushing a conjunct into a
> `LEFT` join's **preserved** side removes rows, and with them evaluations of
> that join's `ON` residual, which is evaluated once per **candidate pair**
> inside the probe loop. The residual is not a conjunct of any list the pass
> touches, so no argument about the conjunct's own descent could have found it.
> Seam audit pass 5 (P5-B1) reproduced it in both directions on the shipped
> catalogs: `optimized` answered where `--no-optimize` and both Volcano legs
> threw. Since Week 37 `distribute` declines the preserved side too, when the
> join's `on_residual` can raise (`predicate_pushdown.cc`; the screen is
> `conjunctMayRaise` in `parser/expr_totality.h`).
>
> The general form, which is the part worth carrying forward: **the obligation is
> per EXPRESSION whose row set a rewrite changes, not per MOVE the rewrite
> makes** — a strictly larger set. Five consecutive audit passes found the defect
> in exactly the entry a shorter enumeration was missing.

Two consequences to hold in your head:

- A conjunct that stops here could legally be attached just above *its* outer
  join rather than above the whole tree. Correct but not required; do not build
  it (see "What not to build").
- A null-rejecting conjunct on the null-supplying side makes the outer join
  equivalent to an inner join, at which point pushing would be legal again. That
  rewrite is out of scope; the guard must not try to be clever about it.

### Code

```cpp
// src/planner/predicate_pushdown.cc — distribute()

std::unique_ptr<LogicalPlanNode> distribute(std::unique_ptr<LogicalPlanNode> node,
                                            std::map<int, std::vector<std::unique_ptr<Expr>>>& by_slot,
                                            const Catalog& catalog) {
    if (node->type == LogicalNodeType::JOIN) {
        auto* join = static_cast<LogicalJoin*>(node.get());

        // Week 29. children[1] of a LEFT join is the NULL-SUPPLYING relation.
        // sigma_p(R LEFTJOIN S) is NOT sigma_p(R) LEFTJOIN sigma_p(S): filtering S
        // first makes left rows that HAD matches lose them, and the outer join
        // then null-extends exactly the rows the WHERE existed to remove. MORE
        // rows, no error, and both plans look reasonable in --explain.
        // The preserved side is unaffected — sigma_p(R) LEFTJOIN S IS equivalent —
        // so the recursion into children[0] below stays unconditional, and the
        // test is re-applied at every join on the way down.
        // Leaving the bucket in by_slot is not a leak: pushIntoJoin's leftover
        // loop lifts it above the whole tree, which is where WHERE semantics put
        // it — the "degrade instead of drop" path that pass already documents.
        if (join->join_type == JoinType::INNER) {
            auto it = by_slot.find(join->join_slot);
            if (it != by_slot.end()) {
                for (auto& c : it->second) restampSlots(c.get(), 0);
                join->children[1] = filterOnto(std::move(join->children[1]), std::move(it->second), catalog);
                by_slot.erase(it);
            }
        }

        join->children[0] = distribute(std::move(join->children[0]), by_slot, catalog);
        return node;
    }
    ... // bottom of the spine — unchanged
}
```

Also update the class comment in `predicate_pushdown.h`, which currently reads
as a warning about the future:

```cpp
// Inner-join only for the null-supplying side: sigma_p(R ⋈ S) ≡ sigma_p(R) ⋈ S
// holds for both sides of an inner join, but for a LEFT join only for the
// PRESERVED side. distribute() therefore declines to push into children[1] of a
// LEFT join (Week 29); such conjuncts stay above the whole join tree.
```

### Implementation guidance

1. Write the guard as `if (join->join_type == JoinType::INNER)` rather than
   `if (join->join_type != JoinType::LEFT)`. When `RIGHT`/`FULL` never arrive
   this reads the same; if they ever do, the positive form is the safe default.
2. Do **not** also guard the `children[0]` recursion. It is correct, and
   skipping it would silently lose pushdown for every relation under an outer
   join — a performance regression with no test to catch it.
3. `restampSlots(c.get(), 0)` stays inside the inner branch. A conjunct that is
   *not* pushed must keep its real binder slot: it will be evaluated above the
   join, against the merged schema, where slot 0 would resolve to the wrong
   relation.
4. **The pruning-hint interaction, which is the non-obvious part.** The residual
   filter above the join is handed down as the zone-map pruning hint to the FROM
   side (`vectorized_plan_builder.cc`, FILTER case). It now contains
   `ColumnRef op Literal` conjuncts belonging to a null-supplying relation —
   exactly the shape `collectSimplePredicates` accepts. Three things keep this
   safe, and you should verify all three rather than assume them:
   - `ChunkPruner` only accepts refs with `relation_slot < 1`, and a
     null-supplying relation in a left-deep chain always has slot ≥ 1;
   - slot 0 is the FROM relation, which is always on the preserved side of a
     left-deep `LEFT` chain;
   - Week 28's `leftmost_is_slot0` guard withholds the hint entirely if the
     leftmost relation is not slot 0, and Task 5 keeps enumeration from ever
     making that true for an outer-join tree.
   Add the pruning-count check in Verification below; do not take this on faith.

### Verification

```bash
Q="SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id WHERE l.season = 2024"

# The whole task, in one comparison. These MUST be equal.
./build/swiftql --catalog catalog.json --no-cache --storage columnar --execution vectorized --query "$Q"
./build/swiftql --catalog catalog.json --no-cache --storage columnar --execution vectorized --no-optimize --query "$Q"

# And the plan must show the filter ABOVE the join, not on the laps scan:
./build/swiftql ... --explain --query "$Q"      # LogicalFilter [season = 2024] over LogicalLeftJoin

# Control: the same predicate on the PRESERVED side must still push down.
./build/swiftql ... --explain \
  --query "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id WHERE d.age > 30"
# LogicalFilter [age > 30] must sit under the join, on the drivers scan
```

Add to `tests/test_predicate_pushdown.cc`: one test asserting the null-supplying
conjunct is *not* below the join for a `LEFT` join, and one asserting the
preserved-side conjunct still is. Both are plan-shape assertions on the logical
tree — the pass's existing test style.

---

## Task 5 — Join enumeration declines outer-join trees; the outer cardinality rule

### Why it matters

I1 and I2. This is the bullet the Week 28 Starting note leads with: "a left
outer join makes this sharper, not softer: it is not commutative, so the
enumerator must be *told* which pairs it may reorder before it sees one — an
unguarded outer join in the graph is a wrong answer, not a bad plan."

Downstream: with enumeration declining, the merged-schema leftmost relation is
always slot 0 for any tree containing an outer join, which is what keeps Task
4's pruning-hint argument and `ChunkPruner`'s `slot < 1` test valid. The two
tasks are load-bearing for each other.

### Conceptual explanation

**The decline.** `JoinEnumeration::reorder` already has two declines
(`n < MIN_ENUMERATED_RELATIONS`, `n > 32`), both of which `return node;`
unchanged and print no `order=` line — honest, because there was no decision.
Add a third. Detect *before* `decompose()`: decompose moves subtrees out of the
tree, so a decline discovered halfway through has nothing clean to return.

Why decline the **whole tree** rather than the outer join's block only: the DP
enumerates subsets of the whole relation set. Restricting it to a connected
inner-only sub-block means computing which sub-blocks are reorder-safe, which is
the conflict/eligibility machinery listed under "What not to build". A decline
costs one function and is always correct.

**The cardinality rule.** An outer join's output is at least the preserved
side's row count:

```
|R ⟕_p S| = |R ⋈_p S| + |{ r ∈ R : no match }|
```

We have no statistic for the second term, so the standard containment is
`max(inner_estimate, |R|)`. Where it goes matters more than what it is:

> Week 28 moved the ≥1-row floor **off** the search's transition function
> because a per-step clamp makes a subset's row count depend on the path that
> reached it, destroying the DP's optimal substructure. `max(rows, left_rows)`
> has exactly the same defect — it is not multiplicative. It therefore goes at
> the **stamping** site (`CardinalityEstimator::estimateNode`'s JOIN case),
> never inside `joinCardinality`.

Since enumeration declines outer trees, the DP never meets the rule at all. Both
statements are true and both must stay true; that is the discipline Week 28's
first Starting note asks any week adding a cardinality rule to inherit.

The `ON` residual's selectivity is deliberately ignored: it can only reduce the
number of matches, and the output is `≥ |R|` regardless, so modelling it would
change the estimate only in the direction the `max` already floors. Document it
as an approximation rather than guessing.

### Code

```cpp
// src/planner/join_enumeration.cc — file-local, next to countRelations()

// Week 29. An outer join is not commutative (R ⟕ S ≠ S ⟕ R) and not freely
// associative, so an ordering that is sound for inner joins is a WRONG ANSWER
// here, not a merely expensive one. The DP's whole premise — any relation may be
// added to any subset in any order — is a legality claim, and repairing it needs
// per-edge conflict/eligibility sets (Moerkotte TES/SES), which is a different
// algorithm and buys nothing until a supported query has an outer join inside a
// reorderable block. Decline, in the same shape as the <3-relation and
// >32-relation declines: return the tree untouched and print no order= line,
// because there was no decision to report.
bool containsOuterJoin(const LogicalPlanNode* node) {
    if (node->type == LogicalNodeType::JOIN &&
        static_cast<const LogicalJoin*>(node)->join_type != JoinType::INNER) {
        return true;
    }
    for (const auto& child : node->children) {
        if (containsOuterJoin(child.get())) return true;
    }
    return false;
}
```

```cpp
// src/planner/join_enumeration.cc — first lines of reorder()

const int n = countRelations(node.get());
if (n < MIN_ENUMERATED_RELATIONS) return node;
if (n > 32) return node;                       // uint32_t subset masks
if (containsOuterJoin(node.get())) return node; // Week 29 — see above.
                                                // BEFORE decompose(): that call
                                                // moves subtrees out, so a
                                                // decline found afterwards has
                                                // no clean tree to return.
```

```cpp
// src/planner/cardinality_estimator.cc — JOIN case, at the stamping site

const double l_rows = node.children[0]->estimated_rows;
const double r_rows = node.children[1]->estimated_rows;
double rows = flooredJoinCardinality(
    l_rows, r_rows, joinCardinality(l_rows, r_rows, join.keys, left, right));

// Week 29: a LEFT join emits every preserved-side row at least once, so its
// output is |R ⋈ S| + |unmatched R|. We have no statistic for the second term;
// max() is the containment. Applied HERE, at the stamp, and deliberately NOT
// inside joinCardinality: max() is not multiplicative, so a subset's row count
// would depend on the path that reached it and the DP's optimal substructure
// would be gone — the same reason the >=1-row floor moved out in Week 28.
// (JoinEnumeration declines outer-join trees entirely, so the search never meets
// this rule; both facts must stay true.) The ON residual's selectivity is
// ignored: it only removes MATCHES, and the output is floored at l_rows anyway.
if (join.join_type == JoinType::LEFT) rows = std::max(rows, l_rows);
node.estimated_rows = rows;
```

```cpp
// src/planner/join_enumeration.h — approximation list

//   3. The DP is exact only where every join key has statistics. ... (unchanged)
//      Note also that `cost=` and `est=` on the same --explain line are NOT
//      reconcilable and are not meant to be: the search chains the RAW product
//      from joinCardinality, while every est= on the tree is stamped through
//      flooredJoinCardinality. A sub-1-row intermediate is searched at 0.9 and
//      printed as est=1, so cost= cannot be re-derived from any est=. Any test
//      checking one against the other is testing the wrong thing.
//   5. Outer joins are not reordered AT ALL. R ⟕ S ≠ S ⟕ R and associativity
//      fails, so any tree containing one is declined whole (containsOuterJoin,
//      Week 29). --explain shows no order= line for such a tree, because there
//      was no decision.
```

### Implementation guidance

1. Place the decline **before** `decompose()`. This is the single most common
   way to get this task wrong, and the symptom is a half-moved tree.
2. Do not touch the unbound-key throw. Week 28's third Starting note hands it to
   Week 30 for a specific reason (`e.slot_a >= n` stops meaning "unbound key"
   once subqueries introduce non-range-table scans). Your decline sits next to
   it as the precedent Week 30 will follow — that is the whole contribution.
3. Do not add the outer rule to `joinCardinality`. If you find yourself wanting
   `keys` plus a join type in that signature, re-read approximation 3.
4. Gotcha: `CardinalityEstimator::estimateSubtree` shares `estimateNode`, so
   your `max` is applied when enumeration estimates leaves too. That is correct
   and harmless (a leaf is never a JOIN), but it is why the rule must live in
   the shared function rather than in a copy.
5. The `--no-optimize` path never runs either pass, so an outer join's
   `estimated_rows` stays -1 there and lowering takes the raw-table-size
   fallback. Unchanged behaviour; just know it when reading `--explain-analyze`.

### Verification

```bash
Q3="SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id \
    LEFT JOIN drivers d2 ON d.team = d2.team"

# No order= line at all — the decline, visible on the checkpoint surface
./build/swiftql --catalog catalog.json --no-cache --storage columnar --execution vectorized \
  --explain --query "$Q3" | grep -c "order="        # expect 0

# Same rows optimized and not (invariant 11), which is what the decline buys
./build/swiftql ... --query "$Q3"
./build/swiftql ... --no-optimize --query "$Q3"

# CONTROL: the all-inner version of the same shape must STILL be reordered
./build/swiftql ... --explain --query \
  "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id \
   JOIN drivers d2 ON d.team = d2.team" | grep "order="   # expect method=dp
```

The control is not optional. A `containsOuterJoin` that returns true too eagerly
turns off join ordering for the entire project and every Week 28 steering
assertion goes with it — `python3 python_tools/test_new_queries.py` is the gate
that catches that.

Add to `tests/test_join_enumeration.cc`: `DeclinesAnyTreeContainingAnOuterJoin`
(assert `order_decision` is empty on the top join, and that the tree shape is the
written one) plus the existing inner shapes as controls.

---

## Task 6 — The vectorized left outer hash join, and its lowering

### Why it matters

The operator is the checkpoint. Everything above it is about making sure the
tree handed to it means what the SQL said; this is where unmatched rows actually
get emitted.

Downstream: the null-extended rows flow into `VecHashAggregateNode` (where
`COUNT(col)` must return 0 — Q13), `VecSortNode`, `VecDistinctNode` and
`VecProjectNode`. Per the validity-mask audit above, none of them needs a change;
if one of them does, you have found a Week 24 gap and should say so rather than
patching it locally.

### Conceptual explanation

Three changes to `VecHashJoinNode`, one to `VectorizedPlanBuilder`.

**1. The preserved side must probe.** With `swapped_ == false`, the FROM side is
`probe_child_` and the output row is `[probe columns] ++ [build columns]` — so a
null-extended row is "the probe row, then `Value::null()` until the schema is
full". With `swapped_ == true` you would need matched-flags on the hash table and
a drain pass. Force the non-swapped construction for outer joins (I6) and make
the assumption loud in the constructor.

**2. `serializeKey == false` must emit, not skip.** I5. This is the line that is
currently a correct comment attached to a wrong action.

**3. A probe row that finds no surviving match must emit.** "Surviving" includes
the `ON` residual: a candidate pair that fails the residual is *not a match*, so
if it was the only candidate the left row is null-extended. Track it with one
`bool matched` per probe row — do not try to infer it from `output_buffer_`
sizes, which also grow from earlier rows.

The residual is evaluated with the scalar `evaluate(expr, Row, Schema)` against
the assembled output row and the merged `output_schema_` — the same
correct-but-slow fallback pattern `CASE` uses, and the same evaluation
`VecFilterNode` would have done above the join. Only outer joins with residuals
pay for it. The three-valued rule applies: UNKNOWN is not a match.

### Code

```cpp
// src/execution/vec_hash_join_node.h

    // Week 29. left_outer: emit each probe row at least once, null-extended
    // across the build block when nothing matched. Only valid with
    // swapped == false — the PRESERVED side must be the probe input, because a
    // build-side-preserved outer join needs matched flags and an end-of-probe
    // drain, and VectorizedPlanBuilder therefore forces the side rather than
    // costing it (the one place Week 22's build-side decision does not apply).
    // The constructor throws on the illegal combination.
    //
    // on_residual: the non-key ON conjuncts, conjoined (LogicalJoin::on_residual,
    // moved in). Evaluated against the ASSEMBLED output row and output_schema_.
    // For an outer join it filters the MATCH TEST, so a row whose every candidate
    // fails it is null-extended rather than dropped. nullptr on every inner join
    // — an inner join's residuals are in the WHERE conjunction (Week 27).
    VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child,
                    std::unique_ptr<VecPlanNode> build_child,
                    std::vector<int> probe_key_indices, std::vector<int> build_key_indices,
                    Schema output_schema, bool swapped = false,
                    bool left_outer = false,
                    std::unique_ptr<Expr> on_residual = nullptr);
private:
    bool left_outer_ = false;
    std::unique_ptr<Expr> on_residual_;
    int build_width_ = 0;                 // resolved in open(): the NULL block's width
    void emitNullExtended(const DataChunk& probe_chunk, int r);
```

```cpp
// src/execution/vec_hash_join_node.cc

VecHashJoinNode::VecHashJoinNode(..., bool swapped, bool left_outer,
                                 std::unique_ptr<Expr> on_residual)
    : ..., swapped_(swapped), left_outer_(left_outer),
      on_residual_(std::move(on_residual)) {
    // Loud rather than latent: with swapped_ the build block is the LEFT half of
    // the output row, so emitNullExtended's trailing-NULL assembly would null the
    // preserved side's own columns. Unreachable from the builder (Week 29 forces
    // the side), so this is the shape of a planner bug.
    if (left_outer_ && swapped_) {
        throw std::runtime_error(
            "internal: a left outer join requires the preserved side on the probe input");
    }
}

void VecHashJoinNode::open() {
    ...                                   // unchanged build phase
    // width of the NULL block, taken from the schema the operator actually sees
    build_width_ = build_child_->outputSchema().size();
}

// One preserved-side row with no surviving match. The build block is the TRAILING
// columns of output_schema_ (guaranteed by !swapped_), so the assembly is the
// probe row followed by build_width_ NULLs.
//
// appendColumnValue (vec_types.h) back-fills the validity prefix on the first
// NULL, so fillOutChunk turns these into REAL nulls rather than the placeholder
// underneath them. That is the Week 24 validity mask doing the whole job — this
// operator needs no materialization change at all.
void VecHashJoinNode::emitNullExtended(const DataChunk& probe_chunk, int r) {
    Row out_row;
    out_row.reserve(output_schema_.size());
    for (const auto& cv : probe_chunk.columns) out_row.push_back(valueAt(cv, r));
    for (int i = 0; i < build_width_; ++i) out_row.push_back(Value::null());
    output_buffer_.push_back(std::move(out_row));
}
```

```cpp
// src/execution/vec_hash_join_node.cc — the probe loop in nextChunk()

// SQL boolean-as-INT with an explicit null test: UNKNOWN is not a match, the
// same rule the filter path uses (columnar_eval.cc).
auto passes = [&](const Row& row) {
    if (!on_residual_) return true;
    Value v = evaluate(on_residual_.get(), row, output_schema_);
    return !v.isNull() && v.asInt() != 0;
};

for (int r : *indices_ptr) {
    stats.rows_in++;

    // Week 29: an unmatchable key (NULL member, or NaN) matches nothing — which
    // for a LEFT join is exactly the row that must still be emitted. The Week 27
    // `continue` here was a correct comment attached to the wrong action, and it
    // is the one line in this operator that drops rows silently.
    if (!serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_)) {
        if (left_outer_) emitNullExtended(*probe_chunk, r);
        continue;
    }

    bool matched = false;
    auto it = hash_table_.find(key_buf_);
    if (it != hash_table_.end()) {
        for (const Row& build_row : it->second) {
            Row out_row;
            out_row.reserve(output_schema_.size());
            ... // existing append_probe / append_build assembly, unchanged
            // A candidate failing the ON residual is NOT a match, so it must not
            // set `matched` — otherwise the left row is neither emitted joined
            // nor null-extended, and it vanishes.
            if (!passes(out_row)) continue;
            matched = true;
            output_buffer_.push_back(std::move(out_row));
        }
    }
    if (left_outer_ && !matched) emitNullExtended(*probe_chunk, r);
}
```

```cpp
// src/execution/vec_hash_join_node.cc — explain()
std::string s = left_outer_ ? "VecLeftHashJoin [" : "VecHashJoin [";
... // key rendering unchanged
s += "] (materialize)";
if (on_residual_) s += " residual=" + exprToString(on_residual_.get());
if (!cost_decision_.empty()) s += " " + cost_decision_;
```

```cpp
// src/planner/vectorized_plan_builder.cc — the JOIN case

const bool outer = join->join_type == JoinType::LEFT;

// Week 29: the SIMD loop join is an INNER equi-join — its probe loop emits
// matches and has no unmatched path — so it is simply not costed here, exactly
// as a composite key is not costed (Week 27). An ineligible algorithm is not a
// fallback; the hash join is always correct.
bool use_simd = estimate_driven && int_keys && !outer && best_simd < best_hash;

// Week 29: the build side stops being a free cost choice. A left outer join with
// the preserved side BUILDING needs matched flags plus an end-of-probe drain;
// with it PROBING it needs one branch. Force the side and say so in the decision
// string rather than printing an (alt=) that was never an option. The cost: when
// the null-supplying side is the larger input we hash the larger input. Stated in
// README Limitations; revisit under Week 37 measurement, not on a hunch.
bool from_builds = outer ? false
                 : (use_simd ? cost_simd_from < cost_simd_join
                             : cost_hash_from < cost_hash_join);

if (estimate_driven && outer) {
    std::ostringstream d;
    d << std::fixed << std::setprecision(0)
      << "build=" << (isSingleRelation(join->children[1].get())
                          ? leafScanTable(join->children[1].get()) : "join-subtree")
      << " cost=" << cost_hash_join
      << " (outer: the preserved side must probe)";
    decision = d.str();
}

// ... construction: outer always takes the non-swapped branch
return std::make_unique<VecHashJoinNode>(
    std::move(from_child), std::move(join_child), left_idx, right_idx,
    join->output_schema, /*swapped=*/false,
    /*left_outer=*/true, std::move(join->on_residual));
```

### Implementation guidance

1. **Order of work:** constructor + `emitNullExtended` + the two emit points
   first, with the residual left as `nullptr`; get unmatched preservation
   correct and diffed against SQLite before adding the residual branch. The two
   bugs have different symptoms and debugging them together is avoidable pain.
2. `build_width_` must come from `build_child_->outputSchema().size()`, not from
   `output_schema_.size() - probe_chunk->columns.size()`. The probe chunk's
   column count is the child's schema width and they agree today, but the
   builder already asserts that agreement explicitly for exactly this reason —
   read it from the schema the operator was given.
3. `evaluate` is declared in `execution/evaluator.h`; include it. The residual is
   evaluated **per candidate pair**, not per probe row. If a profile ever shows
   it dominating, the fix is a compiled kernel over the assembled chunk, not a
   semantic shortcut.
4. Do not touch `key_encoding.h`. The NULL policy is the caller's by design, and
   this week is the third caller with the third answer.
5. Do not touch `fillOutChunk`. It already writes through `appendColumnValue`.
6. `stats.rows_out` counts null-extended rows, which is what makes
   `EXPLAIN ANALYZE`'s `rows_out` compare sensibly against `est=` from Task 5.
7. Gotcha: an outer join with **zero** matching probe chunks now produces output
   where it previously produced none. The `while (output_cursor_ >= size)` loop
   in `nextChunk` already handles this; do not "optimize" it into an early
   `return nullptr`.
8. Gotcha: `VecSimdLoopJoinNode` must never be selected. `!outer` in the
   `use_simd` conjunction is the whole guard — but check the `int_keys` line too,
   because if a future change makes SIMD selection unconditional the operator
   will silently return inner-join results.

### Verification

```bash
CAT="--catalog catalog.json --no-cache --storage columnar --execution vectorized"

# 1. Unmatched preservation. LEFT count MUST exceed the inner count.
./build/swiftql $CAT --query "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id AND l.speed > 340"
./build/swiftql $CAT --query "SELECT COUNT(*) AS c FROM drivers d      JOIN laps l ON d.driver_id = l.driver_id AND l.speed > 340"

# 2. THE Q13 shape: COUNT(col) is 0 for an unmatched row, COUNT(*) is 1.
./build/swiftql $CAT --query \
 "SELECT d.name AS n, COUNT(l.lap_id) AS c, COUNT(*) AS star FROM drivers d \
  LEFT JOIN laps l ON d.driver_id = l.driver_id AND l.speed > 400 GROUP BY d.name ORDER BY n"
# every row: c = 0, star = 1. If c = 1, COUNT is counting NULLs — check non_null_count.

# 3. The NULL is a real NULL, not a placeholder 0 / empty string
./build/swiftql $CAT --query \
 "SELECT d.name AS n, l.speed AS s FROM drivers d LEFT JOIN laps l \
  ON d.driver_id = l.driver_id AND l.speed > 400 ORDER BY n LIMIT 5"
# the s column must print NULL

# 4. IS NULL must find them — proves the validity mask survived materialization
./build/swiftql $CAT --query \
 "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id \
  AND l.speed > 400 WHERE l.lap_id IS NULL"

# 5. SIMD is never chosen for an outer join (INT keys make it eligible otherwise)
./build/swiftql $CAT --explain --query \
 "SELECT COUNT(*) AS c FROM laps l LEFT JOIN drivers d ON l.driver_id = d.driver_id" | grep "algo="   # expect nothing
```

Unit tests (`tests/test_vectorized.cc`, following the existing vec-node style —
hand-built chunks, no catalog):

- a probe row with no match emits exactly one row, with `isNull()` true on every
  build-side column;
- a probe row with two matches emits two rows and **no** null-extended row;
- a probe row whose key is NULL emits one null-extended row (the I5 regression —
  write this one first, it is the line most likely to be missed);
- an `ON` residual that rejects the only candidate produces a null-extended row,
  not zero rows;
- constructing with `left_outer = true, swapped = true` throws.

---

## Task 7 — The Volcano baseline: the same rule, or a documented refusal

### Why it matters — and the decision you have to make

The README bullet says "logical and **vectorized** left outer hash join", and
Week 27 established the precedent that a capability may be vectorized-only
(multi-way joins). So refusing on Volcano is a legitimate reading of the
checkpoint.

Against that: invariant 6 says Volcano is the correctness baseline and must
support the full SQL surface, and **this is the worst possible week to halve the
oracle**. Every defect this week can introduce is a NULL-semantics defect that
returns plausible rows; `compare_against_sqlite.py` runs four modes, and the two
Volcano modes are half of the evidence that the vectorized operator is right.
Invariant 14 also means these are the *first* NULLs the Volcano path has ever
seen from real catalog data.

**Recommendation: implement it.** The cost is ~40 lines in one operator plus the
residual branch in `Planner::plan`, and the scope is naturally bounded —
`Planner::plan` builds exactly one join and refuses more, so Volcano gets
two-relation outer joins and nothing else, which is exactly Q13's shape.

**If you refuse instead**, do it properly: the message must name the capable
path (Week 27's wording), and `compare_against_sqlite.py` needs an
`OUTER_VOLCANO_REJECTED` block asserting the refusal in the two Volcano modes —
a per-mode difference with no assertion on the refusing side is how a refusal
quietly becomes a wrong answer later. Then say so in README Feature Scope and
Limitations. Do not leave it implicit.

The rest of this task assumes the recommendation.

### Conceptual explanation

Volcano's `HashJoinNode::next()` is a two-state iterator: either it is draining
the current probe row's bucket, or it is pulling the next probe row. Outer
semantics add one fact that must survive across `next()` calls — *did the
current probe row match anything* — and one new emission point, at the moment
the bucket is exhausted.

The subtlety Volcano has and the vectorized path does not: the null-extended row
must be emitted **after** the bucket drains, so you need to keep the probe row
reachable at that point. Clear `current_probe_row_` *after* assembling the
null-extended row, not before.

`Planner::plan` must force `swap = false` for an outer join, for the same reason
as Task 6, and must skip the residual fold from Task 2.

### Code

```cpp
// src/planner/plan_nodes.h — HashJoinNode
    // Week 29: same contract as VecHashJoinNode. left_outer requires
    // swapped == false (the preserved side must be the probe input); Planner::plan
    // forces it rather than costing it.
    HashJoinNode(..., bool swapped = false,
                 bool left_outer = false,
                 std::unique_ptr<Expr> on_residual = nullptr);
private:
    bool left_outer_ = false;
    std::unique_ptr<Expr> on_residual_;
    bool probe_matched_ = false;   // reset per probe row; survives next() calls
    int build_width_ = 0;          // resolved in open()
```

```cpp
// src/planner/plan_nodes.cc — HashJoinNode::next(), shape only

Row* HashJoinNode::next() {
    while (true) {
        if (current_probe_row_ != nullptr) {
            auto it = hash_table_.find(probe_key_);
            if (it != hash_table_.end() && bucket_idx_ < (int)it->second.size()) {
                const Row& build_row = it->second[bucket_idx_++];
                current_row_.clear();
                ... // existing assembly, unchanged
                // a candidate failing the ON residual is not a match: do not set
                // probe_matched_, and do not emit it
                if (on_residual_ && !passes(current_row_)) continue;
                probe_matched_ = true;
                stats.rows_out++;
                return &current_row_;
            }
            // Bucket exhausted. Assemble the null-extended row BEFORE clearing
            // current_probe_row_ — it is the source of the preserved half.
            if (left_outer_ && !probe_matched_) {
                current_row_.clear();
                for (const Value& v : *current_probe_row_) current_row_.push_back(v);
                for (int i = 0; i < build_width_; ++i) current_row_.push_back(Value::null());
                current_probe_row_ = nullptr;
                stats.rows_out++;
                return &current_row_;
            }
            current_probe_row_ = nullptr;
        } else {
            Row* probe_row = left_->next();
            if (!probe_row) return nullptr;
            stats.rows_in++;
            probe_matched_ = false;
            if (serializeRowKey(*probe_row, left_key_idx_, probe_key_)) {
                current_probe_row_ = probe_row;
                bucket_idx_ = 0;
            } else if (left_outer_) {
                // unmatchable key: matches nothing, and for a LEFT join that is
                // precisely the row to emit (same rule as the vectorized path)
                current_row_.clear();
                for (const Value& v : *probe_row) current_row_.push_back(v);
                for (int i = 0; i < build_width_; ++i) current_row_.push_back(Value::null());
                stats.rows_out++;
                return &current_row_;
            }
        }
    }
}
```

```cpp
// src/planner/planner.cc
const bool outer = !stmt.joins.empty() && stmt.joins[0].type == JoinType::LEFT;

// Week 29: an outer join's residuals stay in the ON (they filter the match test),
// so only an inner join folds them into the WHERE conjunction — see
// logical_plan.cc for the same split and join_condition.h for why.
std::unique_ptr<Expr> on_pred;
if (!stmt.joins.empty()) {
    JoinCondition on = classifyJoinCondition(stmt.joins[0].condition.get(), 1);
    join_keys = std::move(on.keys);
    if (outer) {
        std::vector<std::unique_ptr<Expr>> parts;
        for (const Expr* r : on.residuals) parts.push_back(cloneExpr(r));
        if (!parts.empty()) on_pred = conjoinAll(std::move(parts));
    } else if (!on.residuals.empty()) {
        ... // existing fold into stmt.where
    }
}
...
// the preserved side must probe: no swap for an outer join
bool swap = !outer && from_row_count < join_row_count;
```

### Implementation guidance

1. `probe_matched_` must be reset when a **new probe row is pulled**, not in
   `open()`. A stale `true` silently deletes unmatched rows.
2. `current_row_` is returned by pointer and reused; assemble the null-extended
   row into it exactly as the matched path does, so the caller's lifetime
   assumptions do not change.
3. `build_width_` from `right_->outputSchema().size()` in `open()`, after the
   existing key resolution.
4. `evaluate(on_residual_.get(), current_row_, output_schema_)` — the merged
   schema, exactly as `FilterNode` above the join would have used.
5. Gotcha: `Planner::plan` builds the *pruning hint* from `stmt.where` before the
   scan is constructed. Task 2's split means an outer join's residual is no
   longer in `stmt.where` on either path, so both paths hand the same hint down.
   That symmetry is the reason Week 27 moved the ON decomposition above the scan
   construction; do not move it back.
6. Volcano still refuses ≥3 relations. An outer join in a 3-relation query is a
   Volcano refusal for the *pre-existing* reason, with the pre-existing message.
   Do not add a second refusal for it.

### Verification

The point of this task is cross-engine agreement, so verify it as a diff:

```bash
Q="SELECT d.name AS n, COUNT(l.lap_id) AS c FROM drivers d LEFT JOIN laps l \
   ON d.driver_id = l.driver_id AND l.speed > 340 GROUP BY d.name ORDER BY n"

for MODE in "--storage row --execution volcano" \
            "--storage columnar --execution volcano" \
            "--storage columnar --execution vectorized" \
            "--storage columnar --execution vectorized --no-optimize"; do
  ./build/swiftql --catalog catalog.json --no-cache $MODE --query "$Q" > /tmp/$RANDOM.out
done
# all four files identical, and equal to SQLite's answer (Task 10)
```

---

## Task 8 — The two dialect facts Week 27 handed to this week

### 8a — A join key comparing a STRING column to a numeric one

#### Why it matters

Week 27's note: the `ON` clause accepts it and silently half-matches, while the
identical predicate in `WHERE` throws. `inferExprType` type-checks only the
arithmetic operators (`=` falls through to `INT`), and `classifyJoinCondition`
accepts any cross-slot `ColumnRef = ColumnRef` as a key. The key is then compared
as **text**, which carries no type tag, so a STRING `"7"` matches an INT `7`
while `"007"` does not — and `Value::operator==` on the same pair throws
`Type mismatch`. Both halves are reachable on the shipped catalog
(`drivers.team` vs `laps.lap_id`).

An outer join makes it worse in a specific way: the rows the type confusion
fails to match no longer go missing — they come back **null-extended**, which is
a plausible-looking result rather than an obviously short one. That is why it is
worth the gate now.

It also makes the `int_keys` SIMD gate's assumption explicit: that gate asks
whether both key columns are INT, and has been relying on nothing else asking
whether they are the same *kind* of thing.

#### Conceptual explanation

The check needs the two key columns' **types**, which means schemas —
`classifyJoinCondition` works at `Expr` level and has none. `Validator::validate`
already builds a `relations` vector keyed by ref name (alias or table) in
range-table order, and it already calls `classifyJoinCondition` for its shape
check. Both engines route through `Validator::validate`
(`Planner::plan` and `LogicalPlanBuilder::build` both call it first), so one
implementation covers all four modes with one message.

The rule is deliberately coarse — **both STRING, or both numeric** — matching
`Value::operator==`'s own rule (it coerces INT/DOUBLE and throws only across the
STRING boundary) and `keyFieldText`'s numeric affinity (`7.0` and `7` join, which
is SQLite's behaviour and must keep working).

#### Code

```cpp
// src/planner/validator.cc — inside the existing per-join loop

JoinCondition on = classifyJoinCondition(stmt.joins[i].condition.get(), (int)i + 1);
validateJoinCondition(stmt.joins[i].condition.get(), relations);

// Week 29 (deferred from the Week 27 audit). A join key is compared as TEXT,
// which carries no type tag: a STRING "7" matches an INT 7 while "007" does not,
// while the identical predicate in a WHERE clause throws Type mismatch. Half a
// match with no error either way — and under an outer join the unmatched half
// returns as null-extended rows rather than as missing ones. `relations` is in
// range-table order, so relations[slot] is the schema JoinKey slots address.
for (const JoinKey& k : on.keys) {
    if (k.from_slot < 0) continue;                 // unbound: positional routing,
                                                   // no relation identity to check
    const Schema* left  = relations[k.from_slot].second;
    const Schema* right = relations[i + 1].second;
    int li = left->indexOf(k.from_col), ri = right->indexOf(k.join_col);
    if (li < 0 || ri < 0) continue;                // existence is validateJoinCondition's
    const bool l_str = left->column(li).type  == TypeId::STRING;
    const bool r_str = right->column(ri).type == TypeId::STRING;
    if (l_str != r_str) {
        throw std::runtime_error(
            "JOIN ON: cannot join a STRING column with a numeric one ('"
            + k.from_col + "' and '" + k.join_col + "')");
    }
}
```

#### Implementation guidance

1. This is an **added rejection**: queries that execute today stop executing.
   That is the "own gate" Week 27 asked for — land it in its own commit, with
   its own harness entries, so a bisect can separate it from the outer join.
2. Keep INT-vs-DOUBLE **legal**. `keyFieldText` deliberately makes `7.0` and `7`
   join (SQLite's numeric affinity), and the Week 27 encoding tests pin it.
3. `relations[k.from_slot]` is safe because `from_slot` is a binder slot and the
   vector is built in range-table order — but assert it if you prefer: an
   out-of-range index here is a planner bug, not a user error.

#### Verification

```bash
# now a clean error, in all four modes
./build/swiftql --catalog catalog.json --no-cache \
  --query "SELECT COUNT(*) FROM drivers d JOIN laps l ON d.team = l.lap_id"

# still fine: INT vs DOUBLE, and every existing join query
./build/swiftql ... --query "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id"
```

Add it to `compare_against_sqlite.py`'s rejection suite (the
`WEEK26_REJECTED_QUERIES` pattern, which runs in every mode), and a
`tests/test_planner.cc` case. Update README's "Syntax Deliberately Not
Supported" table with the row and its reason.

### 8b — NaN at load: deferred, with the reason made concrete

#### Why it matters

Week 27's note proposed converting NaN to NULL in `CSVLoader`, which would align
SwiftQL with SQLite for `GROUP BY`, `DISTINCT` and joins at once.

#### Conceptual explanation — why it cannot be a three-line change

`CSVLoader::parseField` returning `Value::null()` for a NaN DOUBLE breaks the
next stage:

```cpp
// src/storage/csv_to_columnar.cc, pass 2
case TypeId::DOUBLE:
    std::get<std::vector<double>>(table.columns[name]).push_back(row[c].asDouble());
```

`asDouble()` on a null `Value` is a `std::get` on the wrong variant alternative.
`ColumnarTable` has **no NULL representation at all** — that is invariant 14, and
it is why every null-handling path in the project has been testable only through
in-memory operator tests. Closing the NaN note therefore means giving columnar
storage a validity mask and teaching `VecScanNode` to emit it: a storage-layer
project that touches every scan, every encoding and every zone map.

**Decision: defer to Week 35**, which owns the loader, the pipe-delimited TPC-H
format and the scale-factor workflow — i.e. the week that is already rewriting
this code path and would otherwise have to implement the fix twice.

Note also that the outer join does **not** make it more urgent, and slightly
improves it: a NaN key on the preserved side is unmatchable
(`isUnmatchableKey`), so it is now emitted null-extended — which is exactly what
SQLite does with the same row, since it stores the NaN as NULL and a NULL key
matches nothing. The remaining divergence is confined to `GROUP BY` / `DISTINCT`,
unchanged from Week 27.

#### Implementation guidance

Do not write code. Do two things:

1. Sharpen the README Limitations bullet: replace "closing it means converting at
   load, which is a storage-layer decision noted for Week 29" with the finding —
   converting at load requires a NULL representation in `ColumnarTable`
   (invariant 14), so it belongs to the week that rewrites the loader (Week 35).
2. Leave `key_encoding.h`'s NaN branch alone, and note in the Week 29 record that
   it is defensive rather than live: no engine arithmetic produces a NaN
   (`x / 0` is NULL by design), so it is reachable only from a hand-written CSV
   cell.

#### Verification

`grep -n "NaN" README.md` shows exactly one Limitations bullet, and it names
Week 35 and invariant 14 rather than Week 29.

---

## Task 9 — Close Week 28's test residue before adding cases to it

### Why it matters

Week 28's second Starting note: the two assertions that pin the written-order
bound **cannot fail**, because `reorder()` clamps
`chosen_cost = min(chosen_cost, written_cost)` two statements before it builds
the string both assertions parse. Task 5 adds a case whose correct outcome is
that *no* `order=` line is printed at all — and a tautological assertion cannot
distinguish "the decline fired" from "the search silently downgraded". Fix the
assertion first, then add the case.

The same note reports `method=written-floor` has never executed, because both
shipped tables carry full statistics. One stats-less fixture exercises that guard
*and* `joinCardinality`'s non-multiplicative `max(l, r)` branch — the reason the
guard exists. You are already editing this file; it is the cheapest it will ever
be.

### Conceptual explanation

Three edits, all in test code:

1. **`tests/test_join_enumeration.cc`,
   `NeverInstallsAnOrderWorseThanTheWrittenOne`** — replace `EXPECT_LE(chosen,
   written)` with an assertion on `method=dp`. The non-tautological statement is
   "the search did not need the bound", not "the clamped number is ≤ itself".
2. **`python_tools/test_new_queries.py`, `run_join_order_steering`'s `cost_ok`** —
   same substitution: parse `method=(\w+)` and require `dp`.
3. **A stats-less-catalog fixture** — a `Catalog` where one table never gets
   `setStats`. That makes `have_ndv` false for its keys, sends `joinCardinality`
   down `max(l, r)`, and gives `written_cost < chosen_cost` a chance to hold —
   the first time `method=written-floor` can execute.

### Code

```cpp
// tests/test_join_enumeration.cc

// The bound is enforced by construction (reorder() clamps chosen_cost to
// written_cost before printing), so asserting chosen <= written is a tautology:
// reintroduce the Week 28 floor defect exactly and the DP again returns a
// cost-666 order against the written 629, reorder() silently downgrades to
// method=written-floor, prints cost=629 (written=629), and this test passes
// while the optimizer has stopped optimizing that shape. `method=dp` is the
// statement that actually has content: the search did not need the bound.
for (const std::string& sql : cases) {
    auto plan = optimize(sql, cat);
    const std::string decision = decisionOf(plan.get());
    auto [chosen, written] = costs(decision);
    ASSERT_GE(chosen, 0.0) << sql;
    EXPECT_LE(chosen, written) << sql;                          // keep: cheap, still true
    EXPECT_NE(decision.find("method=dp"), std::string::npos)    // the real assertion
        << sql << "  decision: " << decision;
}

// New — the guard that has never executed. A catalog with one stats-less table
// sends joinCardinality down its non-multiplicative max(l, r) branch, which is
// the exact condition the written-order bound exists to contain.
TEST(JoinEnumeration, WrittenOrderBoundIsReachableWithoutStatistics) {
    Catalog cat(CATALOG);
    seedStatsExcept(cat, "drivers");     // new helper: seeds every table but one
    auto plan = optimize("<a 3+ relation shape joining laps and drivers>", cat);
    const std::string decision = decisionOf(plan.get());
    auto [chosen, written] = costs(decision);
    EXPECT_LE(chosen, written);
    // whichever of dp / written-floor produced it, the printed method must name
    // the search that actually ran
    EXPECT_TRUE(decision.find("method=dp") != std::string::npos ||
                decision.find("method=written-floor") != std::string::npos) << decision;
}

// Week 29 — the decline. No order= line at all, because there was no decision.
TEST(JoinEnumeration, DeclinesAnyTreeContainingAnOuterJoin) {
    Catalog cat(CATALOG);
    seedStats(cat);
    auto plan = optimize(
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "LEFT JOIN drivers d2 ON d.team = d2.team", cat);
    EXPECT_TRUE(decisionOf(plan.get()).empty());
    // and the written order survives: the bottom join's left child is relation 0
}
```

```python
# python_tools/test_new_queries.py — run_join_order_steering

# Week 29: cost <= written holds by construction (reorder() clamps before it
# prints), so it cannot fail and never could. method=dp is the assertion with
# content — it says the search did not fall back to the written-order bound.
m = re.search(r"method=(\w+)", section)
method_ok = m is not None and m.group(1) == "dp"
```

### Implementation guidance

1. Do this **before** Task 5's test, in its own commit. A green run of the fixed
   assertion on the pre-Week-29 tree is the evidence that the fix is a fix rather
   than a rewrite that happens to pass.
2. `seedStatsExcept` is a small change to the existing `seedStats` helper — pass
   a table name to skip. Do not build a second fixture harness.
3. The stats-less test asserts a *disjunction* on purpose: which branch fires
   depends on the cost numbers, and pinning the wrong one makes the test brittle
   without making it stronger. What must hold is that `method=` names the search
   that actually ran.

### Verification

```bash
cd build && ./tests/swiftql_tests --gtest_filter='JoinEnumeration.*' && cd ..
python3 python_tools/test_new_queries.py
```

Then confirm the fixed assertion is not vacuous: temporarily make `reorder()`
return the written order unconditionally, re-run, and watch
`NeverInstallsAnOrderWorseThanTheWrittenOne` and the python steering check
**fail**. Revert. An assertion you have not seen fail is not evidence.

---

## The verification gate, and this week's success criteria

Run in this order; each stage is cheaper than the next and catches different
things.

```bash
# 1. build + unit tests
cmake --build build -j$(nproc)
cd build && ./tests/swiftql_tests && cd ..

# 2. the SQLite oracle, all four modes — the highest-value gate this week,
#    because it is the first time NULLs from real data reach a result
python3 python_tools/compare_against_sqlite.py

# 3. the invariant / steering harness (optimized ≡ --no-optimize, join steering)
python3 python_tools/test_new_queries.py

# 4. the project's full gate
#    (the `verify` skill: build, unit tests, SQLite harness, regression across modes)
```

### Harness wiring (do not skip — this is where the week is actually proved)

Add a `WEEK29_OUTER_JOIN_QUERIES` block to `compare_against_sqlite.py`, appended
to `QUERIES` so it runs in **all four modes** (or to the vectorized-only block
plus an `OUTER_VOLCANO_REJECTED` list, if you took the refusal option in Task 7).
SQLite supports `LEFT JOIN` natively, so it is a true oracle here.

Two harness properties to respect, both already documented in that file:

- `normalize()` maps SwiftQL's printed `NULL` and SQLite's `None` to the same
  token, so null-extended rows diff correctly. Its known blind spot is that rows
  are keyed by column **name**, so duplicate names collapse — **project named,
  aliased columns**, never `SELECT *`, exactly as the Week 27/28 blocks do.
- Any query with `ORDER BY` must be compared with `preserve_order=True`.

Minimum query set, each one pinned to a specific failure it can catch:

| Query shape | Catches |
|---|---|
| `d LEFT JOIN l ON d.driver_id = l.driver_id` — plain, unmatched drivers exist | unmatched preservation at all (Task 6) |
| the same with `COUNT(l.lap_id)` and `COUNT(*)` per driver | `COUNT` counting NULLs — the Q13 semantic |
| `... ON k = k AND l.speed > 340` | the residual fold (Task 2) |
| `... WHERE l.season = 2024` | pushdown through the null-supplying side (Task 4) |
| `... WHERE d.age > 30` | pushdown on the preserved side still happening |
| `... WHERE l.lap_id IS NULL` | the validity mask surviving materialization |
| `l JOIN d ON ... LEFT JOIN d2 ON ...` (3 relations, vectorized only) | the enumeration decline (Task 5) and lowering recursion |
| `... ORDER BY l.speed` over null-extended rows | `compareForSort`'s NULL ordering against SQLite |
| `SELECT DISTINCT l.team FROM d LEFT JOIN l ON ...` | NULL as its own group in the dedup key |

### Success criteria

Do not call the week done until every one of these is a command you have run and
read the output of:

1. `LEFT JOIN` and `LEFT OUTER JOIN` parse; a bare `JOIN` is unchanged.
2. Every one of the nine harness shapes above matches SQLite in every mode the
   feature is supported in.
3. Optimized and `--no-optimize` return identical rows on all of them.
4. **Every pre-existing `--explain` string is byte-identical.** Diff a handful
   of inner-join plans against the pre-week binary.
5. `--explain` on a tree containing an outer join prints **no** `order=` line;
   the all-inner control still prints `method=dp`.
6. `--explain` never prints `algo=simd` for an outer join.
7. `COUNT(col)` is 0 and `COUNT(*)` is 1 for an unmatched preserved row.
8. A preserved row with a NULL join key is emitted (unit test, since invariant 14
   means CSV cannot produce one — build the chunk by hand).
9. The written-order-bound assertions fail when you break `reorder()` on purpose,
   and pass when you revert.
10. README updated: Feature Scope (`LEFT [OUTER] JOIN`), the Week 29 section's
    result table, Limitations (forced build side for outer joins; the sharpened
    NaN bullet; the Volcano capability line if you refused), and the "Syntax
    Deliberately Not Supported" row for Task 8a.

### Hand this forward

Write the Week 29 Starting notes for Week 30 as you go, not at the end. At
minimum, these three are already visible from here:

- **The forced build side is a real regression waiting for a measurement.** An
  outer join always hashes the null-supplying side, which is the larger one in
  Q13's shape. Week 37's profiling is where a build-side-preserved variant
  (matched flags + drain) earns its keep, or does not.
- **The outer-join decline in `JoinEnumeration` is a blunt instrument**, and it
  becomes blunter as queries grow: one outer join turns off ordering for the
  whole query. Conflict/eligibility sets are the standard fix and the first week
  that has a supported query paying for it should say so.
- **Week 30's subquery scans will make `JoinEnumeration`'s `e.slot_a >= n` check
  fire on legitimate plans** (Week 28's third Starting note). Your outer-join
  decline is the `return node;` precedent it should follow.
