# Seam audit — engine divergence (Volcano vs vectorized), PASS 5 — FINAL

Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `b14d086`. Predecessors read in
full: `-pass-1.md` … `-pass-4.md`.

Gate state at start, reported by the orchestrator on this exact tree: 910 unit / 98 suites; oracle
1722 passed / 0 failed / 0 errors; regression 340/0/0; TPC-H `PASS (20/22 meaningful)`, baseline md5
unchanged; tracked fingerprint identical before and after; staleness disproved both ways (rebuilt
binaries bit-identical); three refusal-pin matrices clean (7/17/5 pins); rejection sweep 35 suites /
257 entries.

All runs below use the tree's own `build/swiftql`, in the **six meaningful mode combinations** —
`--storage row|columnar` x `--execution volcano|vectorized` x `--optimize|--no-optimize`, minus the
two `row`+`vectorized` cells the CLI refuses outright:

```
rv    row/volcano             rvn   row/volcano --no-optimize
cv    columnar/volcano        cvn   columnar/volcano --no-optimize
vec   columnar/vectorized     vecn  columnar/vectorized --no-optimize
```

Status: IN PROGRESS — appended as each item is confirmed.

---

## Part B — the seam taken fresh (written first, because it found the blocker)

### B-1 FINDING **E-19 (BLOCKER)** — a SILENT WRONG ANSWER, no error, no contrivance beyond one
### ordinary multiplication. `MAX`/`MIN` over a mixed-type `CASE` hands Volcano an **INT Value** and
### the vectorized engine a **DOUBLE cell**, and the first `+`, `-` or `*` above it carries that
### difference past the `%.15g` cliff, where the two engines print different numbers. SQLite
### adjudicates: **Volcano is right, the vectorized engine is wrong.**

Run at HEAD, shipped `catalog.json`, `laps` (10,000 rows). No derived table, no `--no-optimize`, no
`LIMIT`, no 15-digit literal — every constant is nine digits:

```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 123456789 ELSE 0.5 END) * 987654321 AS y FROM laps
```
```
row/volcano                y = 121932631112635269
row/volcano/noopt          y = 121932631112635269
columnar/volcano           y = 121932631112635269
columnar/volcano/noopt     y = 121932631112635269
columnar/vectorized        y = 1.21932631112635e+17     <-- WRONG
columnar/vectorized/noopt  y = 1.21932631112635e+17     <-- WRONG
```

**The oracle agrees with Volcano.** `sqlite3` on the same shape:
```
SELECT MAX(CASE WHEN lap_id = 1 THEN 123456789 ELSE 0.5 END) * 987654321  ->  121932631112635269
SELECT typeof(MAX(CASE WHEN lap_id = 1 THEN 123456789 ELSE 0.5 END))      ->  'integer'
```

**The control that isolates it to the materialization**, same product, no `CASE`:
```sql
SELECT 123456789 * 987654321 AS y FROM laps LIMIT 1
   all six modes: 121932631112635269
```
So the difference is not the arithmetic, not the printer and not the storage. It is one cell.

**The mechanism, both halves read rather than inferred.**

- Volcano: `HashAggregateNode`'s MIN/MAX keeps the argument `Value` verbatim (plan_nodes.cc:311),
  so `MAX` returns the **INT** `123456789`. `evaluate()`'s `*` then takes the INT/INT branch
  (evaluator.cc:140-148) and `checkedMul` produces the exact `121932631112635269`, which
  `Value::toString` prints in full because it is an INT.
- Vectorized: `VecHashAggregateNode::fillChunk` appends that same INT `Value` into a column whose
  type was declared **before the value existed** — `inferExprType` types a mixed `CASE` as DOUBLE —
  so `appendColumnValue` runs it through both refusals and **both pass**: `int_narrowing` is
  `RENDERED` (the column IS in the root's output sets) and `narrowToDoubleColumn` compares the
  **stored value** `123456789` against `1e15`. The cell becomes the double `123456789.0`. The
  projection above then does a DOUBLE `*`, giving `1.2193263111263526e17`, and `%.15g` prints
  fifteen significant digits.

**The false premise, in the code's own words** (`vectorized_plan_builder.cc:84-86`, the paragraph
titled *"Why `/` alone and not 'consumed by any expression'"*):

> `+ - *` on INT/INT give an INT whose `%.15g` rendering is identical to the DOUBLE (`x+1` is "8"
> either way), so `WHERE x+1 > 3` and `SELECT x*2` agree across engines.

That is a statement about the **operand**, and the rendering cliff applies to the **result**. A
single `*` moves a value from `1.2e8` to `1.2e17` — past `2^53`, where the double is no longer the
integer and `%.15g` is no longer the integer's text. `narrowToDoubleColumn` guards the operand and
nothing guards the result, because by then the vectorized value is a **genuine DOUBLE**, and both
refusals are `if (v.type() != TypeId::INT) return` by construction. `refuseObservableIntNarrowing`
arms `/` only, deliberately — so the one operator family that is armed is the one that CANNOT
produce this, and the three that are not armed all can.

**Three more routes, all run, all the same split (Volcano/SQLite exact, vectorized `%.15g`):**

| shape | Volcano (4 modes) | vectorized (2 modes) |
|---|---|---|
| `MAX(CASE WHEN lap_id=1 THEN 999999999999999 ELSE 0.5 END) + 9000000000000000` | `9999999999999999` | `1e+16` |
| `MAX(CASE WHEN lap_id=1 THEN 999999999999999 ELSE 0.5 END) - 9000000000000000` | `-8000000000000001` | `-8e+15` |
| `MIN(CASE WHEN lap_id=1 THEN 123456789 ELSE 999999999.5 END) * 987654321` | `121932631112635269` | `1.21932631112635e+17` |

