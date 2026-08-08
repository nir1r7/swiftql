# Seam audit — storage — pass 4

Branch `claude/phase5-week26-qomtkb`, HEAD `b2bc70e` (code identical to the gated
`9da0494`; `b2bc70e` is docs-only — verified with `git show --stat`).
Written incrementally; sections appear in the order they were established.

Prior passes: `seam-storage-pass-1.md` (S-0 headline), `-2.md` (refuted S-0; 0/0/0/2),
`-3.md` (0 BLOCKER / 0 HIGH / 1 MEDIUM S-9 / 3 LOW).

**Binaries.** All measurements below use a **Release** build (`-O3 -DNDEBUG`) configured
out of the session scratchpad from the HEAD tree, built under
`flock -w 1800 /tmp/swiftql-build.lock`; `build/`, `build-seamfix/` and
`build-seamfix-rel/` in the working tree were **not touched**. `build-seamfix-rel/swiftql`
predates HEAD (05:54 vs 09:03) and was deliberately not reused.

The three real cells, restated so the tables below are unambiguous:

    A  --storage row      --execution volcano
    B  --storage columnar --execution volcano
    C  --storage columnar --execution vectorized
    (+ D = C with --no-optimize, an optimizer flag rather than a fourth cell)

---

## Part A.1 — S-9 re-measured on the current tree: **still open, both halves, unchanged**

### The code has not moved

- `narrowRows` is still a plan-time lambda inside `Planner::plan`
  (`src/planner/planner.cc:216-237`), still applied at the same three ROW-storage
  sites — FROM scan (`:290`), self-join right scan (`:315`), ordinary join right
  scan (`:322`) — and still rewrites every row of the table before the first
  `next()`.
- `SeqScanNode` still has no `keep_` member; the row branch of `next()` is still
  `row = &rows_[cursor_++];` (`plan_nodes.cc:84-86`) and the columnar branch still
  refills `reconstructed_row_` per call (`:73-79`). The two paths are still asymmetric
  in exactly the way pass 3's proposed fix would close.
- `python_tools/benchmark.py::run_once` still greps **`Execution:` only**
  (`benchmark.py:81`, `re.search(r'Execution:\s+([\d.]+)µs', result.stdout)`).
  Plan time is still not parsed, not summed, not reported. `MODES` still includes
  `("row", ["--storage","row"])`.

### Re-measured, Release, TPC-H sf0.1, `--storage row --execution volcano --explain-analyze --no-cache`

| query | Plan | Execution |
|---|---|---|
| `SELECT * FROM lineitem LIMIT 1` (narrowRows early-returns) | **44.6 µs** | 17.3 µs |
| `SELECT l_quantity FROM lineitem LIMIT 1` (keeps 1 of 16) | **109 834.5 µs** | 3.7 µs |
| `SELECT COUNT(*) FROM lineitem WHERE l_quantity > 30` | **109 115.4 µs** | 179 449.9 µs |

Within noise of pass 3's numbers (103 677 / 105 056 µs). **S-9 is open and unchanged.**

### One correction to pass 3's framing, made by measuring what it did not

Pass 3's headline is "**1540× slower**". That factor is over **Plan + Execution**, the
two numbers the engine prints. It excludes CSV loading, which the engine does not time
at all (`main.cc:638-639,691-692` print `Parse:`, `Plan:`, `Execution:` and nothing
else) and which dominates a process. Measured end-to-end with a wall clock around the
whole process (7 runs each, median; same Release binary, sf0.1, `--storage row
--execution volcano --no-cache --format tsv`):

| query | wall median | delta vs `SELECT *` |
|---|---|---|
| `SELECT * FROM lineitem LIMIT 1` (no narrowing) | **2697.5 ms** | — |
| `SELECT l_quantity FROM lineitem LIMIT 1` (1 of 16) | **2771.3 ms** | +73.8 ms (+2.7%) |
| `SELECT l_quantity, l_discount, l_tax, l_extendedprice ... LIMIT 1` (4 of 16) | **2853.1 ms** | +155.6 ms (+5.8%) |

So: the plan-time regression is real, it is O(rows × kept width) — keeping 4 columns
costs twice what keeping 1 costs, which is the signature of pure overhead rather than
saved work — and it is **~3-6% of the process**, not 1540× of it, because 2.7 s of
every one of these runs is `CSVLoader::load`. Both statements are true of different
denominators and pass 3 only stated the flattering one. The finding is not thereby
softened: the pass is unbounded in the data size while the load is a fixed cost of the
same data, `--explain` still pays it in full, and the measurement-integrity half is
untouched — **`benchmark.py` still reports this query as a speed-up**, because narrower
rows genuinely make `Execution:` smaller while the work moved into a timer nothing
reads. S-9 stays MEDIUM, restated below with both denominators.

### The proposed fix, re-sized on the current tree

Unchanged and still ~25 lines: give `SeqScanNode` a `std::vector<int> keep_` and a
`reconstructed_row_` fill in the row branch of `next()`, exactly mirroring
`plan_nodes.cc:71-79`; `Planner::plan` then passes `keep` instead of rewriting `rows`.
No new member types (`reconstructed_row_` already exists and is unused on the row
branch), no schema change, no planner-structure change. It also deletes L-1's
double-move hazard as a side effect, since an index used twice becomes a copy.

---

