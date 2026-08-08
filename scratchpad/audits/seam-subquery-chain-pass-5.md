# Seam audit: subquery chain (weeks 31 -> 35), pass 5 — FINAL

Scope: W31 (materialize-then-substitute) / W32 (IN -> semi/anti) / W33 (EXISTS
decorrelation) / W34 (correlated scalar + derived RTEs) / W35 (subqueries inside
derived bodies).

Tree: `claude/phase5-week26-qomtkb` @ `b14d086`, the gated tree (910 unit / oracle
1722·0·0 / regression 340·0·0 / TPC-H 20-of-22 / three pin matrices 7·17·5).
`build/` verified up to date under `flock` before any probe.

All probes: `./build/swiftql --catalog catalog.json --no-cache --format tsv`,
four modes (`columnar/vectorized`, the same `--no-optimize`, `row/volcano`,
`columnar/volcano`), SQLite through the oracle's own
`load_from_catalog(CATALOG_PATH)` so the data is byte-identical to the harness's.

Status: **IN PROGRESS** (written incrementally; this line is the last thing to
change).

---

## B-1 — BLOCKER. The arming request crosses the cut through every expression
## slot except one: `divWalk` does not walk a `SUM`/`AVG` argument, so
## `HAVING SUM(int_col / (SELECT <mixed CASE>))` is a silent wrong answer.

Fix round 4 closed pass 4's B-1 by sending the arming *request* inward. The
request is computed by `divWalk` (`subquery_materialization.cc:253-345`), whose
own docstring says it "Mirrors taintWalk in vectorized_plan_builder.cc, one AST
level up". It does not mirror it at one node: the `AggregateExpr` arm.

```cpp
// subquery_materialization.cc:268-275   (divWalk)
if (auto* agg = dynamic_cast<const AggregateExpr*>(e)) {
    if (agg->function_name == "COUNT") { out.may_be_int = true; return out; }
    if (agg->function_name == "SUM" || agg->function_name == "AVG") return out;
    return divWalk(agg->argument.get(), rt, observed, catalog, depth);
}
```

`SUM` and `AVG` return **without walking `agg->argument`**. The early return is
right about the aggregate's own type (both emit a DOUBLE, so `may_be_int` is
correctly false) and wrong about `observed`, which is a pure side channel: every
other non-arithmetic arm of the same walk — `IsNullExpr`, `InExpr`, `LikeExpr`,
`SubstringExpr` — recurses *for the side effect alone* and then discards the
result. `SUM`/`AVG` are the only arms that drop the subtree.

The plan-level walk it mirrors has no such hole:
`collectIntOrigins`' `AGGREGATE` case calls
`taintWalk(spec.argument, cs, child, armed)` for **every** aggregate
(`vectorized_plan_builder.cc:426`), outside the `order_stat` test that follows.
So the identical arithmetic is armed in one plan and unarmed across the cut.

### The failing shape

```
SELECT l.team FROM laps l GROUP BY l.team
HAVING SUM(l.round / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END)
                      FROM laps l2)) > 5900
ORDER BY l.team

  col-vec               7 rows   AlphaTauri Alpine Ferrari McLaren Mercedes RedBull Williams   <-- WRONG
  col-vec --no-optimize 7 rows   identically wrong (both legs agree)
  row storage / Volcano 4 rows   Alpine Ferrari McLaren RedBull
  columnar / Volcano    4 rows   Alpine Ferrari McLaren RedBull
  SQLite                4 rows   Alpine Ferrari McLaren RedBull
```

`5900` is discriminating by construction, not by luck. Per team, `SUM(round/2)`
(INTEGER division, what Volcano and SQLite do) against `SUM(round/2.0)` (REAL,
what the vectorized path does after the body's `2` is flattened to `2.0`):

| team | INT-division sum | REAL-division sum |
|---|---|---|
| AlphaTauri | 5671 | 5926.0 |
| Alpine | 8958 | 9327.0 |
| Ferrari | 15268 | 15917.0 |
| McLaren | 8966 | 9325.0 |
| Mercedes | 5814 | 6054.0 |
| RedBull | 9398 | 9786.5 |
| Williams | 5782 | 6016.0 |

Three teams straddle 5900, which is exactly the 7-against-4 measured.

### `AVG`, and the other polarity of the `/`

```
HAVING AVG(l.round / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END)
                      FROM laps l2)) > 6.1
  col-vec / col-vec --no-optimize  6 rows      <-- WRONG
  both Volcano modes / SQLite      1 row (RedBull)

HAVING SUM((SELECT MAX(CASE WHEN l2.round > 10 THEN 7 ELSE 0.5 END) FROM laps l2)
           / l.round) > 1000
  col-vec / col-vec --no-optimize  7 rows      <-- WRONG
  both Volcano modes / SQLite      1 row (Ferrari)
```

The subquery is the *divisor* in the first two and the *dividend* in the third:
the arming request never leaves the outer AST in either polarity, because it is
the enclosing `SUM`/`AVG` that swallows it, not anything about the `/`.

### The three controls that make it the aggregate arm and nothing else

| control | change | result |
|---|---|---|
| `MAX(l.round / (SELECT <same body>)) > 5` | `divWalk`'s MIN/MAX arm **does** recurse | **refuses**, in both vectorized modes, with the round-3 message |
| `SUM(l.round / 2)` — same division, INT literal instead of the subquery | no cut to cross | all four modes and SQLite agree, 4 rows |
| `COUNT(*) > 7 / (SELECT <same body>)` — same HAVING, subquery **outside** the aggregate | `divWalk` reaches it | **refuses**, in both vectorized modes |

The first and third are the sharp ones: the rule exists, fires from the same
clause, on the same body, and is defeated by wrapping the division in `SUM`.

### The sharpest witness — the same shape in ONE plan refuses

```
SELECT SUM(t.x / 2) AS s
FROM (SELECT CASE WHEN lap_id = 2 THEN 2 ELSE 0.5 END AS x FROM laps) t

  col-vec / col-vec --no-optimize
    -> Error: vectorized execution cannot materialize the integer 2 into a
       DOUBLE result column that another expression divides. ...
  (AVG in place of SUM: the same refusal)
```

An aggregate argument holding a `/` over a mixed-`CASE` value is refused when
the value is produced inside the same build (`collectIntOrigins` walks
`spec.argument`) and answered wrongly when it is produced across the
materialization cut (`divWalk` does not). One walk mirrors the other everywhere
except here.

### Why no suite sees it

Same three structural reasons pass 4's B-1 had, unchanged by fix round 4:

* it is a wrong ANSWER, so only a `run_query_suite` (diffed) entry could hold it,
  and the round-4 cut-family entries added at `bb67beb` put the subquery under a
  bare `/` in `WHERE`/`HAVING`, never under an aggregate;
* both optimizer legs are wrong identically, so the `--no-optimize` invariant
  reports it clean;
* the four-mode census compares each mode against SQLite, never against another
  mode, and Volcano is right here.

### Minimum fix

One line, and it is the line the file's own convention already writes four times:

```cpp
if (agg->function_name == "SUM" || agg->function_name == "AVG") {
    divWalk(agg->argument.get(), rt, observed, catalog, depth);   // side effect only
    return out;                                                    // ... type still not INT
}
```

`COUNT` deserves the same treatment for uniformity, though I could not construct
an observable failure through it (see "probed and found correct" below). Nothing
about `may_be_int` changes; only `observed` gains the subtree. The
counter-probes above are the regression entries: the `SUM` query as a **diffed
four-mode** entry (it returns rows in every mode, so only a diff discriminates)
and the `MAX` query as the rejection twin that proves the arm still fires.
