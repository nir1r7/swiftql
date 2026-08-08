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
