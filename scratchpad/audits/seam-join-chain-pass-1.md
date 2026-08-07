# Seam audit: weeks 26 -> 27 -> 28 -> 29 (join chain)

Question: does DP join enumeration still hold once outer joins exist?

Status: in progress.

## Targets
1. Build/probe swap under N relations vs Week 29's fixed side.
2. `[FROM] ++ [JOIN]` schema order at depth; `Schema::indexOf(name, slot)` on merged schemas 3+ deep.
3. Completeness of enumeration's decline on outer joins.
4. Cardinality rule composition: `joinCardinality` / outer `max(rows,left_rows)` / semi-anti.
5. Later-week guards resting on earlier-week invariants that have since moved.

## Findings
