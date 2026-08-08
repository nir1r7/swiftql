# Seam audit: subquery chain (weeks 31 -> 35), pass 4

Scope: W31 (materialize-then-substitute) / W32 (IN -> semi/anti) / W33 (EXISTS
decorrelation) / W34 (correlated scalar + derived RTEs) / W35 (subqueries inside
derived bodies).

Tree: `claude/phase5-week26-qomtkb` @ `b2bc70e` (code identical to the gated
`9da0494`). Pass 3 came back 0/0/0/4-LOW; this pass re-checks fix round 3's three
incursions into this seam and hunts what four passes have not reached.

All probes: `./build/swiftql --catalog catalog.json --no-cache`, four modes
(`col-vec`, `col-vec --no-optimize`, `row storage/Volcano`,
`columnar storage/Volcano`), SQLite through the oracle's own `load_from_catalog`
so the data is byte-identical to the harness's.

Status: **IN PROGRESS.**

---

## B-1 — BLOCKER. The type-through-division refusal does not cross the
## materialization cut, and a scalar subquery turns it into a silent wrong answer.

```
SELECT COUNT(*) AS n FROM laps l
WHERE 7 / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END) FROM laps l2) > 3

  col-vec              10000        <-- WRONG
  col-vec --no-optimize 10000       <-- WRONG, identically (both legs agree)
  row storage / Volcano     0
  columnar / Volcano        0
  SQLite                    0
```

And the same defect with no aggregate at all, through the PROJECT site:

```
SELECT COUNT(*) AS n FROM laps l
WHERE 7 / (SELECT CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END
           FROM laps l2 WHERE l2.lap_id = 2) > 3

  col-vec  /  col-vec --no-optimize   10000     <-- WRONG
  both Volcano modes  /  SQLite           0
```

**Control, one character different** — `THEN 2.0` instead of `THEN 2`, so no INT
branch and no type to lose: all four modes and SQLite answer **10000**. So the
divergence is exactly the stored-type story the round-3 fix names, not a
coincidence of the data.

**The identical arithmetic written inside ONE plan is REFUSED, today, by the fix
that was supposed to own this:**

```
SELECT COUNT(*) AS n FROM (SELECT CASE WHEN lap_id = 2 THEN 2 ELSE 0.5 END AS x
                           FROM laps WHERE lap_id = 2) t
WHERE 7 / t.x > 3
  -> Error: vectorized execution cannot materialize the integer 2 into a DOUBLE
     result column that another expression divides. ...
```

Same CASE, same `7 / x > 3`, same engine. Written as a derived table it refuses;
written as a scalar subquery it returns 10000 where SQLite returns 0.

### Why

`collectIntOrigins` is called **once per `VectorizedPlanBuilder::build`**
(`vectorized_plan_builder.cc:1093-1094`), and `materializeSubqueries` cuts the
query into **two independent builds**:

1. `runOnce` moves the body out and calls the runner
   (`subquery_materialization.cc:206`), which is
   `runVectorizedToRows` (`main.cc:129-143`) — its own
   `LogicalPlanBuilder::build` + its own `VectorizedPlanBuilder::build` + its own
   `collectIntOrigins`. Inside that plan there is **no `/`**, so no origin is
   armed, so `appendColumnValue` narrows the INT into the DOUBLE column silently
   — which is correct behaviour *for that plan*, and is exactly what
   `TYPEFIX_DIV_GUARDS_ALL_MODES`' third entry pins ("the aggregate PRODUCED but
   not DIVIDED").
2. The value crosses the cut as a `Value` inside a `Literal`
   (`buildReplacement`, `subquery_materialization.cc:241`). It has **already
   been flattened to DOUBLE** by step 1. The outer plan's `collectIntOrigins`
   sees a `Literal` whose `value.type()` is DOUBLE and correctly concludes there
   is nothing to arm — `taintWalk` bottoms out at "Literal, and anything with no
   column underneath it" (`vectorized_plan_builder.cc:222`).

Neither walk is wrong on its own. The taint has to travel from a node in plan A
to a `/` in plan B, and no walk spans both — the `IntObservableMap` is keyed by
`const LogicalPlanNode*` of a tree that has already been destroyed by the time
the outer tree exists.

`foldConstants` then folds `7 / 2.0` to `3.5` on the vectorized leg and `7 / 2`
to `3` on Volcano, so the divergence is baked into the outer plan before either
engine runs — which is why **both optimizer legs agree**, and why the harness's
`optimized == --no-optimize` invariant reports this as clean.

### Why no suite sees it

* The diffed suites cannot hold it: it is a wrong ANSWER, so it would have to be
  a `run_query_suite` entry, and no entry in `compare_against_sqlite.py` puts a
  mixed-type `CASE` inside a subquery body. The whole `TYPEFIX_DIV_*` family
  (11 queries) is single-plan: three derived-table levels, a FILTER, a HAVING, a
  GROUP BY — and not one subquery. `FROM (SELECT` is a derived table, which is
  the same plan; `(SELECT` in a predicate is a different plan, and that is the
  boundary the family never crosses.
* The two-leg optimizer invariant cannot see it: both legs are wrong identically
  (pass 2 already recorded that this seam gets nothing from that check — all
  three lowerings run in both legs).
* The four-mode census cannot see it either, because Volcano is *right* here;
  the modes are compared against SQLite one at a time, never against each other.

### Scope, measured rather than assumed

The hole is the **cut**, so it is as wide as the set of shapes that cross it.
Confirmed reachable through both materializing sites (PROJECT and AGGREGATE) and
in both polarities of the `/` (`7 / sub` and `sub / 2`) — see the execution block
below. It does NOT reach the decorrelated shapes: a correlated EXISTS, a
correlated scalar and a subquery-inside-a-derived-body are all lowered into the
SAME logical plan as their consumer, so one `collectIntOrigins` spans them and
the refusal fires correctly (verified, below).

### Minimum fix

Two options, both small; neither needs the taint walk to span plans:

* **At the cut** (preferred, and it is where the round-3 LIMIT fix was also put):
  `buildReplacement`'s SCALAR arm holds `res.rows[0][0]` and
  `res.schema.column(0).type`. When the schema says DOUBLE and the Value says
  INT, the body flattened a type. Refuse there with the existing message, or —
  better and answer-preserving — **keep the Value's own type**, which is what
  Volcano does and what makes the two engines agree. The Value is already
  correct; nothing needs to be re-run.
* At the runner: have `runVectorizedToRows` return the pre-narrowing `Value`.
  Larger, and it fights `appendColumnValue`'s reason for existing.

The first is one branch in a function this seam owns. Note that "refuse" here
costs a query the vectorized path answers correctly whenever the flattening is
not observable — so preserving the type is the better of the two, and it makes
the materialized scalar agree with Volcano by construction rather than by a
second rule.

---

(rest of the audit below; this file is written incrementally)