## Part A.2 — the deterministic cut, where it touches this seam

`deterministicCut` (`logical_plan.cc:995-1005`) inserts a keyless `LogicalSort` with
`row_cap = limit` beneath a `LIMIT` when `orderIsPlanStable` says the input order is not
plan-stable. Volcano reaches the same conclusion locally at `planner.cc:414`
(`order_is_plan_stable = (jc == nullptr) || !stmt.order_by.empty()`) and inserts a
`SortNode` with the same `row_cap` at `:477-480`. **The trade itself is accepted and is
not a finding.** What follows is only its interaction with the scan path.

### A.2.1 `orderIsPlanStable` classifies SCAN as stable for both storage formats — and it is true

`logical_plan.cc:961-977`: `case LogicalNodeType::SCAN: return true;`, unconditionally.
The logical layer has no storage knowledge to condition on, and Volcano's local
equivalent is storage-blind too (`jc == nullptr`). Checked rather than assumed:

- **Order.** The row branch walks `rows_[cursor_++]` and the columnar branch walks
  `cursor_` upward reconstructing per row (`plan_nodes.cc:62-86`);
  `CSVToColumnar::convert` fills every column in file order. Same sequence, not merely
  some sequence.
- **Pruning does not perturb it.** `SeqScanNode::next` advances `cursor_` past a whole
  chunk (`plan_nodes.cc:69`), so survivors keep their relative order; and a skipped
  chunk contains only rows the predicate above rejects, so the SET reaching the cut is
  unchanged. `VecScanNode::nextChunk` does the same and applies **no filter of its own**
  (`vec_scan_node.cc:17-27` prunes, nothing else) — so a pruned scan is never the last
  word on a predicate, there is always a `VecFilterNode`/`FilterNode` above it.
- **The one shape that could break it does not exist.** A `LIMIT` directly over a
  pruning SCAN with no filter between them would make the cut read a plan-dependent
  row set. Every hint route requires the filter: `planner.cc:280` derives the hint from
  `stmt.where`, which is exactly the condition under which the planner builds a
  `FilterNode`; `vectorized_plan_builder.cc:644` routes a hint only from a FILTER whose
  child is SCAN or JOIN. Enumerated in pass 2 B.5 and re-read here; still eight routes,
  still no ninth.

So the classification is correct, and correct for a reason that survives the storage
choice rather than by coincidence.

### A.2.2 The row path's `&rows_[cursor_]` return is safe under the new sort

`SortNode::open()` drains the child in a `while (true)` loop and **copies** in every
branch — `sorted_rows_.push_back(*row)` unbounded, `push_back(*row)` while the heap
fills, `sorted_rows_.back() = *row` when a candidate displaces the heap's largest
(`plan_nodes.cc:527-544`). `less(*row, sorted_rows_.front())` reads the pointer but does
not retain it. So the bug class pass 2 B.7 was written for — an operator holding a
`Row*` across a child `next()`, correct on the row path and reading a reused
`reconstructed_row_` on the columnar path — is not reintroduced by the new node. This
had to be checked because the new node is on the columnar path for the first time.

### A.2.3 Chunk pruning and zone maps under a cut: no interaction, verified rather than argued

The cut changes *how many* rows the scan is asked for, not *which*. `pruning_where_` is
fixed at plan time and consulted per chunk boundary; draining more of the scan only
means more chunks get the same test. Confirmed by `--explain-analyze`: `chunks_skipped`
is identical for the same predicate with and without a `LIMIT` above it.

### A.2.4 **FINDING S-10 — `LIMIT 0` over a join is the most expensive cut in the engine, not the cheapest**

`deterministicCut` sets `sort->row_cap = limit`. `SortNode`/`VecSortNode` read
`row_cap_ <= 0` as "**unbounded** — materialize the whole input and `stable_sort` it"
(`plan_nodes.cc:535,550`; `vec_sort_node.cc:94,107`). At `limit == 0` the bound is
tightest and the bounded top-N is therefore **off**. `LimitNode::open()` calls
`child_->open()` unconditionally (`plan_nodes.cc:592-595`) and `SortNode::open()` is
where all the work happens, so `LimitNode::next()`'s `count_ >= limit_` short-circuit
comes far too late to help.

Concrete shape, TPC-H sf0.01 (`lineitem` 60 144 rows), Release, `--explain-analyze
--no-cache`:

    SELECT l.l_orderkey, o.o_orderstatus
      FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey LIMIT <n>

| n | cell | Sort node | Execution |
|---|---|---|---|
| **0** | A row-volc | `rows_in=60144 rows_out=0` **23 987 µs** | 73 323 µs |
| **0** | B col-volc | `rows_in=60144 rows_out=0` **25 276 µs** | 89 262 µs |
| 1 | A row-volc | `rows_in=60144 rows_out=1` 3 132 µs | 54 524 µs |
| 1 | B col-volc | `rows_in=60144 rows_out=1` 2 860 µs | 62 849 µs |
| 5 | A row-volc | `rows_in=60144 rows_out=5` 2 898 µs | 50 328 µs |
| 5 | B col-volc | `rows_in=60144 rows_out=5` 2 846 µs | 64 238 µs |

