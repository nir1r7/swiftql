---
description: Trace a SQL query through every transformation stage in SwiftQL — SQL string → Lexer → Tokens → Parser → AST → Validator → Planner → Plan Tree → Optimizer → Execution — identifying what changes at each stage.
---

# AST-to-Execution Pipeline Tracing

You are tracing a SQL query through the full SwiftQL transformation pipeline. The goal is to surface what each stage produces and where transformations happen, so bugs at stage boundaries become visible.

## Stages

Given a SQL query, trace it through each stage below. At each stage, show the **input**, the **transformation applied**, and the **output representation**.

---

### Stage 1 — SQL String (Input)

Write out the raw SQL query exactly. Note anything syntactically unusual that might affect parsing.

---

### Stage 2 — Lexer (`src/parser/`)

List the token stream. Format: `[TOKEN_TYPE "raw_value"]`

Example:
```
[SELECT] [DISTINCT] [IDENT "team"] [COMMA] [IDENT "AVG"] [LPAREN] [IDENT "speed"]
[RPAREN] [FROM] [IDENT "laps"] [WHERE] [IDENT "season"] [EQ] [INT_LITERAL "2025"]
[GROUP] [BY] [IDENT "team"] [EOF]
```

Flag any token that is ambiguous (e.g. identifier vs keyword) or that the lexer might misclassify.

---

### Stage 3 — AST (`src/parser/ast.h`)

Show the AST as a structured tree. Use indentation for nesting.

Example:
```
SelectStatement
  distinct: true
  select_list:
    ColumnRef "team"
    AggregateExpr fn=AVG
      ColumnRef "speed"
  from: "laps"
  where:
    BinaryExpr op==
      ColumnRef "season"
      Literal INT 2025
  group_by: ["team"]
```

Note any AST fields that are missing or have unexpected null/default values.

---

### Stage 4 — Validator (`src/planner/validator.cc`)

List each semantic check the validator applies to this query and whether it passes.

```
✅ Table "laps" exists in catalog
✅ Column "team" exists in laps
✅ Column "speed" exists in laps, type DOUBLE (compatible with AVG)
✅ Column "season" exists in laps
✅ "team" appears in GROUP BY (non-aggregate SELECT column check)
✅ No HAVING without GROUP BY
```

Flag any check that is missing but should exist.

---

### Stage 5 — Plan Tree (pre-optimizer) (`src/planner/planner.cc`)

Show the plan tree as produced by the planner, before any optimizer pass.

```
ProjectNode [team, AVG(speed)]
  HashAggregateNode [group_by=team, agg=AVG(speed)]
    FilterNode [season = 2025]
      SeqScanNode [laps]
```

State the output schema at each node.

---

### Stage 6 — Optimizer Pass (Phase 4+) (`src/planner/`)

Describe each rewrite the optimizer applies, and show the resulting plan tree if it differs from Stage 5.

If `--no-optimize` would produce a different tree, note it explicitly.

Common rewrites:
- Predicate pushed below aggregate → `FilterNode` moves closer to `SeqScanNode`
- Most selective predicate moved to the inner loop
- Join side swapped based on row counts

If no optimizer changes apply, state: "Plan unchanged by optimizer."

---

### Stage 7 — Execution Path Selection

State which execution path is active:
- `--storage row --execution volcano`: row-based volcano operators
- `--storage columnar --execution volcano`: columnar-backed volcano operators
- `--storage columnar --execution vectorized`: vectorized `DataChunk` operators

Note any operators that behave differently across paths.

---

### Stage 8 — Execution Trace (Volcano mode)

Walk the `open()` / `next()` / `close()` call sequence top-down.

```
Project.open() → Filter.open() → SeqScan.open()
Project.next() → Filter.next() → SeqScan.next() → Row{team=Ferrari, speed=312, season=2025}
                               → Filter evaluates season=2025: PASS → returns row to Project
               → Project evaluates [team, AVG(...)]: ...
...
SeqScan.next() → nullptr (exhausted)
Filter.next() → nullptr
Project.next() → nullptr
Project.close() → Filter.close() → SeqScan.close()
```

For `HashAggregateNode`, mark the pipeline break explicitly: "All rows consumed here before any output."

---

### Stage 9 — Output

Show the final output rows and the output schema.

```
Schema: [team STRING, AVG(speed) DOUBLE]
Rows:
  Ferrari    312.45
  McLaren    308.91
  Mercedes   310.17
```

---

## Summary

End with: one sentence describing the most interesting or surprising transformation in this query's pipeline, and one sentence identifying the stage most likely to contain a bug if the output is wrong.
