# Seam audit — optimizer result preservation (pass 4)

HEAD `b2bc70e`, branch `claude/phase5-week26-qomtkb`. Binary: `build/swiftql`,
newer than every file under `src/` (verified), so it is HEAD's build. No source
file touched; every measurement below is a single-query CLI invocation with
`--no-cache`.

Predecessors: `seam-optimizer-preservation-pass-1.md`, `-2.md`, `-3.md`.

Status: IN PROGRESS (written incrementally; summary block at end).

---

## BLOCKER-CLASS FINDING P4-1 — `mayRaise` is UNDER-approximate. The totality
## screen misses the whole class of TYPE-MISMATCHED COMPARISONS, and B3-2
## reopens in both directions, through two independent raise sites.

The fix round's precondition rests on one load-bearing sentence
(`predicate_pushdown.cc:378-384`):

> What it does NOT have to cover, because `inferExprType` (logical_plan.cc)
> decides it at PLAN time — in both legs, before any pass in this file runs — is
> every TYPE error: STRING arithmetic, a non-STRING LIKE or SUBSTRING operand, a
> mixed IN list, a CASE with mixed branches. Those raise identically in both legs
> no matter how the conjuncts are arranged.

**That list is missing the most common type error of all, and it is missing it
because `inferExprType` does not check it.** `logical_plan.cc:159-173`, the
`BinaryExpr` branch:

```cpp
TypeId l = inferExprType(bin->left.get(), schema);
TypeId r = inferExprType(bin->right.get(), schema);
const std::string& op = bin->op;
if (op == "+" || op == "-" || op == "*" || op == "/") {
    if (l == TypeId::STRING || r == TypeId::STRING)
        throw std::runtime_error("'" + op + "' requires numeric operands");
    ...
}
return TypeId::INT;   // comparison / AND / OR
```

For `=`, `!=`, `<`, `>`, `<=`, `>=` it recurses into both children and returns
INT **without ever comparing `l` against `r`**. So `team = 5` (STRING column,
INT literal) type-checks at plan time and raises PER ROW, from
`Value::checkMatchingType` / the `NUMERIC_COERCE` macro (`common/value.cc:33,57`
— *"Type mismatch in Value comparison"*). Confirmed standalone, both legs:

```
$ swiftql ... --query "SELECT team FROM laps WHERE team = 5 LIMIT 3"
Error: Type mismatch in Value comparison        (identical with --no-optimize)
```

`mayRaise` (`predicate_pushdown.cc:413-417`) answers **false** for that conjunct:
a `BinaryExpr` whose op is not arithmetic recurses, `ColumnRef` → false,
`Literal` → false. So `team = 5` is classified TOTAL, is freely permuted by
`orderByWork`, and is freely pushed below an inner join by `distribute`. A
missed subtype "answers total and is therefore the unsafe direction" — the
screen's own words, four lines above `mayRaise`.

### The four measured divergences

Shipped `catalog.json`, `--execution vectorized --storage columnar --no-cache`.

| # | query | optimized | `--no-optimize` |
|---|---|---|---|
| 1 | `SELECT team FROM laps WHERE team = 5 AND speed > 999999` | **0 rows** | **Error: Type mismatch in Value comparison** |
| 2 | `SELECT team FROM laps WHERE team > 'zzzzz' AND team = 5` | **Error: Type mismatch in Value comparison** | **0 rows** |
| 3 | `SELECT team FROM laps WHERE 5 = team AND speed = 333.3333` | **0 rows** | **Error: Type mismatch in Value comparison** |
| 4 | `SELECT team FROM laps WHERE team LIKE 'zzz%' AND 5 = team` | **Error: Type mismatch in Value comparison** | **0 rows** |

1 and 3 are the MASK direction; 2 and 4 are the INTRODUCE direction — the
optimizer turning a query that succeeds into one that fails. `--explain` shows
the mechanism directly in each case, e.g. for #2:

```
=== Logical Plan ===
  LogicalFilter [((team > zzzzz) AND (team = 5))]
=== Optimized Logical Plan ===
  LogicalFilter [((team = 5) AND (team > zzzzz))]
```

### TWO raise sites, not one, and the second is not in `evaluate()` at all

Pairs 1/2 and 3/4 fail through **different code**, which matters because a fix
aimed only at `mayRaise`'s `evaluate()` model closes half of it.

* **3 and 4 raise from `evaluate()`.** `5 = team` puts the Literal on the LEFT,
  so `ChunkPruner::collectSimplePredicates` (which requires ColumnRef-op-Literal,
  `chunk_pruner.h:61-71`) never collects it; the throw comes from the per-row
  `evalFallback` loop in `columnar_eval.cc:117-125`. `ExpressionExecutor` declines
  the shape on purpose (`expression_executor.cc:579-583`, *"STRING vs numeric
  throws in Value's comparison operators; decline so the fallback raises the same
  error from the same place"*), so the compiled kernel is not involved.

* **1 and 2 raise from `ChunkPruner::canSkipChunk`** (`chunk_pruner.h:76-86`):
  `if (op == "=") return val < mn || val > mx;` — `val` is `Value(5)`, `mn` is a
  STRING, and `Value::operator<` throws. This raise is **not reachable from
  `mayRaise`'s model at all**: it happens at SCAN time, on zone-map metadata,
  before a single row is evaluated, and the conjunct never had to be reached by
  the AND cascade. `ChunkPruner::shouldSkip` walks `preds` in **conjunct order**
  and returns on the FIRST predicate that proves a skip, so a prunable conjunct
  ordered ahead of `team = 5` short-circuits the throw and one ordered behind it
  does not. That is the entire mechanism of #1 and #2.

  Control, isolating it: with the SAME second conjunct, the outcome is decided by
  whether the FIRST conjunct proves a skip.

  | first conjunct | `... AND team = 5`, `--no-optimize` |
  |---|---|
  | `speed = 333.3333` (in range, proves nothing) | **Error** |
  | `speed > 999999` (proves skip) | 0 rows |
  | `lap_id < 0` (proves skip) | 0 rows |
  | `team = 'zzzz'` (proves skip) | 0 rows |
  | `lap_id = 999999` (proves skip) | 0 rows |
  | `sector_1 = 12345.5` (proves skip) | 0 rows |

  Note what this also means: the pruner raise is order-sensitive **even without
  the optimizer**. The optimizer is what makes the two legs disagree.

### Mode census (query #2, the INTRODUCE direction)

| mode | optimized | `--no-optimize` |
|---|---|---|
| `columnar` + `vectorized` | **Error** | 0 rows |
| `columnar` + `volcano` | see below | see below |
| `row` + `volcano` | Error | Error |
| `row` + `vectorized` | refused (needs columnar), both legs | — |

Volcano's `evaluate()` is EAGER for AND (`evaluator.cc:104-107` computes both
operands before the three-valued rule), so it raises in both legs regardless of
order — the same asymmetry pass 3 recorded for `SUBSTRING`.

### Ranking

Ranked **HIGH**, by the precedent pass 3 set and for the same reason it gave:
`optimized != --no-optimize` on CLI-typable queries on the shipped catalog, in
both directions, but the failing side is LOUD. It is not a wrong row. It is
recorded first because it is a **reopening of a finding the fix round declared
closed**, against a precondition that was written down and is false as written:
`inferExprType` does not decide "every TYPE error" at plan time.