**Returning zero rows costs 8.3× more than returning one**, and the gap is O(n log n)
vs O(n) so it widens with the data. Memory goes from O(1) to O(input) at the same time:
`LIMIT 0` over a self-join of a large table materialises the entire product to discard
it. On the same shape over `data/laps.csv` the Volcano sort takes 47.9% of execution
(`rows_in=10000 rows_out=0`) to produce nothing.

**The engines disagree about it, which is the second half.** `VecLimit [0]` never pulls,
so on the vectorized path the sort is opened and reads nothing:

| cell | `SELECT d.name, l.speed FROM drivers d JOIN laps l ON … LIMIT 0` |
|---|---|
| A row-volc | `Sort rows_in=10000` … Execution **12 765.6 µs** |
| B col-volc | `Sort rows_in=10000` … Execution **13 708.0 µs** |
| C col-vec | `VecSort rows_in=0` … Execution **13.3 µs** |

~1000×, same answer (0 rows) in all three. This is not a storage divergence — A and B
agree exactly — but it is a defect in the cut's own guard on the path this seam owns,
and it is **not** part of the accepted trade: at `limit == 0` there is no cut to make
deterministic, so the sort buys nothing at all. Two independent one-line fixes:
`if (limit <= 0) return node;` at the top of `deterministicCut` (and the matching
`stmt.limit.value() > 0` at `planner.cc:477`), or make `LimitNode::open` skip the child
when `limit_ == 0`. Severity **LOW** — no wrong answer, `LIMIT 0` is a rare shape
(it is the "plan/schema probe" idiom), and the escape is trivial.

### A.2.5 **FINDING S-11 — the cut's sort sits ABOVE the projection, so the whole SELECT list is evaluated over the whole input; a declared `ORDER BY` does not do this**

`deterministicCut`'s own comment states the placement as a benefit ("inserted directly
beneath the `LIMIT`, i.e. ABOVE the projection: fewer columns to compare"). It has a
cost the comment does not state. Measured plan shapes, same query with and without
`ORDER BY`, 5000-row join, Release:

    -- declared ORDER BY: sort is BELOW the projection
    Limit [1]  rows_in=1
      Project [v]              rows_in=1      rows_out=1
        Sort [b.id]            rows_in=5000   rows_out=1
          HashJoin …           rows_in=5000

    -- deterministic cut: sort is ABOVE the projection
    Limit [1]  rows_in=1
      Sort [canonical row order]  rows_in=5000   rows_out=1
        Project [v]               rows_in=5000   rows_out=5000
          HashJoin …              rows_in=5000

So dropping `ORDER BY b.id` from a `LIMIT 1` query moves the projection from **1 row to
5000**. On the vectorized path the projection is also where `appendColumnValue` runs,
and that is where the two new refusals live — so the shape change converts an answered
query into a refused one:

    v = CASE WHEN b.c = 1 THEN b.big ELSE b.dd END       -- b.big = 1000000000000007

| query | A row-volc | B col-volc | C col-vec | D col-vec `--no-optimize` |
|---|---|---|---|---|
| `SELECT v FROM big2 b JOIN k5000 a ON b.id=a.id ORDER BY b.id LIMIT 1` | 0 | 0 | **0** | **0** |
| `SELECT v FROM big2 b JOIN k5000 a ON b.id=a.id LIMIT 1` | 0 | 0 | **REFUSED** | **REFUSED** |

(`big2` = 5000 rows, only the last of which takes the INT branch; `k5000` joins 1:1.
Refusal text: *"vectorized execution cannot materialize the integer 1000000000000007
into a DOUBLE result column without changing it…"*.) **Adding an `ORDER BY` that cannot
change the answer makes the query run.** Volcano answers `0` in both storage formats
either way, so the storage oracle is intact and this is not a divergence between A and
B — but the vectorized cell's answerability is now decided by a plan-shape detail
rather than by the query. Severity **LOW**: a loud refusal with a documented and working
escape (`--execution volcano`, which this shape does support), no wrong answer, and the
cost half is bounded by the projection's own cost.

### A.2.6 **FINDING S-12 — the bounded top-N is wired to the deterministic cut only; a real `ORDER BY … LIMIT n` never gets it**

`row_cap` is assigned in exactly two places in the tree, and both are the cut:
`logical_plan.cc:1003` (`sort->row_cap = limit`) and `planner.cc:479`. The declared
`ORDER BY` sort is built with the two-argument constructor (`planner.cc:421`) and
`LogicalSort`'s `row_cap` default of 0 (`logical_plan.h:276`), so **top-N over a large
table sorts the entire table**. Verified by measurement — adding a `LIMIT` to an
`ORDER BY` query changes the sort's cost by nothing (TPC-H sf0.1, `lineitem` 600 865
rows, Release, `--explain-analyze --no-cache`):

| query | cell | Sort node time | Execution |
|---|---|---|---|
| `SELECT l_orderkey FROM lineitem ORDER BY l_orderkey LIMIT 5` | A row-volc | 1 003 371 µs | 1 075 076 µs |
| `SELECT l_orderkey FROM lineitem ORDER BY l_orderkey` | A row-volc | 966 809 µs | 1 162 441 µs |
| `SELECT l_orderkey FROM lineitem ORDER BY l_orderkey LIMIT 5` | C col-vec | 1 109 962 µs | 1 110 039 µs |
| `SELECT l_orderkey FROM lineitem ORDER BY l_orderkey` | C col-vec | 1 095 796 µs | 1 187 150 µs |

