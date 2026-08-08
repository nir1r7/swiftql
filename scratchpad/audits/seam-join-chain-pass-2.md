# Seam audit: the join chain across weeks 26–36 — PASS 2

Scope: the joints between the join layers (single hash join -> multi-way DP -> semi/anti
-> derived tables as join inputs -> decorrelated subqueries lowered to joins).
Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `ee9c9d7`.
Predecessor: `scratchpad/audits/seam-join-chain-pass-1.md`.

STATUS: in progress — written incrementally.

---

## Part A — is pass 1's fix (`17bfcea`, DERIVED case) real and complete?

### A.1 The estimate is genuinely derived from the subplan, not a constant — CLEAN

`src/planner/cardinality_estimator.cc:330-357`:

    case LogicalNodeType::DERIVED: {
        estimateNode(*node.children[0], catalog);
        node.estimated_rows = node.children[0]->estimated_rows;
        return StatsContext{};
    }

It recurses into the body and adopts the body's root count. Not a constant, not a
heuristic multiplier. It is exactly right for the relational fact it cites: a derived
relation is row-preserving over its body. The recursion also stamps every node *inside*
the body, which is the half that `--explain` shows.

The context is deliberately dropped (returns empty `StatsContext{}`), with a
reasoned justification in the comment: the body's top-node context is named by the body's
*inputs* (PROJECT returns its child's context unchanged), while the relation's columns are
the select list after aliasing, and no mapping is computable at that point. That is the
conservative direction — "no statistic" — and `joinCardinality` already models it.

### A.2 Every node kind now has a case — CLEAN

`LogicalNodeType` (`src/planner/logical_plan.h:12-14`) has exactly nine members:
`SCAN, DERIVED, JOIN, FILTER, AGGREGATE, PROJECT, SORT, DISTINCT, LIMIT`.
`estimateNode`'s switch (`src/planner/cardinality_estimator.cc:308-587`) now has a case
for each of the nine (SCAN:310, DERIVED:330, FILTER:359, JOIN:373, AGGREGATE:528,
PROJECT/SORT/DISTINCT:557-559, LIMIT:567). The trailing `return StatsContext{}` at :587 —
the line that swallowed DERIVED for fourteen weeks — is now unreachable.

I checked the same shape in every other switch/dispatch over `LogicalNodeType`:

- `src/planner/vectorized_plan_builder.cc:285-681` — a nine-way switch, all nine present
  (SCAN:285, DERIVED:302, JOIN:314, FILTER:636, AGGREGATE:651, PROJECT:659, SORT:666,
  DISTINCT:672, LIMIT:677).
- `src/planner/join_enumeration.cc` and `src/planner/predicate_pushdown.cc` use `if`
  chains rather than switches, and each handles DERIVED explicitly where it matters
  (`join_enumeration.cc:42, 67`; `vectorized_plan_builder.cc:55, 74, 575`).

No second silently-defaulting case of the same class.

### A.3 Nothing downstream depended on the zero — CLEAN

The only consumers of `estimated_rows` outside the estimator are:

- `src/planner/join_enumeration.cc:533` `rels[r].rows = std::max(leaves[r]->estimated_rows, 0.0)`
  — the clamp that turned -1 into the phantom 0. It is now a no-op for DERIVED (real
  count), and is still needed for the `--no-optimize`-adjacent case where a node was
  never stamped. Nothing reads "== 0" as a signal.
- `src/planner/vectorized_plan_builder.cc:279, 318-371` — `estimate_driven = from_est >= 0
  && join_est >= 0`; a *negative* estimate was the disabling signal, never a zero. The fix
  restores the positive branch rather than perturbing a threshold.
- `src/cli/main.cc:302, 328` — `estimated_rows >= 0.0` gates printing `est=`. Same
  negative-vs-nonnegative test.

There is no "cheap enough not to bother" shortcut keyed on zero anywhere in the tree.

### A.4 The order did NOT change; only the justification did — pass 1 slightly overstated

Re-ran pass 1's cited query on `data/tpch/sf0.01` (columnar + vectorized, `--explain`):

    LogicalJoin [c_nationkey = n_nationkey] order=customer@1,@2,nation@0 cost=6543 (written=7409) method=dp

The order is **byte-identical to the pre-fix order pass 1 reported**
(`order=customer@1,@2,nation@0`). What changed is the numbers: `cost=1525 (written=4216)`
— a claimed 2.76x win — became `cost=6543 (written=7409)`, a 1.13x win. So the fabricated
0 did not steer the DP to a *different* order on this shape; it manufactured the *margin*.
Pass 1's "the search hoists the derived relation ahead of nation and claims a 2.8x
improvement" is right about the claim and wrong to imply the hoist was caused by the
phantom — the hoist survives correct costing, and is in fact the better order (the chosen
order's intermediate is 1000 rows, the written order's is 1500).

The two physical decisions pass 1 said had silently switched off are back, visibly:

    VecSimdLoopJoin [c_nationkey = n_nationkey] build=nation cost=525 (alt=1532) algo=simd (hash=1050)
    VecHashJoin     [c_custkey = k]             build=derived cost=3516 (alt=4024) algo=hash (simd=31016)

and every node of the derived body is stamped (`est=1000` on DERIVED/PROJECT/AGGREGATE,
`est=15000` on the orders scan).

**A.4 verdict: the fix is real, complete, and correctly derived. Part A finds no defect.**

---

## Part B — hunting fresh

### B-1 (LOW, comment/doc drift in the wake of a removed refusal) — `hasSlotOutsideRangeTable` was renamed and its silence was removed in `18af84f`; four live source citations and one `development.md` row still describe the old function and the old behaviour

`18af84f` renamed `hasSlotOutsideRangeTable` -> `slotDeclineReason`
(`src/planner/join_enumeration.cc:151`) and, deliberately, **made the semi/anti
decline REPORTED** where it had been silent. The standing rule this codebase has
learned twice says every comment, precondition, assertion and header that cites a
changed guard must be swept. It was not:

Live source, still naming a function that no longer exists:

- `src/planner/join_enumeration.h:66` — "...range table (hasSlotOutsideRangeTable, Week 30)"
- `src/planner/join_enumeration.cc:540` — "DEAD since Week 30: hasSlotOutsideRangeTable declines the whole tree"
- `src/planner/cardinality_estimator.cc:464` — "JoinEnumeration also declines these trees — hasSlotOutsideRangeTable fires on join_slot == -1"
- `src/planner/cardinality_estimator.cc:513` — "(hasSlotOutsideRangeTable declines the tree on join_slot -1)"

Docs, still asserting the behaviour `18af84f` argued against, in the very table
whose stated purpose is to be the audit trail:

- `development.md:808` — "**Level-agnostic, and now LIVE.** Fires on `join_slot == -1` ...
  **The decline is silent, in the same shape as the <3-relation one — there was no
  ordering decision to report**". That sentence is the exact argument `18af84f`'s
  commit message records as having "stopped being true once IN-lowering shipped".
  A reader consulting the table for the current contract is told the opposite of
  what the code does.
- `README.md:1597, 1804, 1815, 1890` — four more citations of the old name.

Not a wrong answer and not a plan change: the four source comments describe the
right *behaviour* under the wrong *name*, and `development.md:808`'s error is
confined to the reporting, not the decline. Ranked LOW for that reason, but it is
squarely the class the standing rule exists for, and `development.md`'s own header
says "a missing row is worse than a wrong one".

Concrete shape (the thing that makes the doc row false), verified below in B-1a.

#### B-1a — confirmed on the shipped f1 catalog

    SELECT l.lap_id FROM laps l
      JOIN drivers d  ON l.driver_id = d.driver_id
      JOIN drivers d2 ON l.driver_id = d2.driver_id
    WHERE l.driver_id IN (SELECT dr.driver_id FROM drivers dr WHERE dr.age > 25)

    LogicalSemiJoin [driver_id@0 = driver_id] join-ordering=skipped (semi/anti join)

The decline prints. `development.md:808` says it does not.

---

### B-2 (MEDIUM, plan quality; over-reach of a legality guard) — `containsOuterJoin` recurses into a DERIVED relation's BODY, so an outer join sealed inside a derived table switches off ordering for the enclosing fully-inner block

`src/planner/join_enumeration.cc:91-101`:

    bool containsOuterJoin(const LogicalPlanNode* node) {
        if (node->type == JOIN && join_type != INNER) return true;
        for (const auto& child : node->children)
            if (containsOuterJoin(child.get())) return true;
        return false;
    }

The recursion is over **all children with no node-type stop**, so it walks straight
through a `LogicalDerived` into the body. But a derived relation is a **leaf** to this
pass — `countRelations` (`:67`) returns 1 for it and `decompose` takes its whole subtree
as `leaves[slot]`. Nothing the search does can move, split, or reach inside it. The
outer join in the body is therefore not in the reorderable tree at all, and the Week 29
legality argument the decline rests on (an outer join is not commutative/associative with
the inner joins **around it**) simply does not apply.

Concrete shape, `data/f1/sf-small`, actually run:

    SELECT l.lap_id FROM laps l
      JOIN drivers d ON l.driver_id = d.driver_id
      JOIN (SELECT dr.driver_id AS k, dr.team AS t
            FROM drivers dr LEFT JOIN laps l2 ON dr.driver_id = l2.driver_id) x
        ON l.driver_id = x.k

    LogicalJoin [driver_id@0 = k] join-ordering=skipped (outer join)
      ...
      LogicalDerived [x, 2 columns]
        LogicalProject [k, t]
          LogicalLeftJoin [driver_id = driver_id]      <-- the only outer join, sealed

Three inner relations (`laps@0`, `drivers@1`, `x@2`) in a fully inner outer block, and
ordering is refused. Same over-reach applies to a semi/anti join's `children[1]` body:
`containsOuterJoin` runs *before* `slotDeclineReason`, so a `LEFT JOIN` inside an `IN`
body makes the decline report `(outer join)` when the real cause is `(semi/anti join)` —
a misattributed reason on the one surface `--explain` readers consult.

**Not a wrong answer.** Declining is always legal; this is strictly a plan-quality and
reporting defect. Ranked MEDIUM because it is reachable from ordinary SQL, silent about
its real cause, and the cost is measured in B-3.

---

### B-3 (MEDIUM, plan quality; a stated invariant that stopped being true three weeks running) — `JoinEnumeration::apply` never descends past the topmost JOIN, so no join tree beneath a join is EVER enumerated: derived bodies, subquery bodies, and the fully-inner spine under a declined semi/anti join

`src/planner/join_enumeration.cc:613-620`:

    // The topmost JOIN is the root of the whole join tree — there is exactly one
    // per statement until subqueries arrive in Week 30 — so this replaces at most
    // once and never descends into a tree it has already reordered.
    if (node->type == LogicalNodeType::JOIN) return reorder(std::move(node), catalog);
    for (auto& child : node->children) child = apply(std::move(child), catalog);

The premise in the comment — "there is exactly one per statement" — has been false since
Week 32 (a semi-join body is a second join tree) and doubly false since Week 34 (a derived
body is a third). The `return` is unconditional: it fires whether `reorder` accepted or
declined, and it never reaches the children. Three losses fall out of one line:

1. a derived relation's body is never enumerated once the derived relation is itself a
   join input;
2. an `IN`/`EXISTS` body's join tree is never enumerated;
3. the fully-inner spine below a *declined* semi/anti or outer join is never enumerated
   (pass 1 noted this third one as "the documented Week 29 cost"; the first two are new).

Measured, `data/tpch/sf0.01`, by the engine's own cost model:

    -- control: the same body with nothing above it
    SELECT x.ok FROM (SELECT o.o_orderkey AS ok, n.n_name AS nn
                      FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey
                                    JOIN nation n   ON c.c_nationkey = n.n_nationkey) x

      LogicalJoin [c_custkey = o_custkey] order=customer@1,nation@2,orders@0
                  cost=38417 (written=62729) method=dp

    -- the same body as a join input
    SELECT x.ok FROM region r
      JOIN (SELECT o.o_orderkey AS ok, c.c_nationkey AS nk
            FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey
                          JOIN nation n   ON c.c_nationkey = n.n_nationkey) x
        ON r.r_regionkey = x.nk

      LogicalJoin [r_regionkey = nk]                 <-- no order= (n=2, below the guard)
        LogicalScan [region, 1 columns]
        LogicalDerived [x, 2 columns]
          LogicalJoin [c_nationkey = n_nationkey]    <-- no order= AT ALL
            LogicalJoin [o_custkey = c_custkey]      written order kept

**cost 62729 instead of 38417 — a 1.63x regression the engine's own model can see and
prints nowhere.** No `join-ordering=skipped` line either: the body's join simply never
meets the pass, so there is not even a decline to read. Silent, unlike every other
decline in this file, which Week 30's own rule ("a supported query pays a real
plan-quality cost" earns a reported decline) says should not be.

**Not a wrong answer.** The written order is always legal. MEDIUM: measurable, reachable
from ordinary SQL, invisible on the `--explain` surface, and resting on a comment whose
premise three separate weeks invalidated.

---

### Targets probed and found CLEAN

Everything below was read AND, where a shape could be constructed, executed
differentially: SwiftQL optimized vs SwiftQL `--no-optimize` vs SQLite loaded from the
same catalog, on `catalog.json` (f1, 10k laps / 20 drivers) and `data/tpch/sf0.01`.
Runner: `scratchpad/diff.py` (my own, deliberately independent of the big harnesses;
sorts both sides, normalises numerics). **28 seam shapes, 0 disagreements** —
the one "DIFF" is a documented refusal (a subquery in the SELECT list), not a divergence.

#### C-1 — the DP's search space is result-preserving

- **Cross products.** `enumerateDP` (`join_enumeration.cc:349`) only extends by a
  relation with `adj[r] & s`, and `rebuild` (`:242-245`) throws if a step yields no key.
  Both legs, so an unconnected step cannot be reached and cannot be built.
- **Crossing an outer join.** `containsOuterJoin` declines the whole tree before
  `decompose` moves anything (pass 1's T3; re-verified). `JoinType` still has only
  `INNER` and `LEFT`.
- **A predicate moved across a join it is not valid below.** `decompose` moves *whole
  subtrees* — a pushed conjunct is already inside `leaves[slot]` as a `LogicalFilter` over
  that relation and travels with it. Residual conjuncts stay in the `LogicalFilter` above
  the tree, which `JoinEnumeration::apply` recurses *through* without touching. Slots on
  the merged schema are preserved by `rebuild`'s per-relation restamp, so a residual's
  `indexOf(name, slot)` still resolves. Nothing rewrites a predicate.
- **Edge consumption.** Each `Edge` from `decompose` is picked up at exactly the step its
  later-in-order endpoint arrives (`rebuild:225-231`), so a reordered tree carries the same
  conjunct set as the written one. Verified by construction on the three shapes where it
  could go wrong: two keys between one pair, two keys from one join to two different
  earlier relations, and a triangle.
- **A self-edge would be dropped** (`e.slot_a == e.slot_b == r` satisfies neither arm of
  `rebuild`'s test, and a dropped key is MORE rows). It is **unreachable**:
  `classifyJoinCondition` (`join_condition.cc:100-107`) explicitly falls a same-relation
  equality (`ON a.id = a.grp`) through to the residual list rather than making a key, and
  the only other producer of `from_slot` is the unbound positional path, which
  `slotDeclineReason` declines. Recorded as a latent shape with no live input, not a
  finding.

#### C-2 — semi/anti joins cannot be moved, cannot reach the build side, and cannot be projected from

- **Moved:** `slotDeclineReason` (`:151-166`) fires on `join_slot < 1`, and both lowerings
  construct with `/*join_slot=*/-1` unconditionally (`subquery_lowering.cc:92`,
  `subquery_decorrelation.cc:632`). Now *reported* rather than silent — B-1.
- **Build side:** forced, not costed (`vectorized_plan_builder.cc:498-505`), and
  `VecHashJoinNode`'s constructor throws on `swapped_`, on `left_outer_`, on a non-null
  `on_residual_`, and on an output schema wider than the probe child's
  (`vec_hash_join_node.cc:24-40`). Four independent legs.
- **Projected from:** the node's `output_schema` **is** `children[0]`'s, so no body column
  is ever in scope above it; `CardinalityEstimator`'s context merge is guarded on
  `semantics == STANDARD` (`:472`); `collectSlotTables` likewise (`:201`);
  `PredicatePushdown::distribute` declines to push into `children[1]` (`:302`).

#### C-3 — NULL join keys are honoured on both sides of all three operators

`isUnmatchableKey` (NULL or NaN) is tested **before** encoding, on **both** the build and
the probe path, in all three join operators: `VecHashJoinNode` (`:55, 99, 265`),
`VecSimdLoopJoinNode` (`:47, 158` — `isNull` on the ColumnVector, so the placeholder under
a NULL never enters the flat key buffer), and Volcano's `HashJoinNode`
(`plan_nodes.cc:636-644`). The three-way NULL split for `SEMI` / `ANTI` / `ANTI_NOT_IN`
at `vec_hash_join_node.cc:249-278` is correct on every case I could enumerate:

| shape | SQL | code |
|---|---|---|
| `EXISTS`, outer key NULL | body's equality is UNKNOWN -> no rows -> FALSE | emits nothing |
| `NOT EXISTS`, outer key NULL | ... -> NOT EXISTS TRUE | emits **unconditionally** (`semantics_ == ANTI`) |
| `NOT EXISTS`, body key NULL | that body row just never matches | no short-circuit (guarded `ANTI_NOT_IN` only) |
| `NOT IN`, body holds a NULL | never TRUE | whole probe short-circuited (`:228`) |
| `NOT IN`, outer key NULL, S non-empty | UNKNOWN -> dropped by WHERE | not emitted |
| `NOT IN`, outer key NULL, S empty | `NULL NOT IN ()` is TRUE | emitted (`build_keys_.empty() && !had_unmatchable`) |
| `NOT IN`, S emptied only by NULLs | UNKNOWN | short-circuit fires first — correct |

`ANTI` vs `ANTI_NOT_IN` is chosen at the two lowering sites and nowhere else
(`subquery_decorrelation.cc:639` and `subquery_lowering.cc:100`), and each carries the
rule in a comment naming the wrong answer it once shipped. Confirmed live on 5 NULL-bearing
shapes (NULLs manufactured by a `LEFT JOIN` inside a derived body — the CSV loader cannot
express a NULL, so that is the only way to get one onto a join key).

#### C-4 — composite key encoding across the derived boundary

`appendJoinKeyField`'s `length_prefixed` flag is derived as `key_idx.size() > 1`
**separately on each side**, at all three serializers (`vec_hash_join_node.cc:54`,
`plan_nodes.cc:637`). The two sides cannot disagree: both index vectors come from the same
`join->keys` list, and `leftKeyIndices`/`rightKeyIndices` push one index per key. The
`'\x01'` collision class is closed by the length prefix at arity >= 2 and is vacuous at
arity 1. Ran composite-key joins (`ON l.driver_id = x.k AND l.team = x.t`) with a derived
table on each side and with a join-topped derived body — all agree with SQLite.

#### C-5 — the predicate/residual split

- An **INNER** join's non-key `ON` conjuncts are lifted into the WHERE conjunction
  (`logical_plan.cc:940-946`), which is `R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S)` — sound, and it is
  what buys them pushdown. A **LEFT** join's stay on the node as `on_residual`, where the
  identity is false. That is the only branch, and it is written `== LEFT` on the residual
  side and `== INNER` on the pushdown side, so a future RIGHT/FULL is refused by default
  in both.
- `pushIntoJoin`'s leftover loop (`predicate_pushdown.cc:352-354`) re-lifts anything
  `distribute` declined, so nothing is dropped — verified by reading; the only two
  decliners are the LEFT and the SEMI/ANTI arms.
- The Week 34 correlated-scalar rewrite builds a **LEFT** join, so its own predicate (which
  references only the `$scalarN` slot after substitution) is correctly *not* pushed into
  the null-supplying side — `distribute`'s `join_type == INNER` test declines it and the
  leftover loop lifts it above the tree. Ran the `COUNT` zero-row shape
  (`WHERE d.age > (SELECT count(*) ... WHERE l.speed > 999)`, 20 rows) and the `AVG` shape
  in a three-relation spine; both match SQLite.

#### C-6 — derived-table naming and resolution across the boundary

- `derivedRelationSchema` refuses two output columns of one name — verified live:
  `SELECT * FROM (SELECT d.driver_id, l.driver_id FROM drivers d JOIN laps l ON …) x`
  gives *"derived table 'x': column 'driver_id' is produced twice; give one of them an
  alias"*. `development.md`'s Week 34 row is accurate here.
- A derived column sharing a name with an outer relation's column stays resolvable:
  `indexOf(name, slot)` is an exact pair match and the derived relation is normalized to
  slot 0 in its own schema, then stamped with the outer slot by the merged schema. Ran
  `SELECT x.team, d.team FROM (SELECT … AS team FROM laps) x JOIN drivers d …` — correct.
- `rightKeyIndices`' bare-name lookup on a STANDARD join's build side is still safe with
  derived relations: `children[1]` of a STANDARD join is always a single relation (a leaf
  after `decompose`/`rebuild`, a `buildRelation` result before), a derived relation's own
  schema is slot-0 and duplicate-free by the refusal above, and `$scalarN`'s group keys
  were renamed `$k0…$k{n-1}` in `8a23b9d` (`$` is not lexable, so no body column can
  shadow one).
- An outer reference INTO a derived body is refused by name, not silently mis-resolved:
  *"derived table 'y': a subquery in FROM cannot reference a column of an enclosing query
  (LATERAL is not supported)"* — verified at top level and inside an `EXISTS` body.
- Nested derived tables (`FROM (SELECT … FROM (SELECT …) y) z JOIN …`) and a derived
  relation as the LEFTMOST relation of a reordered spine both agree with SQLite. In the
  latter case the DP moved the derived relation OFF slot 0 (`order=drivers@1,laps@2,@0`),
  `leftmost_is_slot0` correctly withheld the top-level pruning hint, and each relation's
  own pushed filter still reached its own scan (`pruning=on`, `chunks_skipped=1/2`).

---

### B-4 (LOW, EXPLAIN surface; a case added without the wrapper the comment promises) — the DERIVED lowering calls `lowerNode` instead of `lower`, so the ROOT of every derived body's physical subtree is the one node in the plan with no `est=`

`src/planner/vectorized_plan_builder.cc:274-281`:

    // Week 23: every physical node inherits its logical counterpart's estimate so
    // EXPLAIN ANALYZE can print est= next to rows_out. The wrapper stamps once for
    // all eight cases (the JOIN case has two returns); -1 stays -1 under --no-optimize.
    std::unique_ptr<VecPlanNode> Lowering::lower(...) {
        std::unique_ptr<VecPlanNode> phys = lowerNode(node, pruning_where);
        phys->estimated_rows = node->estimated_rows;
        return phys;
    }

`lowerNode` has **nine** cases, not eight — Week 34 added DERIVED — and DERIVED is the
only case in the file that recurses via `lowerNode` rather than `lower`
(`:304`, the sole in-case caller; every other case calls `lower`). So the body's root
physical node never gets stamped and prints blank.

Visible in `17bfcea`'s own pasted evidence, unremarked:

    VecHashJoin [c_custkey = k] ... build=derived cost=3516 (alt=4024)   est=1000
      VecScan [customer, 2 columns]                                      est=1500
      VecDerived [d, 2 columns]                                          est=1000
        VecProject [k, s] (materialize)                                  <-- no est=
          VecHashAggregate [group_by=o.o_custkey, agg=COUNT(*)]          est=1000

Reproduced on `catalog.json` under `--explain-analyze`: `VecProject [driver_id, speed]`
inside the derived body shows `rows_in=10000 rows_out=10000` and no `est=`, while its own
child scan shows `est=10000`. So `17bfcea`'s claim that "`--explain-analyze` now shows
estimates tracking actuals on **every node** of that plan" is one node short, and it is a
node inside the very construct the commit was fixing.

**Impact is confined to EXPLAIN.** `VecPlanNode::estimated_rows` has exactly one writer
(`:279`) and one reader (`collectVecNodes`, `src/cli/main.cc:302`); no operator and no
cost decision consults it — the physical cost decisions read the LOGICAL estimates
(`join->children[i]->estimated_rows`) directly. Hence LOW, not higher. But it is exactly
the class the standing rule names: a case was added, the comment that states the
invariant ("the wrapper stamps once for all eight cases") was not swept, and the new case
is the one that broke it.

Fix is one token: `lowerNode(` -> `lower(` at `:304`, plus "eight" -> "nine" at `:275`.

---

### B-5 (MEDIUM, stale invariant in a header, in the wake of a fix that corrected only the `.cc`) — `join_enumeration.h` still carries verbatim the paragraph `18af84f` identified as "false in both halves" and replaced in `join_enumeration.cc`

`18af84f`'s commit message, item 3:

> join_enumeration.cc: the Week 34 paragraph claiming joinCardinality's max(l, r) branch
> runs for a derived relation, and that method=written-floor was therefore CLI-reachable,
> is false in both halves … Replaced with what is true.

It was replaced in `join_enumeration.cc:483-500`. The **header was not swept**, and
`src/planner/join_enumeration.h:84-91` still says, word for word, the thing that commit
deleted:

    // What IS new, and is a different consumer: a derived relation has no
    // TableStats, so joinCardinality's non-multiplicative max(l, r) branch runs on a
    // query the CLI can type. Optimal substructure does not hold for a subset
    // containing one; … Week 28 recorded that method=written-floor
    // had never executed — it is reachable now.

Both halves are still false, for the reason `18af84f` gives: `have_ndv` is set when
**either** side supplies an NDV (`cardinality_estimator.cc:259-264`), and on the shapes
the CLI can type the non-derived side always does, so the multiplicative branch runs.
I re-probed for `method=written-floor` on the shape most likely to produce it — a
three-relation spine where **every** relation is derived, so neither side of any subset
has an NDV and `max(l, r)` genuinely runs:

    SELECT count(*) FROM (SELECT l.driver_id AS k FROM laps l) a
      JOIN (SELECT d2.driver_id AS k2 FROM drivers d2) b ON a.k = b.k2
      JOIN (SELECT d3.driver_id AS k3 FROM drivers d3) c ON b.k2 = c.k3

    order=@1,@2,@0 cost=16108 (written=30080) method=dp

`method=written-floor` still does not fire. The header asserts a reachability nobody has
observed, and asserts it as the justification for a containment.

Two more stale claims in the same header block, from the same commit:

- `join_enumeration.h:66` — "It also declines, **silently**, any tree carrying a relation
  slot outside the range table (`hasSlotOutsideRangeTable`, Week 30)". The decline is no
  longer silent (B-1a shows it printing) and the function no longer exists under that name.
- `join_enumeration.h:53-56` — invariant 5's parenthetical "(unlike the two declines below,
  where there is none to make)" is now false for one of those two.

Ranked MEDIUM rather than LOW because this is the *header* — the surface a reader
consults for the pass's contract — and because it is the second time in one audit cycle
that this exact paragraph has had to be corrected. The standing rule this codebase
records ("three silent wrong answers shipped from that shape in Week 33, **two of them
living in header comments**") names this precise failure. No wrong answer today: the
`.cc` is right and the containment (the written-order bound) is real regardless of which
branch runs.

#### C-7 — the decisive `ANTI` vs `ANTI_NOT_IN` pair, run with a real NULL in the body's key column

The one thing this seam could get wrong as a silent wrong answer is `NOT EXISTS`
re-inheriting `NOT IN`'s three-valued rule (or the reverse). It has happened once here
already (`vec_hash_join_node.cc:223-234` records it). The CSV loader cannot express a
NULL, so the NULL has to be manufactured by a `LEFT JOIN` inside the body — which is
itself a Week 29 x Week 32/33 seam shape:

    -- body's key column is NULL for EVERY row
    …NOT EXISTS (SELECT 1 FROM drivers dr LEFT JOIN laps l
                 ON dr.driver_id = l.driver_id AND l.speed > 9999
                 WHERE l.driver_id = d.driver_id)          -> 20 rows  (SQLite: 20)

    …d.driver_id NOT IN (SELECT l.driver_id FROM drivers dr LEFT JOIN laps l
                         ON dr.driver_id = l.driver_id AND l.speed > 9999)
                                                            ->  0 rows  (SQLite:  0)

**The two answers differ, in the right direction, on the same NULL-bearing body.** The
positive forms (`EXISTS` -> 0, `IN` -> 0) agree too. The distinction is live, not just
asserted in a comment.

#### C-8 — latent shapes with no live input (recorded, not counted as findings)

- `JoinEnumeration::rebuild` (`:262-264`) constructs fresh `LogicalJoin`s copying neither
  `join_type`, `on_residual` nor `semantics`, and `vectorized_plan_builder.cc:626-629`'s
  `swapped=true` branch drops `on_residual`. Both rest on the same single guard
  (`containsOuterJoin` / the forced side), which pass 1 flagged as a coupled pair. Still
  coupled, still unreachable — `on_residual` has exactly one writer
  (`logical_plan.cc:965-970`, inside `if (jc.type == JoinType::LEFT)`), and a LEFT tree
  never reaches either site.
- A self-edge (`from_slot == join_slot`) would be silently dropped by `rebuild`; it is
  unreachable because `classifyJoinCondition` routes a same-relation equality to the
  residual list (C-1).

---

## Summary

| Severity | Count | IDs |
|---|---|---|
| BLOCKER | 0 | — |
| HIGH | 0 | — |
| MEDIUM | 3 | B-2, B-3, B-5 |
| LOW | 2 | B-1, B-4 |

**Part A: `17bfcea` is real, complete and correctly derived.** The DERIVED estimate is
taken from the subplan (not a constant), the switch now covers all nine `LogicalNodeType`
members with no silent default left anywhere in the file or in the other three
`LogicalNodeType` dispatchers, nothing downstream keyed on the old zero (every consumer
tests `>= 0`, not `!= 0`), and re-running pass 1's cited shape shows the same join order
with honest numbers (`cost=6543 (written=7409)` where it had claimed
`cost=1525 (written=4216)`) plus both physical decisions restored. The one correction to
pass 1: the fabricated 0 manufactured the *margin*, not the *order* — the DP picks the
same, and better, order with correct costing.

**Part B: no wrong answer found.** 28 constructed seam shapes across
`catalog.json` and `data/tpch/sf0.01` — multi-way reordering with derived inputs, derived
relations as the leftmost and as the last relation, composite keys across the derived
boundary, nested derived tables, semi/anti stacked on multi-way spines, correlated scalars
(`AVG` and the `COUNT` zero-row rule) over three-relation spines, and NULL join keys
manufactured by a `LEFT JOIN` inside a derived body — all agree three ways (optimized,
`--no-optimize`, SQLite). The `ANTI` / `ANTI_NOT_IN` split is live and correct on a real
all-NULL body key. NULL keys are dropped before encoding on both sides of all three join
operators. The `'\x01'` composite-key class is closed by the length prefix and the
prefix flag cannot differ across a join's two sides.

The five findings are all **plan-quality or documentation** defects, three of them the
same shape: a guard or an invariant changed and its citations were not swept
(B-1 four source comments + a `development.md` row, B-4 an "eight cases" count that is now
nine, B-5 a header paragraph a fix corrected only in the `.cc`). The two behavioural ones
(B-2, B-3) both cost join ordering on supported queries — B-3 measurably, 62729 against
38417 by the engine's own model — and both are invisible on `--explain`.

**Verdict: the join chain's seam is correct; pass 2 returns no blockers. What it returns
instead is that the pass's own header and `development.md`'s slot table now describe a
join enumerator that no longer exists in three places, and that two legality guards
decline more than legality requires.**
