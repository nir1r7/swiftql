# Seam audit — storage (chunks, pruning, columnar->row reconstruction)

Scope: every Phase 5 change reaching `src/storage/`, chunk pruning, or columnar-to-row
reconstruction. Question: can any Phase 5 plan shape prune a chunk holding a needed row,
or reconstruct a row wrongly?

Status: COMPLETE. Verdict: no blocker. 0 high, 0 medium, 2 informational (S-0 oracle gap; duplicate-column-name note, out of scope).

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

## S-3 PASS — every Phase 5 plan shape, hand-computed oracle

Same disjoint-range construction as S-1 (`alt.k` 100000..100019 vs `big.k`
0..19999, three chunks). A leaked hint collapses the count to 0 in every case,
so a wrong answer is unmissable. All measured values equal the Python-computed
truth:

| shape | query | expected | measured |
|---|---|---|---|
| 3-way join | `big b JOIN alt a ON b.g=a.j JOIN small s ON b.k=s.k WHERE a.k>100010` | 18 | 18 (opt and `--no-optimize`) |
| semi join | `big b WHERE b.g IN (SELECT j FROM alt WHERE k>100010)` | 9000 | 9000 (opt and `--no-optimize`) |
| anti join | `... NOT IN ...` | 11000 | 11000 |
| decorrelated EXISTS | `WHERE EXISTS (SELECT 1 FROM alt a WHERE a.j=b.g AND a.k>100010)` | 9000 | 9000 (opt and `--no-optimize`) |
| decorrelated NOT EXISTS | as above | 11000 | 11000 |
| derived under LEFT (preserved) | `(SELECT k+100000 AS k, g FROM big) t LEFT JOIN alt a ON t.g=a.j WHERE t.k>118000` | 1999 | 1999 |
| derived on null-supplying side | `alt a LEFT JOIN (SELECT k+100000 AS k, g FROM big) t ON a.j=t.g WHERE a.k>100010` | 9000 | 9000 |
| materialized scalar subquery | `big b WHERE b.k > (SELECT MIN(k) FROM alt) - 100000` | 19999 | 19999 |
| enumeration reorder | `alt a JOIN big b ON a.j=b.g WHERE a.k>100010` | 9000 | 9000 |

The reorder case is the one worth reading the plan for. `--explain-analyze`
shows join enumeration promoting `big` to `children[0]`, so the leftmost leaf is
relation slot 1, and `vectorized_plan_builder.cc:434`'s `leftmost_is_slot0`
guard fires: `VecScan [big]` prints no `chunks_skipped` field at all (hint
withheld), while `a.k > 100010` still reaches `alt`'s own scan as a pushed
filter. That is the intended behaviour, observed rather than inferred.

## S-4 PASS — columnar-to-row reconstruction, all types, all encodings

`SELECT <every column> FROM <table>` in `--storage row` vs `--storage columnar`
(volcano, the only executor that runs both), sorted, compared line-by-line on
all eight TPC-H sf0.01 tables:

    region 5, nation 25, part 2000, supplier 100, partsupp 8000,
    customer 1500, orders 15000, lineitem 60144   -> ALL IDENTICAL

`lineitem` at 60144 rows is 8 chunks and exercises raw INT, RLE INT, raw DOUBLE
and dictionary STRING simultaneously. No column-index off-by-one, no chunk
boundary error, no encoding round-trip loss.

## S-5 PASS — zone maps and `canSkipChunk` boundaries

`canSkipChunk` (`src/storage/chunk_pruner.h:81-86`) matches the safe-skip table
exactly, including `!=` falling through to `return false`:
`=` -> `val<mn || val>mx`; `<` -> `val<=mn`; `>` -> `val>=mx`; `<=` -> `val<mn`;
`>=` -> `val>mx`.

Empirically, on a purpose-built 3-chunk clustered table, for every column and
every per-chunk min and max as the literal, all six operators, checked against
BOTH `--storage row` and an independent Python count:

    84 boundary tests, 0 divergences, 0 wrong counts

`csv_to_columnar.cc:99-113` builds chunk boundaries with the same
`start += CHUNK_SIZE` loop for every column, and computes min/max through
`table.getValue` AFTER encoding, so RLE/dictionary columns get typed values, not
codes. Chunk boundaries are therefore identical across columns by construction.

## S-6 PASS — `parseField` hardening rejects nothing it should accept; the two delimiter paths agree

