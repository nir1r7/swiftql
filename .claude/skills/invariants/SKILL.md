---
name: invariants
description: SwiftQL engine invariants that must hold across planner, execution, and storage changes — join schema ordering, relation_slot resolution, late materialization, pipeline breakers, optimizer result-preservation. Load BEFORE modifying src/planner/, src/execution/, or src/storage/, or when a change might violate a structural guarantee.
---

# SwiftQL Engine Invariants

Violating any of these produces bugs that surface far from their cause. Check your change against each relevant invariant before writing code.

## Planning & schema

1. **Fixed join schema order.** `HashJoinNode` and `VecHashJoinNode` ALWAYS emit `[FROM columns] ++ [JOIN columns]`, regardless of build/probe swap. The swap is internal via the `swapped_` flag (`src/planner/plan_nodes.cc`, `src/execution/vec_hash_join_node.{h,cc}`). Never let output column order follow the physical build side.
2. **relation_slot is the relation identity, not table name.** Qualified columns and self-joins resolve via `relation_slot` (0=FROM, 1=JOIN) on `ColumnRef`/`ColumnDef`/`AggregateSpec`; table names cannot disambiguate self-joins. `Schema::indexOf(name, slot)` is slot-first with bare-name fallback. Self-joins REQUIRE aliases.
3. **Duplicate column NAMES in merged/output schemas are legal** (two `team` columns). Resolution is slot-based. Never add name-uniqueness assertions to `Schema`.
4. **GROUP BY qualifiers are unsupported** — parser strips `table.` prefixes in `parseColumnList`; group-by resolves by bare name (FROM side wins). If adding qualified GROUP BY: update parser + validator + `CardinalityEstimator` NDV lookup together.
5. **Planner performs no I/O** — it receives pre-loaded rows/tables.

## Execution

6. **Volcano is the correctness baseline.** It is never optimized and must support the full SQL surface. When volcano and vectorized disagree, suspect vectorized first — but verify against SQLite before trusting either.
7. **Vectorized requires columnar.** `--storage row --execution vectorized` is rejected. The optimizer runs ONLY on the columnar+vectorized path.
8. **Late materialization.** `VecFilterNode` emits a `SelectionVector` only — no data copies. Full materialization happens once, at `VecProjectNode`. Don't materialize mid-pipeline.
9. **Pipeline breakers** (consume ALL input before emitting): `HashAggregate`, `Distinct`, `Sort`, and the hash-join build phase. `Limit` enables early scan termination — don't break that.
10. **Row-count monotonicity:** Filter/Having/Distinct/Limit never increase row count; Project/Sort preserve it; Scan/Aggregate/Join set it.

## Optimizer

11. **Result-preservation:** optimized ≡ `--no-optimize` ≡ SQLite, always. Enforced by `python_tools/test_new_queries.py`'s invariant suite.
12. **Chunk pruner only prunes FROM-side predicates** (`relation_slot < 1`) — `chunk_pruner.h`.
13. **Cost-based decisions (build side, join algorithm) are result-invariant internals** — tests assert output equality, never internal choices, except via `--explain` shape checks.

## Storage & environment

14. **CSV cannot express NULLs** — empty numeric fields throw in `csv_loader.cc parseField`. Null-handling paths are only testable via in-memory operator-level unit tests.
15. **Result cache is keyed on the raw SQL string** — use `--no-cache` in every debugging/measurement run.
16. **Unit tests must run from `build/`** — they resolve `../catalog.json` relative to CWD.
17. **RLE applies only when `n_runs < n_rows / 4`**; otherwise columns stay raw. Dictionary encoding maps strings→`int32_t` IDs; hot loops compare IDs, not strings.

## Forward-looking (Phase 5)

18. `relation_slot` as 0/1 must generalize to N slots for multi-way joins — any new code branching on `slot == 0 / slot == 1` hard-codes the two-relation assumption; prefer slot-indexed structures.