The vectorized node reports it outright: `VecSort [l_orderkey] rows_in=600865
rows_out=600865` under a `LIMIT 5`.

**The size of the miss, isolated with the same comparator on the same input.** A.2.4's
table is the controlled experiment: keyless canonical sort over the identical 60 144-row
join, `row_cap=0` (LIMIT 0) at 23 987 µs versus `row_cap=5` (LIMIT 5) at 2 898 µs —
**8.3× on the sort, and O(row_cap) memory instead of O(input)**. Nothing about that
speed-up depends on the comparator being keyless; the same heap is what `order_by_`
non-empty would use.

Method note, since this is a performance claim: every number above is the engine's own
`--explain-analyze` per-node `time=` and `Execution:` line on a Release build, single
run, quiet box. **It is not `benchmark.py`'s figure** — but unlike S-9, `benchmark.py`
*would* see this one, because the cost is in `Execution:`. Nobody has looked.

Scope, stated honestly: of the five TPC-H queries in `tpch_queries.py` that carry a
`LIMIT` (q2, q3, q10, q18, q21), all sort a *grouped* result, which is small — so this
does not move the TPC-H baseline. It bites on the ungrouped top-N over a base table,
which is the canonical shape and the one measured above. Fix is one argument at
`planner.cc:421` and one assignment where `LogicalLimit` is built over a `LogicalSort`.
Severity **MEDIUM**: no wrong answer, but it is an 8×+ execution regression against the
engine's own already-written machinery, on this project's headline shape, in both
engines, and the fix is smaller than the comment explaining it.

---

## Part A.3 — the two new vectorized refusals, against this seam

Both are runtime, both live in `appendColumnValue`'s narrowing path
(`vec_types.h:174-186` `narrowToDoubleColumn`, `:225-236` `refuseObservableIntNarrowing`),
and both are vectorized-only. Measured across the cells on purpose-built data
(`mix(c INT, big INT, dd DOUBLE)`, `big` = 1000000000000001…):

| query | A row-volc | B col-volc | C col-vec |
|---|---|---|---|
| `SELECT c, CASE WHEN c=1 THEN big ELSE dd END AS v FROM mix ORDER BY c, v` | 12 rows, `1000000000000001`… | **identical** | REFUSED (`…without changing it`) |
| `SELECT MIN(CASE WHEN c=1 THEN big ELSE dd END) FROM mix` | `0` | `0` | `0` |
| `SELECT x / 2 FROM (SELECT CASE WHEN c=1 THEN 7 ELSE 0.5 END AS x FROM mix) t` | refused (derived) | refused (derived) | REFUSED (`…that another expression divides`) |
| `SELECT MIN(CASE WHEN c=1 THEN 7 ELSE 0.5 END) / 2 FROM mix` | `0.25` | `0.25` | `0.25` |

Three things follow, and the first is the important one for this seam.

1. **The refusals do not create an uncovered cell — they create Volcano-only shapes,
   and Volcano is precisely the engine that runs both storage formats.** Row 1 is a
   shape only Volcano answers, and A and B agree byte-for-byte on it. So the new
   refusals *widen* the storage oracle's jurisdiction rather than escaping it: every
   shape they push off the vectorized path lands in the one cell pair this seam owns.
   That is the opposite of S-0's structural complaint and worth recording as such.
2. **`ColumnArray`'s missing validity concept does not interact with them.** The
   refusals fire inside `appendColumnValue`, which is also the NULL path — but the NULL
   branch is taken first (`if (v.isNull())`) and returns before any narrowing, and no
   `Value` reaching *storage* is ever null in either format (pass 2 B.2, unchanged:
   `parseField` has no NULL production). Constructed and checked: a `CASE` with no
   `ELSE` over the same columns produces NULLs *above* the scan, in the projection's
   own ColumnVector, and its validity mask is built by the same function that then
   refuses the big INT — the two are sequential, not entangled.
3. **The one real interaction is A.2.5's**, and it runs the other way: the deterministic
   cut decides *how many rows are materialized*, and a runtime refusal fires per
   materialized value. Same query, `ORDER BY` present → answered, absent → refused.
   Recorded there.

Position-dependence of the refusal is a property of any runtime check and is stated here
so it is not rediscovered as a defect: on `big2` (offending row last), `--execution
vectorized` answers `LIMIT 1` and `LIMIT 1024` and refuses `LIMIT 4999` and `LIMIT 5000`
— the granularity is `BATCH_SIZE = 1024`, not the limit, because `VecProjectNode`
materialises a whole batch before `VecLimitNode` truncates it. A and B answer all four.
Output is fully buffered before printing, so a refusal never emits a partial result set
(checked: refusal runs return exit code 1 and one line of output, never rows-then-error).

---

## Part B — hunting what three passes missed

### B.1 Zone maps above 2^53 — the INT/DOUBLE coercion in `canSkipChunk`, closed

`ChunkPruner::canSkipChunk` compares a literal `Value` against a chunk's `min_val`/
`max_val` with `Value::operator<` etc., and those coerce through `toNumeric()` (both
sides to `double`) whenever one side is INT and the other DOUBLE
(`value.cc:52-58` `NUMERIC_COERCE`). Above 2^53 that rounding is lossy, so a chunk whose
true max is `9007199254740993` compares equal to the literal `9007199254740992.0` and
`>` reads `val >= mx` as true — **a chunk that does hold matching rows can be skipped**.
No earlier pass tested this: pass 2's boundary sweep ran on values far below 2^53.