**And a fourth face of the same defect, this one an ERROR/ANSWER split rather than a wrong answer** —
SwiftQL's INT arithmetic is overflow-checked and its DOUBLE arithmetic is not, so the same one-cell
type difference decides whether the query throws:

```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 7 ELSE 0.5 END) * 9223372036854775807 AS y FROM laps
   4 Volcano modes  Error: integer overflow in '*': the result does not fit in a 64-bit integer
   2 vec modes      y = 6.45636042579834e+19
```
It fires at magnitude **7** — the stored value is irrelevant, only the multiplier is — and with an
ordinary-looking multiplier too (`MAX(CASE … THEN 4000000000 ELSE 0.5 END) * 4000000000` -> Volcano
errors, vectorized answers `1.6e+19`). The `+` form errors the same way. Here SQLite sides with the
*vectorized* engine (`6.456360425798343e+19`, it promotes), so this face is a three-way split:
SwiftQL-Volcano throws, SwiftQL-vectorized and SQLite answer.

**This route is left open ON PURPOSE, and its stated compensating control does not compensate.**
The same comment continues:

> The one route left open on purpose is checked INT overflow: `x * <huge>` throws in Volcano and
> yields a double here. **That is a magnitude story like `narrowToDoubleColumn`'s**, not this type
> story, and arming for it would refuse `SELECT x*2`, which is right today.

`narrowToDoubleColumn` bounds the **stored** value. Overflow is a property of the **product**. The
two are independent: the wrong answer above stores `123456789` and the overflow above stores `7`,
both four to fourteen orders of magnitude below either bound. So the magnitude rule was never
capable of covering the route the type rule delegated to it, at any bound — which is why this is a
defect and not the accepted residue the sentence describes.

**Ranked BLOCKER**, on the criterion passes 2 and 3 set and pass 4 had to argue around: this one IS
a silent wrong answer. Both engines return a row, neither warns, the two rows differ, and the
declared oracle picks Volcano. It needs no derived table (which Volcano refuses), no `--no-optimize`,
no `LIMIT`, no plan-order accident and no near-`2^53` literal — the largest constant in the minimal
repro is `987654321`. It is reachable on the shipped catalog in the first shape anyone writes when
mixing an integer and a real branch under `MIN`/`MAX`.

**Corpus coverage: measured 0.** Harvested every `SELECT` string literal from
`compare_against_sqlite.py`, `test_new_queries.py` and `tpch_queries.py` and matched
`(MIN|MAX)\s*\(\s*CASE`: **9 literals, and not one of them has a `+`, `-` or `*` above the
aggregate.** All nine consume it through a comparison (`> 150`, `> 300`, `> 3`, `> 1`), through
`/ 2.0` (the arm that IS screened), or bare (`AS m`). The pinned `9223372036854775807` literals
(compare_against_sqlite.py:2481/2505, test_new_queries.py:1445) are all `lap_id * <huge>` inside a
`WHERE`, i.e. pass 4's ROW-SET class, which is a different mechanism reached through a different
node. `tests/test_int_double_materialization.cc` and
`tests/test_int_double_type_through_division.cc` pin the `/` arm and both magnitude bounds and
contain the string "overflow" zero times.

**A FIFTH face, and the one that leaks a C++ internal to the user.** `AND`/`OR` call `asInt()` on
each operand, and `asInt()` on a DOUBLE is `std::bad_variant_access`. `expr_totality.h:185-189`
documents that consumer exactly — *"asInt() on a non-INT operand raises std::bad_variant_access
(evaluator.cc:112-113, value.cc:28)"* — so the totality screen knows about it and the arming pass
does not:

```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 1 ELSE 0.5 END) AND 1 AS y FROM laps
   4 Volcano modes  y = 1                                    (SQLite: 1)
   2 vec modes      Error: std::get: wrong index for variant
```
The control that separates the ENGINE from the DIALECT: with the aggregate written bare in a
`HAVING` — where Volcano also holds a DOUBLE, because the value came from the storage and not from a
mixed `CASE` — all six modes give the same `std::get` error. It is only the materialization that
splits them. (That both engines surface a raw `std::bad_variant_access` text rather than a SwiftQL
diagnostic is a separate, smaller matter; it is recorded here because it is what the user of the
diverging query sees.)

**Every consumer, enumerated, so the finding is a class and not five queries.** The materialized cell
differs from Volcano's `Value` in exactly one bit — INT versus DOUBLE — and the dialect has five
places that read it:

| consumer | armed by `taintWalk`? | diverges? |
|---|---|---|
| `/` with an INT partner | **yes** (`OBSERVABLE`) | no — refused, loudly |
| `/` with a REAL partner | no, correctly | no — INT/REAL and REAL/REAL agree |
| `+ - *` result past `2^53` | no | **YES — silent wrong answer** |
| `+ - *` result past INT64 | no | **YES — Volcano throws, vectorized answers** |
| `AND` / `OR` (`asInt()`) | no | **YES — Volcano answers, vectorized `std::get` error** |
| `=`,`<`,`IN`,`ORDER BY`, group/DISTINCT keys | no, correctly | no — all coerce or normalize (re-run, clean) |

So the arming rule covers one of the three consumers that can see the difference, and the two it
misses are the two that were argued away in prose rather than measured.
