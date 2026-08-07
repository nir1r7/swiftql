# Week 33 — Round 2 Audit (the fix round as new code)

Range: `ecee221..11f2cee`, excluding `chore:`-only scratchpad commits.
Round 1 findings (`scratchpad/audits/week-33-round-1.md`) are not re-audited.

Status: IN PROGRESS (append-as-confirmed; container may reclaim scratchpad).

## Findings

### R2-C1 (CRITICAL) — the SAME repeat-defect the fix round repaired, unrepaired at its second site: a correlated `IN` subquery is lowered to a semi-join with the outer ref resolved by bare name

**Site of the stale precondition:** `src/planner/subquery_lowering.h:25-31`

```
// PRECONDITIONS, all established by Validator::validate, which both planner
// entry points run FIRST — the same trust-don't-recheck stance
// materializeSubqueries takes ...
//   - no correlated subquery anywhere (refused in Week 33's name), so every
//     ColumnRef in a body is query_level 0 against the BODY's range table;
```

This is the *identical* sentence, citing the *identical* deleted refusal, that
`7c46bdf` removed from `subquery_materialization.cc`. Week 33 deleted the
Validator refusal; `subquery_lowering.{h,cc}` still trusts it. The fix round
repaired one of the two files that named it.

**Why nothing else catches it.** `src/planner/logical_plan.cc:814` runs
`lowerInSubqueries` on the WHERE conjuncts, and `refuseUnloweredCorrelated`
only runs at `src/planner/logical_plan.cc:827` — *after* lowering has already
consumed the conjunct. So the tripwire added this round never sees a correlated
`IN`. `lowerInSubqueries` (`src/planner/subquery_lowering.cc:36-53`) branches
only on `sq->kind != Kind::IN`; it never reads `sq->correlated`, and
`planBody` (`subquery_lowering.cc:19-26`) hands the correlated body straight to
`LogicalPlanBuilder::build`, where the outer ref falls back to bare-name
resolution against the BODY's own schema — `l.team = d.team` becomes
`laps.team = laps.team`, a tautology, exactly the mechanism `7c46bdf`'s message
describes.

**Grounded, reproduced on HEAD** (`--storage columnar --execution vectorized
--no-cache`, project `catalog.json`; SQLite reference computed from the same
two CSVs):

| query | SQLite | SwiftQL |
|---|---|---|
| `SELECT COUNT(*) FROM drivers d WHERE d.driver_id IN (SELECT l.lap_id FROM laps l WHERE l.team = d.team)` | **6** | **20** |
| `SELECT COUNT(*) FROM drivers d WHERE d.driver_id NOT IN (SELECT l.lap_id FROM laps l WHERE l.team = d.team)` | **14** | **0** |
| `SELECT COUNT(*) FROM drivers d WHERE d.age IN (SELECT l.season FROM laps l WHERE l.driver_id = d.driver_id)` | 0 | 0 (agrees by luck — the tautological body still misses) |

No error, no warning: the correlated predicate is silently discarded and the
semi-join degenerates to "does the body have any row at all". This is a silent
wrong answer for every correlated `IN`/`NOT IN`, which is the third time this
project has shipped this defect shape and the second time it has produced a
wrong answer.

**Why the rejection suite does not catch it.**
`WEEK33_CORRELATED_BINDS` (`python_tools/compare_against_sqlite.py:815-866`)
contains no *directly* correlated `IN`. The one nearby entry,
`WEEK33_CORRELATED_NESTED_IN_BODY` (`compare_against_sqlite.py:807-810`), is an
**uncorrelated** `IN` whose *body* holds a correlated scalar — a different
shape, refused for a different reason (the nested scalar), which is why it
passes while this one silently answers.

**Minimal fix:** in `lowerInSubqueries` (`src/planner/subquery_lowering.cc:38`),
skip lowering when `sq->correlated` (push it to `kept`) so it reaches
`refuseUnloweredCorrelated` at `logical_plan.cc:827`; or refuse by name at that
point. Then delete the stale precondition bullet at `subquery_lowering.h:29`
and add both queries above to `WEEK33_CORRELATED_BINDS`.

**Corroboration from inside the fix round itself.**
`src/planner/subquery_decorrelation.h:22-25`, written *this round*, says:

> It is a SIBLING of subquery_lowering.h rather than an addition to it, because
> that header's stated preconditions include "no correlated subquery anywhere",
> **which stops being true this week and must be restated rather than silently
> invalidated.**

It was not restated. `subquery_lowering.h:29` still carries it verbatim, and
`subquery_lowering.cc` still behaves as if it held. The round diagnosed the
defect precisely and repaired one of the two files.

---

### R2-M1 (MEDIUM) — `AnAliasInTheBodysSelectListCannotShadowTheJoinKey` cannot fail on the bug it names

`tests/test_subquery.cc:756-771` (added `12efb77`). It pins round 1's H-2 with:

```cpp
auto plan = planLowered("SELECT d.name FROM drivers d WHERE EXISTS "
                        "(SELECT l.speed AS driver_id FROM laps l "
                        " WHERE l.driver_id = d.driver_id)", cat);
const Schema& body = j->children[1]->output_schema;
ASSERT_EQ(body.size(), 1) << "the body must be projected to its key columns only";
EXPECT_EQ(body.column(0).name, "driver_id") << "and it must be the KEY column, not the aliased one";
```

The alias in the chosen query **is** `driver_id`. Under the pre-fix behaviour
the body's select list survives unchanged, `buildProjectSchema` names the single
output column by its SELECT alias — `driver_id` — and the schema is therefore
also size 1 with name `driver_id`. **Both assertions pass on the defective
code**, so the test would not fail if `subquery_decorrelation.cc:216-217` (the
`select_list = std::move(body_key_refs)` rewrite) were reverted.

What actually distinguishes the two states is the column's TYPE: the key column
`l.driver_id` is `INT`, the aliased `l.speed` is `DOUBLE`. Concrete state
producing the wrong behaviour: revert `subquery_decorrelation.cc:216-217` and
this test still goes green while `SELECT COUNT(*) FROM drivers d WHERE EXISTS
(SELECT l.speed AS driver_id FROM laps l WHERE l.driver_id = d.driver_id)`
returns 0 against SQLite's 20.

**Minimal fix:** add `EXPECT_EQ(body.column(0).type, TypeId::INT);`, or alias to
a name that is not the key (`AS k`). Its two siblings —
`AJoinBodyIsProjectedToItsKeyBeforeTheMergedSchemaCanShadowIt`
(`test_subquery.cc:788-802`, old schema had 14 columns) and
`ABodyThatIsNotSelectStarStillKeepsItsKeyColumn` (`test_subquery.cc:773-786`,
old names were `1`/`name`/5 columns) — do fail on the pre-fix code and are fine.

---

### R2-L1 (LOW) — a third stale citation of the deleted Validator refusal, harmless today

`src/planner/predicate_pushdown.cc:110-113`:

> "NOT reachable from the CLI this week — Validator refuses a bound subquery
> before any logical plan exists — but the branch ships with the node"

That refusal is the one Week 33 deleted, so `if (sq->correlated) out.insert(-1)`
(`predicate_pushdown.cc:115`) *is* now CLI-reachable — via the correlated `IN`
of R2-C1, and via any correlated node that survives materialization into a
conjunct pushdown inspects. Unlike the other two sites this one is **safe**: -1
is the conservative "cannot name it here" sentinel and it only suppresses
pushdown. Comment only; no wrong answer. Flagged because it is the same
sentence pattern and a future reader would take it as still-true.

---

## Targets verified with no finding

**Target 1 — `7c46bdf` itself.** The routing in `subquery_materialization.cc:288-297`
is correct on every path `forEachStatementExpr` + the GROUP BY walk reach: a
correlated node is never run, its body is still recursed for nested uncorrelated
subqueries, and `node_survives` keeps `stmt.has_subquery` set so `buildScanSchema`
does not narrow the key column away. `refuseUnloweredCorrelated`
(`subquery_decorrelation.cc:258`) fires on WHERE residue and HAVING
(`logical_plan.cc:827,862`). **The one path it does not cover is R2-C1**, where
`lowerInSubqueries` at `logical_plan.cc:814` consumes the node 13 lines earlier.

