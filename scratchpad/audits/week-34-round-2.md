# Week 34 Round 2 Audit — IN PROGRESS

Range: fe17f40..b1cf07e (ignoring `chore:` scratchpad-only commits), branch
claude/phase5-week26-qomtkb. Started ~15:41 UTC.

Targets, in priority order:
1. harness suite definitions in `python_tools/compare_against_sqlite.py`
2. the three Week 34 fixes as new code (COUNT CASE wrapper, VecDerivedNode row count,
   derived-table alias execution tests)
3. Q17 end to end
4. anything else risky

## Findings

(appending as confirmed)

---

## F1 — MEDIUM — the only TWO-CORRELATION-KEY oracle entry cannot fail if a key is dropped

`python_tools/compare_against_sqlite.py:1042-1044` (entry index 2 of
`WEEK34_CORRELATED_SCALAR_VEC_ONLY`), whose comment claims "TWO correlation keys,
and MAX rather than AVG":

```sql
SELECT COUNT(*) AS n FROM laps l WHERE l.speed > (SELECT MAX(l2.speed) FROM laps l2
WHERE l2.driver_id = l.driver_id AND l2.team = l.team)
```

The correlated body's group always CONTAINS the outer row itself, so
`MAX(l2.speed) >= l.speed` for every row by construction and the answer is `n = 0`
no matter what the join does. Measured on the shipped data, `./build/swiftql
--execution vectorized --storage columnar --no-cache`:

| query | n |
|---|---|
| both keys (the suite entry) | 0 |
| `l2.driver_id = l.driver_id` only | 0 |
| `l2.team = l.team` only | 0 |

Dropping either correlation key changes nothing, so the entry does not test the
property its comment names. Worse, `0` is also what a total failure produces — a
rewrite that emitted no rows, or that left the scalar NULL for every key, passes
this entry identically. It is the "returns the trivially-safe answer" class the
week already fixed once (the 0.2-coefficient IN entry at `:1112-1117`, which the
author caught and paired with a discriminating 1.10 twin at `:1146-1149`); this one
was not caught, and it is the **only** two-key entry in the week, so
`splitCorrelation` emitting one join key instead of two is currently invisible to
the oracle.

Minimal fix: make the body's group exclude the outer row, e.g.
`... AND l2.lap_id <> l.lap_id`, or compare against a key set that discriminates —
`l.speed > (SELECT MAX(l2.speed) FROM laps l2 WHERE l2.driver_id = l.driver_id
AND l2.season = 2024)` differs from its one-key form. Any variant whose one-key and
two-key answers differ on this data restores the property.
