# Week 36 — Query Coverage + Correctness

**README bullets:** port queries to the documented SwiftQL dialect; close
query-specific parser, execution and optimizer correctness gaps; document
supported scale and memory limits.
**Checkpoint:** supported TPC-H queries match reference results within numeric
tolerance — where "reference results" is **SQLite over the same `.tbl` files**,
never the published TPC-H answer set. `data/tpch/sf0.01/PROVENANCE.txt` states
why: `dbgen` was unavailable, the generator reproduces the spec's value
*domains* but not its *distributions*, so the published answers do not apply.
Every sentence this week writes says **"matches SQLite"**, never "correct" and
never "TPC-H compliant".

**The measured starting point,** from a full 22×4 run recorded in
`docs/tpch-baseline.json` and `docs/tpch-sf0.01-report.json`:

```
GATE tpch: PASS (19/22 meaningful vs SQLite: 5 in all four modes,
                 14 vectorized-only; 1 vacuous; 2 unported)
```

Week 36 is the first week whose goal is a **number that already exists**. The
fifth gate step re-measures it on every `verify` run and exits non-zero on any
regression, so nothing below can quietly cost coverage.

---

## Tasks

1. **Lift the constant-wrapper restriction so TPC-H's own Q17 text runs** — the
   one capability change that raises the headline figure, verified before
   planned on.
2. **The refusal sweep the lift obliges** — every comment, precondition,
   assertion, header, pinned needle and published count citing
   `found[0] == select_list[0]`.
3. **Q21: establish what it actually needs, and decide it in the open** — the
   correlated *inequality*, and the residual semi/anti join it implies.
4. **Mode coverage: settle Volcano semi/anti parity on the measured 34-cell
   breakdown** — say which of the two figures this week targets, with evidence.
5. **The dialect and divergence items three earlier weeks handed here** — the
   `SUBSTRING(d,1,4)` STRING year, the `ON` STRING-vs-numeric half-match, NaN
   groups, DOUBLE display, `SUM` in `double`.
6. **The small items owed** — `compare_against_sqlite.py`'s NaN/inf comparison,
   `random_diff.py`'s projection, `--time`, `--fingerprint-all`, per-query hand
   verification beyond q2/q18/q19.
7. **Document supported scale and memory limits** — the README bullet nobody has
   started, measured rather than asserted.
8. **Re-baseline, the correctness report, and the five-step gate** — the figure
   with its mode split, in the same commit as the capability that moved it.

**Target: 20/22 meaningful committed (Q17), 21/22 if Task 3 lands. All of it is
headline count, none of it is mode coverage — Task 4 shows why, with the
measurement that contradicts the assumption.**

---

*(Sections below are appended as each task is worked. This file is written
incrementally on purpose.)*
