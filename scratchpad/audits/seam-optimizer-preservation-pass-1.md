# Seam audit — optimizer result preservation across Phase 5 (pass 1)

Scope: is `optimized ≡ --no-optimize ≡ SQLite` across everything weeks 26–36 added,
and are the estimates good enough on multi-join plans to be choosing sensibly?

Status: IN PROGRESS (opened)

## Targets
1. Result preservation over the Phase 5 query surface (CLI, ±`--no-optimize`).
2. Completeness of the declines (outer-join trees, `join_slot == -1`, pushdown's null-supplying side).
3. Estimate quality on multi-join plans (`--explain-analyze`, est vs actual).
4. Week 28 written-order fallback + `method=` honesty.
5. Passes reading a field a later week repurposed.

## Findings
(none yet)