It is **sound anyway**, and for a reason worth writing down: the FILTER above the scan
performs the same coercion. Volcano's evaluator uses the same `Value` operators, and the
vectorized executor compiles a `CAST_DOUBLE` node around the INT side so the kernel is
homogeneous (`expression_executor.cc:20,38-41` — and that file already documents the
2^53 hazard for `IN`, which is why its sets are split by storage type). Pruner and filter
therefore round identically, and a chunk the pruner drops holds only rows the filter
would have dropped.

Verified, not merely argued. Purpose-built `bigi(k INT, d DOUBLE, s STRING, g INT)`,
20 000 rows, `k` ascending from `9007199254730000` so that **2^53 falls inside chunk 1**
of 3, zone maps disjoint. For each chunk's min and max, each spelled four ways
(`v`, `v.0`, `v+1`, `(v+1).0`) and each of `= < > <= >=`:

    120 queries x 3 cells (A, B, C), each also checked against an independent
    Python count that mirrors the coercion rule
    -> 0 divergences, 0 wrong counts

Both spellings of the same number give different answers — `COUNT(*) WHERE k >
9007199254740992` is 9007 and `WHERE k > 9007199254740992.0` is 9006 — but they give
the *same* different answer in all three cells. That is SQL numeric coercion, identical
in row and columnar, not a pruning defect.

### B.2 Differential batches — 322 hand-written queries across A/B/C/D, 0 storage divergences

