---
description: Detect when SwiftQL's implementation behavior diverges subtly from correct SQL semantics — join cardinality, aggregation edge cases, alias scoping, NULL semantics, and duplicate handling.
---

# Semantic Drift Detection

You are detecting semantic drift: cases where SwiftQL's implementation produces results that are syntactically valid C++ but semantically wrong relative to SQL. These bugs pass compilation and often pass basic tests — they manifest only on specific data patterns.

## What Semantic Drift Looks Like

Semantic drift is not a crash or a compile error. It is a query that returns:
- The right number of rows with wrong values
- Wrong number of rows with correct-looking values
- Correct results on simple cases but wrong results on edge cases
- Results that differ from SQLite on the same data

---

## Check 1 — Join Cardinality

**Correct semantics:** An inner join between tables A and B on key K produces exactly one output row for each (a_row, b_row) pair where `a_row.K == b_row.K`. If A has 3 rows with K=1 and B has 2 rows with K=1, the join produces 6 rows.

**Drift patterns to check:**
- Build side has duplicate keys → probe row matches all of them (should) — verify all matches are emitted, not just the first
- Probe row has no match → row is silently dropped (inner join) — verify it is not emitted as a NULL-padded row
- Join key is NULL on either side → NULL != NULL, so no match (verify NULL keys do not join)
- Multiple join predicates → all conditions must hold simultaneously, not just one
- Output column order: left table columns first, then right table columns — verify no column swap

**How to verify:** Run a join query where both tables have duplicate key values and verify row count = `count_left_matches * count_right_matches`.

---

## Check 2 — Aggregation Edge Cases

**Drift patterns to check:**

| Function | Edge Case | Correct Behavior | Drift Risk |
|---|---|---|---|
| `COUNT(*)` | All rows NULL | Returns row count (NULLs counted) | Returns 0 |
| `COUNT(col)` | All values NULL | Returns 0 (NULLs excluded) | Returns row count |
| `AVG(col)` | All values NULL | Returns NULL | Returns 0 or NaN |
| `SUM(col)` | All values NULL | Returns NULL | Returns 0 |
| `MIN(col)` / `MAX(col)` | All values NULL | Returns NULL | Returns 0 or crashes |
| `AVG(col)` | Single row | Returns that value | Division by zero |
| `GROUP BY col` | col has NULL values | NULL values form their own group | NULLs dropped or merged |
| `HAVING` | No rows pass | Returns empty result | Crashes or returns wrong group |

**How to verify:** Test each aggregate against SQLite with a table that contains NULLs in the aggregate column.

---

## Check 3 — NULL Semantics Drift

SwiftQL scopes NULL handling to `IS NULL` / `IS NOT NULL`. The drift risk is that NULL values leak into comparison expressions and produce wrong filter results.

**Drift patterns to check:**
- `WHERE col = 5` where `col` is NULL → row should be dropped (NULL is not equal to 5)
- `WHERE col != 5` where `col` is NULL → row should be dropped (NULL is not not-equal to 5)
- `WHERE col > 5 AND col < 10` where `col` is NULL → row should be dropped (both conditions are NULL)
- `WHERE col IS NULL OR col > 5` where `col` is NULL → row should be kept (first condition is TRUE)
- NULL in ORDER BY: NULLs should sort consistently (document whether first or last)
- NULL in GROUP BY key: NULLs should form their own group (NULL == NULL for grouping only)

**How to verify:** Insert a row with a NULL value in a filtered column and verify it is excluded from results.

---

## Check 4 — Alias Scoping

**Correct semantics:** Column aliases defined in SELECT are not visible in WHERE or HAVING — only in ORDER BY (in some SQL dialects).

**Drift patterns to check:**
- `SELECT team AS t FROM laps WHERE t = 'Ferrari'` — `t` should not be resolvable in WHERE
- `SELECT AVG(speed) AS avg_speed FROM laps HAVING avg_speed > 300` — this is ambiguous; SwiftQL should either support it consistently or reject it
- Alias collision: `SELECT team AS team FROM laps` — no drift expected, but verify schema name is correct

---

## Check 5 — Duplicate Handling

**Drift patterns to check:**
- `SELECT DISTINCT` with NULL values: two NULL values in the same column are considered equal for deduplication
- `SELECT DISTINCT` on multiple columns: two rows are duplicates only if all columns match
- `GROUP BY` implicitly deduplicates by group key — verify no phantom groups appear
- `DISTINCT` + `ORDER BY`: ordering applied after deduplication

---

## Check 6 — Ordering Guarantees

**Correct semantics:** Without `ORDER BY`, result row order is undefined. With `ORDER BY`, rows are ordered by the specified columns.

**Drift patterns to check:**
- Multiple rows with the same ORDER BY key: sort stability (equal keys preserve input order, if guaranteed)
- `ORDER BY` column not in SELECT list — verify the column is accessible at sort time even if not projected
- `LIMIT` without `ORDER BY`: any N rows returned — verify not always the first N rows from storage (which would be correct but fragile)

---

## Output Format

For each drift pattern detected:
```
Category: Join Cardinality
Query:    SELECT * FROM laps l JOIN drivers d ON l.driver_id = d.driver_id WHERE l.driver_id = 1
Data:     laps has 3 rows with driver_id=1; drivers has 2 rows with driver_id=1
Expected: 6 rows (3 × 2)
Actual:   2 rows (only first match per probe row)
Location: src/execution/plan_nodes.cc:HashJoinNode::next() — breaks after first match found
Fix:      Iterate all matching build rows, not just the first
```

Conclude with a severity ranking: **silent data loss** > **wrong values** > **wrong row count** > **wrong ordering**.