**Target 2 — the `ANTI` / `ANTI_NOT_IN` split.** Complete on the producer side:
`ANTI` is written only at `subquery_decorrelation.cc:234`, `ANTI_NOT_IN` only at
`subquery_lowering.cc:78`; no path crosses. Every consumer was re-checked against
the new enumerator — `cardinality_estimator.cc:469` (`SEMI ? semi : l_rows-semi`,
estimate only, result-invariant), `vec_hash_join_node.cc:279`
(`hit != (semantics_==SEMI)`), and the `!= STANDARD` build-side sites — all
behave identically for the two anti flavours where they must.

Confirmed end-to-end against SQLite on the committed data (columnar+vectorized,
`--no-cache`); all four agree:

| case | query | both |
|---|---|---|
| NULL probe key | `... d LEFT JOIN laps l ON d.driver_id=l.driver_id AND l.lap_id<5 WHERE NOT EXISTS (SELECT * FROM laps l2 WHERE l2.lap_id=l.lap_id)` | 16 |
| NULL build key | `... WHERE NOT EXISTS (SELECT * FROM drivers x LEFT JOIN laps y ON x.driver_id=y.driver_id AND y.lap_id<0 WHERE y.lap_id=d.driver_id)` | 20 |
| empty build side | `VecAntiJoin` unit pins at `tests/test_vectorized.cc:4245,4313` | — |

Both new unit tests (`test_vectorized.cc:4258-4266`, `4300-4315`) fail on the
pre-split code (they assert `{2,3,4}` and a surviving NULL row where the old
`ANTI` emitted nothing).

**Target 3 — the positional build-key projection (`12efb77`).** No name lookup
survives: `rightKeyIndices(..., positional=true)` at
`vectorized_plan_builder.cc:409-411` returns `0..k-1` and never touches
`indexOf`, and it is selected for every `semantics != STANDARD`. Keys cannot be
reordered or dropped — `keys` and `body_key_refs` are `push_back`ed in lockstep
in the same loop iteration (`subquery_decorrelation.cc:82-93`, no intervening
`continue`), and the arity mismatch throws loudly at
`vectorized_plan_builder.cc:110-116` rather than silently mis-indexing.
Exercised on the non-`SELECT *` shapes the round unblocked; all match SQLite:
duplicate key column (`l.driver_id = d.driver_id AND l.driver_id = d.age` → 0),
body with `ORDER BY` (20), multi-column select list (20), `SELECT *` JOIN body
under `NOT EXISTS` (0), nested uncorrelated scalar in the body (20).

**Target 5 — the six repinned unit tests.** Five in `tests/test_logical_plan.cc`
(`:1264`, `:1290`, `:1329`, `:1352`, `:1481`) and
`SubqueryMaterialization.CorrelationPropagatesToTheOutermostStatement`
(`tests/test_subquery.cc:438-464`). Each now names a *distinct* per-shape
message ("a body with GROUP BY…", "no equality links…", "a body with an
aggregate…") rather than a shared catch-all, and each retains its original
negative assertion, so a regression in the behaviour it names changes the
message and fails the test. `CorrelationPropagates…` is strictly stronger than
what it replaced: it now pins `calls == 0` and that the `SubqueryExpr` is still
in the tree. `NotInBecomesAnAntiJoin` (`test_subquery.cc:566-568`) correctly
tightened to `ANTI_NOT_IN`. No weakening found in this set — the one weak
assertion this round added is R2-M1, which is a NEW test, not a repin.

## Tally

| severity | count |
|---|---|
| CRITICAL | 1 (R2-C1) |
| HIGH | 0 |
| MEDIUM | 1 (R2-M1) |
| LOW | 1 (R2-L1) |

## Not reached

Target 4 covered only partially: the two refusals added this round
(`refuseSurvivingCorrelatedRefs`, `subquery_decorrelation.cc:120-137`, and
`requireDecorrelatableBody`'s body shapes) were verified to fire before
`LogicalPlanBuilder::build` consumes the body (`:188` precedes `:218`), to cover
`joins[].condition` / `select_list` / `order_by` / `where`, and to be pinned in
both the unit suite (`test_subquery.cc:715-754`) and the rejection suite
(`compare_against_sqlite.py:854-866`). NOT audited: whether the ON-clause
refusal is *exactly* as narrow as it should be (whether any shape it declines
could be safely lowered), and the interaction of `body.order_by` surviving the
select-list replacement.
