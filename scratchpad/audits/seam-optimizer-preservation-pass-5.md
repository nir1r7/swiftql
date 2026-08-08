# Seam audit — optimizer result preservation (pass 5, FINAL)

HEAD `b14d086`, branch `claude/phase5-week26-qomtkb`. Binary `build/swiftql`;
`find src -type f -newer build/swiftql` is EMPTY, so it is this HEAD's build. No
source file touched. Unless stated otherwise every measurement is a single CLI
invocation with `--no-cache --execution vectorized --storage columnar`, run on
both legs by `scratchpad`'s two-line dual-leg wrapper.

Predecessors: `seam-optimizer-preservation-pass-1.md` … `-4.md`.

Status: IN PROGRESS (written incrementally; summary at the end).

---

## 0. Round-4's own repros, re-run first

Pass 4's four P4-1 divergences, verbatim, on this HEAD:

| # | query | OPT | `--no-optimize` |
|---|---|---|---|
| 1 | `WHERE team = 5 AND speed > 999999` | Error: Type mismatch | Error: Type mismatch |
| 2 | `WHERE team > 'zzzzz' AND team = 5` | 0 rows | 0 rows |
| 3 | `WHERE 5 = team AND speed = 333.3333` | *(see §1)* | |
| 4 | `WHERE team LIKE 'zzz%' AND 5 = team` | 0 rows | 0 rows |

**All four agree.** Note the direction of the fix on #1: the optimized leg used
to answer 0 rows and now ERRORS — i.e. `ChunkPruner::canSkipChunk`'s new
STRING-boundary decline plus the freeze made the loud leg the common one. That
is the right direction (the definition in `expr_totality.h` says `team = 5` is
evaluated on every row, so it must raise), and it is a user-visible behaviour
change on the optimized leg that no harness query covers.