`src/storage/csv_loader.cc:92-121`. The full-consumption check uses the `pos`
out-parameter of `stoll`/`stod`, both of which skip leading whitespace and
consume the whole numeric token, so every legal spelling still loads. Measured,
identical in `--storage row` and `--storage columnar`:

    INT:    "5" -> 5,  "+7" -> 7,  "-3" -> -3,  "0" -> 0
    DOUBLE: "0.05" -> 0.05, "1e3" -> 1000, "-0.5" -> -0.5, ".25" -> 0.25, "3." -> 3

The same rows written as a `trailing_delimiter`/`header:false`/`|` table produce
byte-identical output, so the comma path and the pipe path agree on every type.
`csv_loader.cc:40` gates the trailing-delimiter pop on BOTH the format flag and
the field actually being empty, so a genuine empty last column is not eaten; and
`csv_loader.cc:31` strips CRLF before splitting, which is what stops a `\r` from
being appended to the last field of every line.

`catalog.cc:44` refuses a multi-character delimiter rather than silently using
its first byte.

## S-7 PASS (with a stated boundary) — the three types not migrated to `ColumnId`

- `ColumnDef::relation_slot` — a schema slot. `derivedRelationSchema`
  (`logical_plan.cc:485`) flattens a derived relation's own schema to slot 0 and
  the merged join schema then stamps it `i+1`, exactly as a leaf scan is
  treated; verified behaviourally in S-2/S-3. No level exists to lose because a
  schema belongs to one block by construction — a derived table's BODY schema is
  consumed by `derivedRelationSchema` and never escapes into the outer block's
  numbering.
- `Schema::indexOf(name, slot)` (`src/common/schema.cc:32`) — the merged schema
  above a derived table is the one place a same-named column from two blocks
  could meet. It cannot: `derivedRelationSchema` (`logical_plan.cc:494-501`)
  THROWS if the derived relation produces the same name twice, and the outer
  merge stamps distinct slots per relation. So `(name, slot)` is unique in every
  merged schema Phase 5 can build. Confirmed at runtime with a derived column
  deliberately SHADOWING a base column name (`SELECT k + 100000 AS k`) joined
  against tables that also have `k` — resolution stayed correct in all cases
  above.
- `ColumnStatsEntry::relation_slot` — feeds `CardinalityEstimator` only
  (`cardinality_estimator.cc:34,54,176,497`), and those sites already guard with
  `col->id.isLocal()` before calling `localSlot(...)`. A wrong entry changes a
  cost estimate, i.e. build-side choice and join order — never a row. Not a
  correctness surface.

## Verdict for this seam

No blocker, no dropped row, no wrong reconstruction found. Every guard the weeks
claim is present, and each was exercised with an input that would have exposed
its absence rather than merely with an input it happens to survive.

Two things a future week should not inherit as settled:

1. **S-0** — the differential oracle the storage layer's safety argument leans on
   does not exist for any Phase 5 plan shape (vectorized requires columnar;
   Volcano refuses derived/multi-way/semi/correlated). Every result above rests
   on hand-computed expected answers, not on a mode comparison. If Phase 6 adds
   a pruning path, there is no automatic net under it.
2. **Not Phase 5, recorded only because this audit touched it**: `ColumnarTable`
   keys `columns` and `zone_maps` by column NAME (`columnar_table.h:31,33`), and
   `catalog.cc:22-27` does not reject a table whose catalog JSON declares the
   same column name twice. Row mode loads such a table positionally and columnar
   mode collapses the pair. Requires a malformed catalog, so it is user error,
   not a plan shape — and derived relations are explicitly protected against the
   same collapse at `logical_plan.cc:497`. Out of scope; not counted as a
   finding.

## Targets not reached

- Zone-map boundary sweep at TPC-H scale (all columns of `lineitem`/`orders`/
  `part`/`partsupp`, ~1800 query pairs) was started and did not finish inside the
  time budget; it reported no divergence for the portion it completed. The
  equivalent sweep on the purpose-built clustered table DID complete (84/84).
  Note that `data/laps.csv` and the TPC-H key columns are shuffled, so their zone
  maps span the full range in every chunk and cannot detect a wrong prune at all
  — which is why the clustered table was built.
- DOUBLE-typed zone-map boundaries were covered only via TPC-H reconstruction
  (S-4), not via the boundary sweep (the clustered table has no DOUBLE column).
- RLE/dictionary internal invariants (run-length sum, no adjacent equal runs,
  dense IDs) were read at `rle_column.h` / `dictionary_encoder.h` and found
  correct by inspection, but not unit-tested — the brief forbids building or
  running the C++ suite.
