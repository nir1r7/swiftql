# Seam audit — storage (chunks, pruning, columnar->row reconstruction)

Scope: every Phase 5 change reaching `src/storage/`, chunk pruning, or columnar-to-row
reconstruction. Question: can any Phase 5 plan shape prune a chunk holding a needed row,
or reconstruct a row wrongly?

Status: OPEN — in progress.

## Targets
1. Pruning safety across all Phase 5 plan shapes (row vs columnar comparison).
2. `relation_slot < 1` test + slot-0 stamping of derived relations (week 34).
3. The three types deliberately not migrated to `ColumnId` (week 33).
4. Zone maps + encodings after week 35 loader changes.
5. `parseField` / `FileFormat` hardening.

## Findings
(none yet)

---

## S-0 (STRUCTURAL, not a bug) — the row/columnar oracle does not exist for any Phase 5 plan shape

`src/cli/main.cc` rejects `--execution vectorized` unless `--storage columnar`
("Error: --execution vectorized requires --storage columnar"), and
`src/planner/planner.cc:118` (derived), plus the sibling refusals for multi-way
joins, IN-subqueries and correlated subqueries, reject every Phase 5 shape on the
Volcano path.

Measured, all four mode cells, on `catalog.json`:

| shape | row/volcano | col/volcano | row/vec | col/vec |
|---|---|---|---|---|
| 2-way join, outer join | OK | OK | refused | OK |
| derived table | refused | refused | refused | OK |
| IN subquery (semi join) | refused | refused | refused | OK |
| EXISTS / NOT EXISTS (decorrelated) | refused | refused | refused | OK |
| 3-way join | refused | refused | refused | OK |

So for derived tables, semi/anti joins, decorrelated bodies and multi-way joins
there is exactly ONE executable cell. The brief's premise — "row/columnar
comparison is the only thing that catches a wrong prune" — is therefore FALSE for
precisely the plan shapes Phase 5 added: no differential oracle covers them.
Everything below had to be checked against hand-computed expected answers on a
purpose-built clustered dataset instead.

Dataset used (scratchpad, regenerable): `big(k,g,team,val)` 20000 rows with `k`
ASCENDING (so 3 chunks with disjoint zone maps 0-8191 / 8192-16383 / 16384-19999
— `data/laps.csv` is shuffled and its zone maps span the full range in every
chunk, so it cannot detect a wrong prune at all), `small(k,team,tag)`,
`alt(j,k,team)` with `alt.k` in 100000..100019 — deliberately DISJOINT from
`big.k` so a leaked hint prunes every chunk and the count collapses to 0.

## S-1 PASS — cross-relation shared column name cannot reach the wrong scan

Adversarial shape (`alt.k` disjoint from `big.k`, both named `k`):

    SELECT COUNT(*) FROM big b JOIN alt a ON b.g = a.j WHERE a.k > 100010

True answer 9000. A leaked hint would make `canSkipChunk(">", 100010, chunk)`
(`src/storage/chunk_pruner.h:83`, `val >= mx`) true for all three chunks -> 0.
Measured 9000 in all 8 cells: {INNER, LEFT} x {vectorized, volcano} x {optimize,
--no-optimize}. `--no-optimize` is the load-bearing leg: pushdown never runs, so
`Planner::plan` (`planner.cc:233`) and `VectorizedPlanBuilder`
(`vectorized_plan_builder.cc:441`) hand the WHOLE `WHERE` to the FROM-side scan
and `chunk_pruner.h:70`'s `localSlot(...) < 1` is the only thing standing
between `a.k`'s slot 1 and `big`'s zone maps. It holds.

## S-2 PASS — a derived relation's slot-0 stamping does not leak a hint into its body

`derivedRelationSchema` (`src/planner/logical_plan.cc:485`) does stamp every
column `relation_slot = 0`, exactly as week 34 claims. That makes a derived
relation indistinguishable from a leaf scan to `leftmost_is_slot0`
(`vectorized_plan_builder.cc:434`), so a hint IS routed at it. Three independent
guards stop it before it reaches a body scan:

1. `vectorized_plan_builder.cc:304` — the DERIVED lowering passes `nullptr`, not
   the incoming hint, to its body.
2. `vectorized_plan_builder.cc:634` — the FILTER lowering only treats its
   predicate as a hint when the child is `SCAN` or `JOIN`; a filter above a
   DERIVED node yields no hint.
3. The body is a different block, so its refs would be `query_level > 0` anyway.

Verified at runtime with a derived column that SHADOWS a base column name and
shifts its range out of the base zone maps
(`SELECT k + 100000 AS k, g FROM big`), which is the only input that can tell a
leak from a no-op:

    SELECT COUNT(*) FROM (SELECT k + 100000 AS k, g FROM big) t WHERE t.k > 118000

True answer 1999; a leak gives 0. Measured 1999 for: plain derived; derived with
`--no-optimize`; derived with column-alias renaming `t (k, g)`; derived as the
PRESERVED side of a LEFT JOIN. `--explain-analyze` confirms the body scan reports
`rows_in=0 rows_out=20000` with no `chunks_skipped`, i.e. the hint really was
withheld rather than coincidentally harmless.