`deterministicCut` **landed after pass 3's HEAD** — `git merge-base --is-ancestor
d085230 922ca15` is false, so every `LIMIT` shape pass 3 differenced was differenced
without the new sort node in the tree. Re-running them is new coverage, not a repeat.
Byte-exact comparison of `--format tsv --no-cache` stdout+stderr (columnar encoding
banner filtered) across all four invocations:

| batch | catalog | what it targets | queries | divergent |
|---|---|---|---|---|
| 1 | `catalog.json` | joins under a `LIMIT` with no `ORDER BY` (the cut fires) at 0/1/2/3/5/1023/1024/1025/8191/8192/8193/9999/10000/10001; LEFT joins; self-join; `SELECT *`; DISTINCT; GROUP BY + HAVING; shared column name `team` as the join key; `ORDER BY` controls both directions | 83 | **0** |
| 2 | synthetic `bigi`/`alt` | 3 chunks with disjoint clustered zone maps, `alt.k` deliberately disjoint from `bigi.k` **and sharing the name `k`**; cut at every chunk boundary; pruning + cut in one query; aggregates and `COUNT(DISTINCT)` spanning chunks; predicates that prune all/none | 131 | **0** |
| 3 | TPC-H sf0.01 | `lineitem`(8 chunks) ⨝ `orders`(2 chunks) under the cut at 0/1/3/1023/1024/1025/8191/8192/8193/16385; pruning that skips leading chunks combined with a cut; single-relation controls | 108 | **0** |

The synthetic batch is not vacuous: a leaked pruning hint on `bigi` collapses the count
to 0 by construction, and `chunks_skipped=1/3` is observed live on the same predicates.

### B.3 Randomized differencing, LIMIT-biased — 750 queries, **0 storage divergences**, 2 order-only vectorized-optimizer differences

`python_tools/random_diff.py` is still vectorized-only (`VEC = ["--execution",
"vectorized", "--storage", "columnar"]`, line 112), so the shipped harness still never
randomly differences the row leg. Closed again here with a generator biased hard toward
the new node: 85% of shapes carry a `LIMIT` (drawn from 0/1/2/3/5/7/10/50/1023/1024/
1025/8191/8192/8193/10000) and only 30% carry an `ORDER BY`, so the deterministic cut
fires on most of them. Three seeds × 250 queries, all four invocations, byte-exact:

    seed 20260808   250   1 divergent
    seed 4242       250   1 divergent
    seed 99991      250   0 divergent

**Both divergences are the same shape, and neither is a storage divergence:**

    SELECT d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id
      WHERE l.sector_2 = 40.0
    SELECT d.driver_id, d.team FROM laps l JOIN drivers d ON l.team = d.team
      WHERE l.sector_1 = 40.0

    A row-volcano  ==  B col-volcano  ==  D col-vec --no-optimize
    C col-vec (optimized): SAME 12 (resp. 13) rows, DIFFERENT ORDER

Verified as a multiset identity (`sorted(C) == sorted(A)`, `len` equal) and diagnosed
with `--explain-analyze`: `JoinEnumeration` promotes `drivers` to `children[0]`, so the
`VecSimdLoopJoin` emits drivers-major instead of laps-major. **That is the documented,
accepted behaviour** — `orderIsPlanStable` states JOIN is not order-stable and
`deterministicCut` forces determinism only where the order becomes an *answer*, i.e.
beneath a cut. Neither query has a `LIMIT`, so the row set is identical and nothing
downstream can see the difference. **Not a finding**, recorded because a mechanical
reading of "optimized == --no-optimize" would call it one.

One latent trap worth a line, since it belongs to whoever owns that contract:
`run_engine_agreement_suite` compares modes with `normalize(..., preserve_order=True)`
(`compare_against_sqlite.py:3159-3162`), i.e. **order-sensitively**. Every query in
`ENGINE_AGREEMENT_QUERIES` therefore has to be order-determined by construction
(single relation, or `ORDER BY`, or a `LIMIT` that the cut now determines). Adding an
unordered join query to that list would fail the gate for a reason that is not a defect.

Grand total for this pass: **322 + 750 = 1072 queries × 4 invocations, 0 divergences
between the two storage formats.**

### B.4 A second, compounding cost on the row leg: subquery materialization copies and re-narrows the whole table

`main.cc:527-537` — for each uncorrelated subquery the runner builds a **fresh copy**
of every table the body touches (`rows_copy.emplace(n, table_rows.at(n))` on the row
leg, `cols_copy.emplace(n, columnar_tables.at(n))` on the columnar leg) and calls
`runVolcanoToRows`, which runs `Planner::plan` again — and therefore `narrowRows` again,
on the copy. The outer query is exempt (`buildScanSchema` returns the full schema when
`has_subquery` is set, so `narrowRows` early-returns), but the body is not. The whole
cost lands in the `Plan:` number, deliberately (`subquery_us` is added to `plan_us`).

Measured, Release, TPC-H sf0.1, `--execution volcano --explain-analyze --no-cache`:

| query | storage | Plan | Execution |
|---|---|---|---|
| `SELECT COUNT(*) FROM lineitem WHERE l_quantity > (SELECT AVG(l_quantity) FROM lineitem)` | **row** | **1 926 822 µs** | 194 276 µs |
| same | columnar | 485 789 µs | 208 565 µs |
| `SELECT COUNT(*) FROM lineitem WHERE l_quantity > 25` | **row** | 110 890 µs | 188 852 µs |
| same | columnar | **33.7 µs** | 195 253 µs |

Both storage modes pay a full-table copy per subquery — that half is the shared-table
representation item already deferred to Week 37 (`main.cc:515`'s comment says so). What
is specific to this seam is the 4× gap between the two rows of the first pair: the row
leg copies 600 572 × 16 `Value`s (each a `std::variant` that may own a `std::string`)
and then rewrites all of them a second time. This is not a new finding — it is S-9
reaching a second call site — but it is the largest single number this seam produces and
pass 3 did not measure it.

### B.5 Re-checks that came back clean, stated so they are not re-derived

- **Chunk-pruning hint naming the wrong relation.** Re-read `chunk_pruner.h` with the
  cut in the tree; the guard is unchanged (`col->id.isLocal() && localSlot(...) < 1`) and
  the hint is still computed at plan time from `stmt.where`, so draining more of a scan
  cannot change *which* chunks are tested. Batch 2's `alt.k`-disjoint construction would
  collapse the count to 0 on a leak; it does not. `collectSimplePredicates` still drops
  everything under an `OR` (the `dynamic_cast<BinaryExpr>` succeeds but `op != "AND"`,
  and its left child is not a `ColumnRef`, so nothing is collected) — checked because a
  conjunct collected out of a disjunction would be an unsound skip.
- **`RLEColumn::decodeRange` (vectorized) vs `RLEColumn::get` (Volcano).** The two
  storage-decode paths are different code, so a boundary bug there would show as B vs C.
  Re-read: `run_starts` is strictly increasing and `run_end > row` holds on every
  iteration, so the loop always advances and cannot walk off `runs`. Exercised by every
  columnar query above (`laps.season` and `bigi.g` both RLE).
- **`CSVToColumnar::convert`.** Zone maps are still built *after* encoding through
  `table.getValue`, so RLE/dictionary columns yield typed values rather than codes, and
  every column uses the same `start += CHUNK_SIZE` loop over the same `num_rows` — chunk
  boundaries are identical across columns by construction, not by agreement.
- **The subquery runner respects `--storage`.** `run_subquery` picks `cols_copy` or
  `rows_copy` per table (`main.cc:529-531`), so a nested query runs in the same storage
  mode as its parent. A scalar subquery whose body contains a join therefore gets the
  same deterministic cut in both formats; batch 1's join+LIMIT shapes cover the outer
  case and this closes the nested one by construction.

### B.6 Week 37's `VecScanNode` item — sizing revised **upward**, and the ordering advice reaffirmed

`main.cc:548` is unchanged (`--execution vectorized requires --storage columnar`), so
there are still three real cells. Pass 3 sized the item at 85-105 lines. On the current
tree it is **~95-120**, for one reason that did not exist when pass 3 wrote its estimate:

**`appendColumnValue` now carries two throwing refusals** (`vec_types.h`,
`narrowToDoubleColumn` and `refuseObservableIntNarrowing`). The obvious way to fill a
`ColumnVector` from a `Row` is `appendColumnValue(cv, row[i])` — and that would route
*base-table* values through a refusal path the columnar constructor never touches
(`vec_scan_node.cc:59-80` writes the typed vectors directly). For a base column the
declared type and the parsed `Value` type always agree, so nothing would actually throw
today — but it would make the row-backed scan's behaviour depend on a check written for
expression results, which is exactly the kind of coupling this seam keeps paying for.
The row-backed fill should mirror the columnar branch and write `cv.data` directly:
+3 switch arms, ~10 more lines than the naive version.

Everything else pass 3 said still holds, including the load-bearing half: the logical
layer hands its SCAN a **narrowed** schema (`blockOutputSchema` → `buildScanSchema`),
`narrowRows`/`keep` is still a local lambda inside `Planner::plan` and still unreachable
from `vectorized_plan_builder.cc`, so the row-backed `VecScanNode` needs the same index
resolution. **Do S-9 first.** Its fix produces exactly the `keep_` index vector a scan
node needs, in a scan node, and the Week 37 fill loop then becomes `row[keep_[i]]` in a
loop that has to be written anyway — one indirection instead of a hoisted helper or a
duplicated lambda.

---

## Findings

Ranked. Every one has a concrete shape run across the three real cells with the outputs
recorded side by side above.

### S-9 — MEDIUM — **still open**: `narrowRows` is an O(rows × width) plan-time pass on the row leg, and `benchmark.py` reports it with the wrong sign

Unchanged in code and re-measured on this tree (A.1). Concrete shape, Release, TPC-H
sf0.1, `--storage row --execution volcano`:

    SELECT * FROM lineitem LIMIT 1            Plan      44.6 µs   Execution 17.3 µs
    SELECT l_quantity FROM lineitem LIMIT 1   Plan 109 834.5 µs   Execution  3.7 µs

**Both denominators, so neither is misleading.** Over the two numbers the engine prints,
that is ~2 400×, and pass 3's 1540× pre/post figure stands. Over the whole process it is
+73.8 ms on 2 697.5 ms = **+2.7%** (7-run medians), because ~2.7 s of every run is
`CSVLoader::load`, which the engine does not time at all. It is O(rows × kept width) —
keeping 4 columns costs +155.6 ms where keeping 1 costs +73.8 ms — so it grows with the
data *and* with the columns kept; `--explain` pays it in full; and B.4 shows it lands a
second time, on a fresh full-table copy, for every uncorrelated subquery (row plan time
1.93 s vs columnar 0.49 s on the same query). The measurement-integrity half is
untouched: `benchmark.py:81` still greps `Execution:` only, `MODES` still includes
`("row", …)`, and narrower rows genuinely make `Execution:` smaller — so the instrument
still moves in the flattering direction on queries that got slower. Fix unchanged
(A.1): a `keep_` index vector consumed in `SeqScanNode::next()`, mirroring the columnar
branch's `reconstructed_row_`, ~25 lines.

### S-12 — MEDIUM — the bounded top-N is wired to the deterministic cut only; a declared `ORDER BY … LIMIT n` sorts the whole table

`row_cap` is assigned in exactly two places, both the cut (`logical_plan.cc:1003`,
`planner.cc:479`). Concrete shape, Release, TPC-H sf0.1:

    SELECT l_orderkey FROM lineitem ORDER BY l_orderkey LIMIT 5
      A row-volcano   Sort 1 003 371 µs   Execution 1 075 076 µs
      C col-vec       VecSort [l_orderkey] rows_in=600865 rows_out=600865
                                          1 109 962 µs   Execution 1 110 039 µs
    SELECT l_orderkey FROM lineitem ORDER BY l_orderkey      (no LIMIT)
      A row-volcano   Sort   966 809 µs   C col-vec  VecSort 1 095 796 µs

Adding the `LIMIT` changes the sort's cost by nothing. Size of the miss, isolated with
the same comparator on the same input (A.2.4's table): keyless canonical sort over the
same 60 144-row join, `row_cap=0` 23 987 µs vs `row_cap=5` 2 898 µs — **8.3×**, plus
O(row_cap) memory instead of O(input). Method: the engine's own `--explain-analyze`
`time=`/`Execution:` on a Release build, single run — **not** `benchmark.py`'s figure,
though unlike S-9 `benchmark.py` *could* see this one. Scoped honestly: the five TPC-H
queries carrying a `LIMIT` all sort a grouped (small) result, so the baseline does not
move; it bites on the ungrouped top-N over a base table, which is the canonical shape.
Fix: one argument at `planner.cc:421` and one assignment where the logical `LIMIT` is
built over a declared `LogicalSort`.

### S-10 — LOW — `LIMIT 0` over a join is the most expensive cut in the engine, and the two engines disagree about it by ~1000×

`sort->row_cap = limit` with `limit == 0` reads as "unbounded" in both sort nodes
(`plan_nodes.cc:535,550`; `vec_sort_node.cc:94,107`), and `LimitNode::open()` opens its
child unconditionally. Concrete shape, TPC-H sf0.01:

    SELECT l.l_orderkey, o.o_orderstatus FROM lineitem l JOIN orders o
      ON l.l_orderkey = o.o_orderkey LIMIT 0
        A row-volc  Sort rows_in=60144 rows_out=0  23 987 µs   Execution 73 323 µs
        B col-volc  Sort rows_in=60144 rows_out=0  25 276 µs   Execution 89 263 µs
      … LIMIT 1
        A row-volc  Sort rows_in=60144 rows_out=1   3 132 µs   Execution 54 524 µs

and on `catalog.json`, `drivers ⨝ laps LIMIT 0`: A 12 765.6 µs / B 13 708.0 µs /
**C 13.3 µs** (`VecLimit [0]` never pulls, so `VecSort rows_in=0`). Same answer (0 rows)
in all three. Returning nothing costs 8.3× returning one, memory goes O(1) → O(input),
and at `limit == 0` the sort buys nothing at all — there is no cut to make deterministic
— so this is not part of the accepted trade. One line: `if (limit <= 0) return node;`
in `deterministicCut`, plus the matching `> 0` at `planner.cc:477`.

### S-11 — LOW — the cut's sort sits above the projection, so adding an `ORDER BY` that cannot change the answer decides whether a query runs

`deterministicCut` inserts beneath the `LIMIT` and therefore above the `PROJECT`, so the
projection is evaluated over the entire input (`Project rows_in=5000 rows_out=5000`); a
declared `ORDER BY` sorts *below* the projection (`Project rows_in=1`). On the
vectorized path the projection is where `appendColumnValue`'s two new refusals run.
Concrete shape (`big2` 5000 rows, only the last taking the INT branch; `k5000` joins 1:1;
`v = CASE WHEN b.c = 1 THEN b.big ELSE b.dd END`, `b.big = 1000000000000007`):

| query | A | B | C | D |
|---|---|---|---|---|
| `SELECT v FROM big2 b JOIN k5000 a ON b.id=a.id ORDER BY b.id LIMIT 1` | `0` | `0` | `0` | `0` |
| `SELECT v FROM big2 b JOIN k5000 a ON b.id=a.id LIMIT 1` | `0` | `0` | **REFUSED** | **REFUSED** |

Volcano answers identically in both storage formats either way, so the storage oracle is
intact; what changes is whether the vectorized cell is *able* to answer, decided by a
plan-shape detail rather than by the query. A loud refusal with a working documented
escape, no wrong answer.

### L-1, L-2, L-3 — LOW — carried forward from pass 3, all three re-verified as still live

- **L-1**: `catalog.cc:49-55`'s duplicate-column refusal is still the only thing keeping
  `narrowRows`' `full.indexOf(c.name)` (first match) from putting one index into `keep`
  twice and double-moving a `Value`; its comment still justifies itself only by the
  columnar interleaving bug. Unchanged.
- **L-2**: `catalog.cc`'s `tables_.emplace` still silently keeps the first of two
  same-named tables, identically in all three cells — catalog input validation, not a
  storage divergence.
- **L-3**: `narrowRows`' `if (narrowed.size() == full.size()) return rows;` is still
  correct only because `narrowSchema` builds an order-preserving subset; still nothing
  states or tests that coupling.

### Not findings, recorded so pass 5 does not re-derive them

- **Order-only vectorized-optimizer differences on unordered joins** (B.3). Two of 750
  randomized queries; identical multiset, `JoinEnumeration` flips the probe side; A = B
  = D always. Documented and accepted by `orderIsPlanStable`'s own JOIN rule.
- **Zone maps above 2^53** (B.1). `canSkipChunk` compares through `toNumeric()` and is
  lossy above 2^53, but the FILTER above performs the identical coercion (Volcano via
  `Value`'s operators, vectorized via a compiled `CAST_DOUBLE`), so a skipped chunk holds
  only rows the filter rejects. 120 boundary queries × 3 cells straddling 2^53, 0
  divergences, 0 wrong counts against an independent Python count.
- **The two new refusals widen the storage oracle rather than escaping it** (A.3). Every
  shape they push off the vectorized path lands in Volcano, which is the one engine that
  runs both storage formats; A and B agree on all of them.
- **`SELECT x/2 FROM (SELECT CASE … END AS x …) t` has zero executable cells** — the
  derived table is refused by Volcano and the division refusal fires on the vectorized
  path. The refusal text says so honestly ("it does not run derived tables"). Engine
  seam, not this one.

---

## Summary

| severity | count |
|---|---|
| BLOCKER | **0** |
| HIGH | 0 |
| MEDIUM | 2 — S-9 (carried, unchanged), S-12 (new) |
| LOW | 5 — S-10 (new), S-11 (new), L-1, L-2, L-3 (all carried) |

**S-9 status: OPEN, unchanged in code and re-measured on this tree.** `narrowRows` is
still a plan-time pass inside `Planner::plan`; `SeqScanNode` still has no `keep_`;
`benchmark.py::run_once` still parses `Execution:` and nothing else. Pass 4 adds two
things pass 3 did not have: the other denominator (+2.7% of a process, not 1540× of one)
and a second call site (subquery materialization re-narrows a fresh full-table copy —
row plan time 1.93 s vs columnar 0.49 s on one TPC-H query).

Evidence base for this pass: **1072 queries × 4 invocations across all three real cells,
0 divergences between the two storage formats** — 322 hand-written (LIMIT-at-chunk-
boundary, pruning-with-a-cut, disjoint-zone-map leak detectors, 2^53 boundary sweep) and
750 randomized with 85% carrying a `LIMIT`, on a tree where the deterministic cut did not
exist when pass 3 ran. The two randomized "divergences" are row-order-only and are the
accepted behaviour of `orderIsPlanStable`'s JOIN rule.

**Verdict: the storage seam is clean, for the fourth time and now with the deterministic
cut on the path — row and columnar have never once disagreed. The four open items are
all cost or refusal-reachability, not correctness: S-9 is unmoved, and the cut arrived
with a bounded top-N that is not wired to the one query shape that most needs it (S-12),
is disabled at the tightest bound (S-10), and sits above the projection where it decides
answerability rather than only order (S-11). No blockers; the audit ends here.**
