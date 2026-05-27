---
description: Simulate SwiftQL query execution row-by-row (Volcano) or chunk-by-chunk (Vectorized) manually, tracking intermediate tuples, filter results, and operator state to verify execution semantics.
---

# Execution State Simulation

You are simulating SwiftQL query execution manually, step by step. The goal is to verify that the engine's output is correct by tracing exactly what each operator does to each row (or chunk).

Use this skill when:
- A query produces a suspicious result and you need to verify which operator is wrong
- You are reviewing a new operator implementation
- You want to confirm NULL handling, join cardinality, or aggregation behavior on a concrete example

## Setup

Before simulating, collect:
1. The query being executed
2. The plan tree (from `--explain` output or by reading `src/planner/planner.cc`)
3. The input data (a small representative sample — 5–15 rows is enough)
4. The execution mode: Volcano (row-at-a-time) or Vectorized (chunk-based)

---

## Volcano Mode Simulation

Simulate the `open()` / `next()` / `close()` lifecycle top-down.

### Format

At each `next()` call, show:
```
[Operator.next()] → calls → [Child.next()]
  child returns: Row{col1=v1, col2=v2, ...}
  [Operator] applies: <predicate / projection / aggregation>
  result: <emits Row{...}> | <drops row> | <accumulates (no emit yet)>
```

### Example

Query: `SELECT team FROM laps WHERE speed > 300`

Input rows:
```
Row 0: {team=Ferrari, speed=312, season=2025}
Row 1: {team=McLaren, speed=295, season=2025}
Row 2: {team=Mercedes, speed=310, season=2025}
```

Plan:
```
ProjectNode [team]
  FilterNode [speed > 300]
    SeqScanNode [laps]
```

Simulation:
```
Project.open() → Filter.open() → SeqScan.open()
  SeqScan state: cursor=0

--- Call 1: Project.next() ---
  Filter.next() → SeqScan.next()
    SeqScan returns: Row{team=Ferrari, speed=312, season=2025}, cursor→1
  Filter evaluates: 312 > 300 → TRUE
  Filter emits: Row{team=Ferrari, speed=312, season=2025}
  Project evaluates: [team] → Row{team=Ferrari}
  Project emits: Row{team=Ferrari}  ✅

--- Call 2: Project.next() ---
  Filter.next() → SeqScan.next()
    SeqScan returns: Row{team=McLaren, speed=295, season=2025}, cursor→2
  Filter evaluates: 295 > 300 → FALSE
  Filter drops row, calls SeqScan.next() again
    SeqScan returns: Row{team=Mercedes, speed=310, season=2025}, cursor→3
  Filter evaluates: 310 > 300 → TRUE
  Filter emits: Row{team=Mercedes, speed=310, season=2025}
  Project evaluates: [team] → Row{team=Mercedes}
  Project emits: Row{team=Mercedes}  ✅

--- Call 3: Project.next() ---
  Filter.next() → SeqScan.next()
    SeqScan returns: nullptr (cursor=3, exhausted)
  Filter returns: nullptr
  Project returns: nullptr

Project.close() → Filter.close() → SeqScan.close()

Final output:
  Row{team=Ferrari}
  Row{team=Mercedes}
```

---

## Pipeline Breaker Simulation

For `HashAggregateNode`, `SortNode`, and `DistinctNode`, the operator must consume all input before emitting output. Simulate the build phase and emit phase separately.

Example for `HashAggregateNode [group_by=team, agg=COUNT(*)]`:
```
--- BUILD PHASE (consuming all input) ---
  next() → Row{team=Ferrari, speed=312}  → hash_map[Ferrari].count = 1
  next() → Row{team=McLaren, speed=295}  → hash_map[McLaren].count = 1
  next() → Row{team=Ferrari, speed=301}  → hash_map[Ferrari].count = 2
  next() → nullptr → build phase complete

hash_map state:
  Ferrari → count=2
  McLaren → count=1

--- EMIT PHASE ---
  Emit Row{team=Ferrari, COUNT(*)=2}
  Emit Row{team=McLaren, COUNT(*)=1}
  Emit nullptr
```

---

## Vectorized Mode Simulation

For vectorized execution, simulate at chunk granularity.

```
VecScan.nextChunk() → DataChunk{
  columns: [team=[Ferrari, McLaren, Mercedes], speed=[312, 295, 310]]
  num_rows: 3
}

VecFilter evaluates speed > 300 over chunk:
  Row 0: 312 > 300 → PASS
  Row 1: 295 > 300 → FAIL
  Row 2: 310 > 300 → PASS
  SelectionVector: {indices=[0, 2], size=2}  ← no data copied

VecProject materializes only passing rows for output columns [team]:
  Index 0: team=Ferrari
  Index 2: team=Mercedes
  Output DataChunk{columns: [team=[Ferrari, Mercedes]], num_rows: 2}
```

---

## NULL Simulation

When simulating NULL values, show the three-valued result explicitly:

```
Row: {team=Ferrari, speed=NULL, season=2025}
Filter evaluates: NULL > 300 → NULL (not TRUE, not FALSE)
Filter: NULL is falsy → drops row
```

```
IsNullExpr evaluates: speed IS NULL → TRUE (speed is null)
Filter: TRUE → keeps row
```

---

## Output

End the simulation with:
1. The complete list of emitted output rows
2. Whether this matches the expected result (from SQL semantics or SQLite)
3. If there is a mismatch: which operator produced the wrong result and why
