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
