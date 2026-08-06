# Week 27 — Multi-Way Join Execution

Teaching plan for the Phase 5 / Week 27 checkpoint. Working branch:
`claude/phase5-week26-qomtkb` (everywhere the workflow says `main`, read that).

**Checkpoint (README):** *Three-or-more-table joins execute correctly.*

That is the whole bar. **Join *enumeration* is Week 28** — this week keeps the
left-deep, written-order join tree exactly as the parser gave it, and changes
nothing about which order relations are joined in. **Outer joins are Week 29** —
every join here stays an inner equi-join. TPC-H 22/22 is the phase goal; it
informs *which* generalizations are worth making (multi-key keys for Q9,
residual `ON` conjuncts for Q21-style conditions), but it is not this week's
deliverable.

Week 26 left a system where a multi-way or multi-key join *binds, validates and
builds a complete logical plan* and then refuses at lowering. Week 27 is
therefore not a "build the feature" week — it is a **"remove the refusals and
make the machinery underneath them true"** week. That framing matters, because
the refusals are the only reason a wrong multi-way answer is impossible today.
Every step below is either (a) making something correct that the refusal was
hiding, or (b) removing a refusal once its hazard is gone.

---

## What Week 27 must deliver

| README bullet | Tasks |
|---|---|
| Lower general logical join trees to vectorized hash joins | 1, 2, 3 |
| Build a join graph and assign local and join predicates | 4 (see the scope note below) |
| Route non-equality `ON` conjuncts as residual post-join filters during predicate assignment | 4 |

> **Scope note on "build a join graph."** Do not build a `JoinGraph` type. The
> data structure that bullet is reaching for is *join enumeration's* input, and
> enumeration is Week 28 — building it now means building it before the consumer
> exists, which violates rule 2 of `CLAUDE.md` ("nothing speculative"). What the
> bullet *requires* at this checkpoint is the property, not the class: **every
> conjunct in the query — `WHERE` and `ON` alike — is assigned to exactly one
> place in the tree, and that place is legal.** Week 26 already delivered half of
> it (`soleSlot()` / `distribute()` route each `WHERE` conjunct to the relation
> slot that owns it). Task 4 delivers the other half by making `ON` conjuncts
> that are not equi-join keys enter the same assignment machinery instead of
> being rejected. When Week 28 needs an explicit graph, it will build it from
> `LogicalJoin::keys`, which is already the edge list.

## The Starting notes, and where each is answered

Every bullet of the README's *"Starting notes, from Week 26's foundations"* for
Week 27:

| Starting note | Answered in |
|---|---|
| **Two guards must come down together with the lowering work** — `checkLowerable` (vec builder) and the `joins.size() > 1` / `keys.size() != 1` pair in `Planner::plan` | **Task 2** (both *multi-key* refusals, one commit) and **Task 3** (the *multi-way* pair). See "How the guards come down" below — this is the single most important sequencing decision of the week |
| Volcano has no multi-way execution planned: keep its refusal, narrow the message to name the path | **Task 3 §4** — `... not supported on the Volcano path; use --execution vectorized`. Deleting it and letting `HashJoinNode` run would silently drop a relation |
| `Planner::plan`'s deferred *multi-key* flag: delete the flag with the guard, keep the multi-way one early | **Task 2 §3** — the flag and its late throw both die; the early multi-way `if` stays exactly where it is |
| **De-duplicate join keys before building the hash-key tuple** — `ON a.x = b.x AND a.x = b.x` yields two identical `JoinKey`s, widening the probe tuple and double-counting in the NDV product | **Task 1 §2** — deduped inside `classifyJoinCondition`, which fixes both symptoms at their single source |
| **`Validator::validateJoinCondition` (site 18) is a second opinion, not the authority** — re-checks by slot, never by `table_name`; its Week 25 and `AggregateExpr` branches are pre-positions | **Task 4 §5** — those branches go live this week; the task's rule is *bind residuals like any other expression and keep the slot check slot-based*. No code change is needed there, which is exactly why it needs a test |
| **`keys[k].from_col` resolves against `children[0]`, whose merged schema can hold the same column name at several slots** — use `Schema::indexOf(name, slot)` | **Task 1 §1** — and it is the one defect this week that produces *wrong rows* rather than an error, so it is Task 1 and not Task 3 |
| `--explain` of a multi-way join currently errors instead of printing the logical section; lifting the guard fixes it for free | **Task 3 §Verification** — verified, not coded. Do not reorder `main.cc` |

## Prerequisite knowledge

Read these before writing code. Week 27 touches no new `Expr` subtype, so
`development.md`'s 18-site dispatch checklist is **not** a per-task chore this
week — but three of its sites become live in new ways, and you need to know
which:

- **`src/planner/join_condition.{h,cc}`** — `classifyJoinCondition`, the whole
  `ON`-clause contract. Task 1 and Task 4 both rewrite parts of it.
- **`src/planner/logical_plan.cc`, `LogicalPlanBuilder::build`** — the left-deep
  fold and the merged-schema slot stamping (`joins[i]` → slot `i+1`).
- **`src/planner/predicate_pushdown.cc`** — `collectSlots` / `soleSlot` /
  `restampSlots` / `distribute`. Task 4 gives `collectSlots` a second caller.
- **`src/common/schema.h`** — the two `indexOf` overloads and what
  `ColumnDef::relation_slot` means. **If you read one thing, read this.**
- `development.md` → *Extending the expression language*, **site 8, 9 and 18**.
  Site 18 (`Validator::validateJoinCondition`) stops being a pre-position this
  week and becomes the only column-existence check for residual `ON` conjuncts.
- The `invariants` skill (join schema ordering, `relation_slot` resolution,
  pipeline breakers) and the `vectorized-audit` skill (`DataChunk` lifecycle,
  `SelectionVector` invariants) — Task 2 and Task 3 both edit vectorized
  operators, and both skills exist to catch exactly what those tasks can break.

## The three identities this week rests on

Week 26 established one arithmetic identity; Week 27 adds two consequences of
it. Write them on a sticky note:

```
(1)  range-table slot of stmt.joins[i]'s relation  ==  i + 1        (Week 26)

(2)  LogicalJoin::children[1] is ALWAYS exactly one relation        (left-deep)
     LogicalJoin::children[0] may be a whole join subtree

(3)  therefore: keys[k].join_col is unambiguous by bare name,
                keys[k].from_col is NOT — it needs from_slot
```

Identity (3) is the entire reason `JoinKey::from_slot` exists, and forgetting it
is the one mistake this week that returns rows instead of an error.

## How the guards come down

The Starting notes say the two guards "must come down together **with the
lowering work**." Read that as a constraint on commit boundaries, not on a
single big-bang commit. The failure mode being prevented is **mode divergence**:
if the vectorized path stops refusing a shape while `Planner::plan` still
refuses it, then `--storage row` and `--execution vectorized` disagree about
whether a legal query is legal — and `compare_against_sqlite.py` runs every
query in four modes, so this shows up as a harness failure with a confusing
message.

The safe decomposition is by *shape*, not by *engine*:

| Commit | Makes executable | Guards deleted, together |
|---|---|---|
| Task 2 | multi-**key**, on **both** engines | `checkKeyCounts` (vec) **and** `multi_key_join` + its late throw (Volcano) |
| Task 3 | multi-**way**, on the **vec** engine only | the `joins_seen > 1` refusal (vec); the Volcano `joins.size() > 1` refusal **stays**, message narrowed |

Between the two commits the engines still agree on every query. After Task 3,
multi-way is the first *deliberate* capability difference between the paths —
which is what the README's Week 26 text predicted ("Week 27 turns it into an
ordinary capability difference — row mode never gains multi-way execution") and
what Task 5 teaches the harness to express.

---

## Task 1 — Slot-exact, de-duplicated join keys

### Why it matters

This is the foundation task, and the only one whose defect class is *wrong
rows*. Everything downstream — the probe hash tuple, the cost model's build-side
choice, the cardinality estimator's NDV product — reads a join key by *name*
against a schema that, from three relations onward, can hold that name several
times.

Concretely: `laps` has a `team` column and `drivers` has a `team` column. In

```sql
SELECT COUNT(*) FROM laps l
  JOIN drivers d  ON l.driver_id = d.driver_id
  JOIN drivers d2 ON d.team = d2.team
```

the second join's `keys[0].from_col` is `"team"` with `from_slot = 1`
(`d`). Its left input is the first join, whose merged schema is
`[laps.*(slot 0), drivers.*(slot 1)]` — and `laps.team` comes **first**. A
bare-name `indexOf("team")` returns `laps.team`. The query then joins on the
wrong column, returns a plausible number of plausible rows, and nothing
anywhere errors. That is the worst failure mode this codebase has, and it is
one line away in three separate files.

Downstream: `VectorizedPlanBuilder`'s `int_keys` test and both join operators
(Task 2), `CardinalityEstimator`'s JOIN case (already slot-exact — Week 26 fixed
it, and it is the model to copy), `Planner::plan` (single join, so slot 0 always
— but write it slot-exact anyway so the two engines read the same way).

### Conceptual explanation

A join key is a *(relation, column)* pair, not a column name. Week 26 made the
binder stamp every `ColumnRef` with its range-table slot and made
`classifyJoinCondition` carry the left operand's slot into
`JoinKey::from_slot` — but nothing yet *consumes* `from_slot` outside the
estimator, because with one join the left input is one relation and the name is
unambiguous by construction.

The rule to internalize:

- **Left input (`children[0]`, the `from_*` side):** merged schema, several
  slots, possibly repeated names → resolve with
  `Schema::indexOf(name, from_slot)`. A miss is a **plan-time error**, never a
  fallback to the bare-name overload (that fallback is what re-introduces the
  bug it is guarding).
- **Right input (`children[1]`, the `join_*` side):** exactly one relation by
  identity (2), whose scan schema stamps every column slot 0 → the bare-name
  `indexOf(name)` is correct and stays. Do **not** "fix" it to
  `indexOf(name, join_slot)`: the *scan's own* schema keeps slot 0; only the
  *merged* schema carries `join_slot`. That asymmetry is deliberate and is what
  keeps `restampSlots(..., 0)` and `ChunkPruner`'s `relation_slot < 1`
  scan-local test correct at any relation count.

The de-duplication half is independent and small. `ON a.x = b.x AND a.x = b.x`
is a legal, if silly, predicate. It currently produces two identical `JoinKey`s.
As a *predicate* that is harmless (`k AND k ≡ k`), but as a *key list* it is
not: the probe tuple gets a redundant field and `CardinalityEstimator`'s
`divisor *= ndv` runs twice for the same column, underestimating the join by a
factor of NDV. Dedupe at the source — inside `classifyJoinCondition` — and both
symptoms disappear at once.

### Code

```cpp
// src/planner/join_condition.cc — at the end of classifyJoinCondition, before
// `return keys;`
//
// Identical keys are a legal predicate (k AND k == k) but not a legal key list:
// the probe tuple gains a redundant field, and CardinalityEstimator divides by
// the same NDV twice, underestimating the join by a factor of NDV. Dedupe here,
// at the single source, rather than at each of the three consumers. O(n^2) over
// a key list that is never longer than a handful — no set needed, and this
// preserves written order, which explain() prints.
    std::vector<JoinKey> deduped;
    for (const JoinKey& k : keys) {
        bool seen = false;
        for (const JoinKey& d : deduped) {
            if (d.from_col == k.from_col && d.join_col == k.join_col
                && d.from_slot == k.from_slot) { seen = true; break; }
        }
        if (!seen) deduped.push_back(k);
    }
    return deduped;
```

```cpp
// src/planner/vectorized_plan_builder.cc — new file-local helper, above Lowering
//
// Resolve one side's key columns to physical column indices.
//
// LEFT input: its merged schema can hold `team` at slot 0 AND slot 1 after two
// joins, so bare-name lookup is a coin flip that returns rows rather than an
// error. from_slot is the binder slot of the left operand and exists precisely
// for this. Resolve against the PHYSICAL child's schema (not the logical node's)
// so the indices are the ones the operator will actually index with — they are
// the same schema today, and asserting it here is cheaper than debugging the
// day they diverge.
std::vector<int> leftKeyIndices(const Schema& left_schema, const std::vector<JoinKey>& keys) {
    std::vector<int> idx;
    idx.reserve(keys.size());
    for (const JoinKey& k : keys) {
        // -1 slot = an unbound key from a validator-only caller; the real
        // pipeline always binds, so bare-name is that path's documented
        // fallback and NOT a fallback for a slot miss on a bound key.
        int i = (k.from_slot >= 0) ? left_schema.indexOf(k.from_col, k.from_slot)
                                   : left_schema.indexOf(k.from_col);
        if (i < 0) {
            throw std::runtime_error(
                "join key '" + k.from_col + "' (relation slot "
                + std::to_string(k.from_slot) + ") not found on the left join input");
        }
        idx.push_back(i);
    }
    return idx;
}

// RIGHT input: children[1] is always exactly one relation (left-deep), whose
// scan schema stamps every column slot 0 — so the bare-name overload is
// unambiguous here and is the right call. Revisit only if the tree ever becomes
// bushy; Week 28's DP keeps it left-deep.
std::vector<int> rightKeyIndices(const Schema& right_schema, const std::vector<JoinKey>& keys) {
    std::vector<int> idx;
    idx.reserve(keys.size());
    for (const JoinKey& k : keys) {
        int i = right_schema.indexOf(k.join_col);
        if (i < 0) throw std::runtime_error(
            "join key '" + k.join_col + "' not found on the joined relation");
        idx.push_back(i);
    }
    return idx;
}
```

The same bug lives in the builder's SIMD-eligibility test, which today reads:

```cpp
// BEFORE — bare-name lookup on a merged left schema: wrong column's type
bool int_keys =
    from_schema.column(from_schema.indexOf(from_col)).type == TypeId::INT &&
    jn_schema.column(jn_schema.indexOf(join_col)).type == TypeId::INT;

// AFTER — indices already resolved slot-exactly above; reuse them
bool int_keys =
    from_schema.column(left_idx[0]).type == TypeId::INT &&
    jn_schema.column(right_idx[0]).type == TypeId::INT;
```

### Implementation guidance

1. Add the dedupe to `classifyJoinCondition` first, with its unit test. It is
   self-contained and cannot break anything else.
2. Add the two index helpers to `vectorized_plan_builder.cc`. Do **not** call
   them yet — Task 2 changes the operator constructors that consume them. If you
   want the compiler's help, add them in Task 2's commit instead; they are
   listed here because they are Task 1's *idea*.
3. **Gotcha — do not add a bare-name fallback "just in case."** The tempting
   shape is `if (i < 0) i = schema.indexOf(name);`. That converts a loud plan
   error into the exact silent wrong-column bug this task exists to prevent.
   Week 26 hit this twice (see `git log`: *"fix(planner): match join-key
   statistics by slot, not by name"* and *"apply the slot-exact stats lookup to
   every slot-carrying caller"*). The estimator's comment spells out the
   principle: **a slot miss is "no statistic", not "try something else."**
4. **Gotcha — the right side is not symmetric with the left.** Resist making
   both sides slot-exact for tidiness. `children[1]`'s scan schema is slot 0;
   asking it for `join_slot` returns -1 and throws on every multi-way query.
5. Grep for remaining bare-name key lookups before declaring the task done:
   `grep -rn "indexOf(from_col\|indexOf(join_col\|indexOf(left_col_\|indexOf(right_col_\|indexOf(probe_join_col_\|indexOf(build_join_col_" src/`.

### Verification

```bash
cmake --build build -j$(nproc) && (cd build && ./tests/swiftql_tests)
```

- New unit test in `tests/test_binder.cc`, next to
  `JoinOnValidation.MultiKeyConditionYieldsOneKeyPerConjunct`:

  ```cpp
  TEST(JoinOnValidation, DuplicateKeysCollapseToOne) {
      Catalog cat(CATALOG);
      auto stmt = bindQuery(
          "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND a.id = b.id", cat);
      auto keys = classifyJoinCondition(stmt.joins[0].condition.get(), 1);
      EXPECT_EQ(keys.size(), 1u);   // was 2: a wider probe tuple and a squared NDV divisor
  }
  ```
  Assert on the *count*, not on "it didn't throw" — the pre-fix code also
  doesn't throw.
- A cardinality test in `tests/test_cardinality.cc` pinning that the duplicated
  form and the single-key form now estimate identically. This is the symptom
  that would otherwise only show up as a bad plan choice much later.
- The slot-exactness itself is not observable until Task 3 can execute a
  three-relation query. **Write the test now and let it fail-to-compile / be
  skipped, or write it in Task 3** — but do not consider Task 1 "verified" by
  the existing suite: nothing in it can distinguish the two lookups yet. That
  is exactly what makes this the dangerous task.

---

## Task 2 — Multi-key equi-join execution (both engines)

### Why it matters

TPC-H Q9 joins `partsupp` to `lineitem` on `(ps_partkey, ps_suppkey)`. A
single-key hash join cannot express that: joining on one column and filtering
the other above the join is *semantically* equivalent for an inner join, but it
builds a hash table with a far larger bucket population and materializes the
cross-product of every partial match — the difference between a plan that runs
and one that does not.

This is also the task that removes the *first* pair of guards, so it carries the
mode-divergence risk described above. Both engines gain multi-key in the same
commit; neither gains multi-way here.

Downstream: `VecHashJoinNode`, `VecSimdLoopJoinNode` (which must *decline*
multi-key), `HashJoinNode`, `VectorizedPlanBuilder`, `Planner::plan`, and the
`--explain` strings of all three operators.

### Conceptual explanation

A hash join on `k` keys is a hash join on one **composite** key. Both operators
already serialize the single key to a `std::string` with a `'\x01'` sentinel
appended; the generalization is to append each key's serialization in the same
fixed order on both sides:

```
key(row) = toString(v1) '\x01' toString(v2) '\x01' ... toString(vk) '\x01'
```

The sentinel is not decoration. Without it, `("ab", "c")` and `("a", "bc")`
serialize to the same bytes and land in the same bucket — a wrong answer that
only appears on STRING keys with adjacent boundaries. The existing code already
appends it after the single key, so the generalization is *literally* moving the
append inside a loop. Keep it that way rather than inventing a new encoding.

**NULL.** SQL says `NULL = NULL` is unknown, so a row whose key is NULL matches
nothing. With `k` keys the rule composes: if **any** key column is NULL the row
can never match, so it is dropped from the build side and skipped on the probe
side. `VecHashJoinNode` already does this for one key (`if (key_val.isNull())
continue;`). Volcano's `HashJoinNode` does **not** — it buckets NULL under
`toString()`'s `"NULL"`, which makes `NULL = NULL` match. That divergence is
unreachable today (`ColumnarTable` and CSV cannot express NULL, and a Volcano
join key comes straight off a scan), so it has never produced a wrong answer —
but you are editing that exact line, and Week 29's outer join will put real
NULLs on join inputs. Fix it while you are there and say so in the commit
message; it is two lines and it removes a latent engine divergence.

**Key ordering.** The `k`-tuple's order is `keys`' order, which is the written
order of the `ON` conjuncts, which `classifyJoinCondition` preserves. Both sides
derive their index vector from the *same* `keys` vector in the *same* loop, so
they cannot disagree — provided you never sort or re-order `keys`. Don't.

**SIMD.** `VecSimdLoopJoinNode` holds one flat `vector<int64_t>` of build keys
and compares them with one SIMD instruction stream. A composite key does not fit
that representation, and inventing one (hashing to 64 bits, then re-checking) is
a Week 37 optimization with a correctness trap in it. Gate SIMD eligibility on
`keys.size() == 1` and let multi-key fall back to the hash join. The cost model
needs no change: an ineligible algorithm is simply not costed.

### Code

```cpp
// src/execution/vec_hash_join_node.h — the constructor takes resolved column
// INDICES now, not names. Two reasons: the left side's name is ambiguous on a
// merged schema (Task 1) and only the builder knows the slots; and the probe
// path stops calling indexOf() once per chunk.
    VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child,
                    std::unique_ptr<VecPlanNode> build_child,
                    std::vector<int> probe_key_indices,
                    std::vector<int> build_key_indices,
                    Schema output_schema, bool swapped = false);
  private:
    std::vector<int> probe_key_idx_;
    std::vector<int> build_key_idx_;
```

```cpp
// src/execution/vec_hash_join_node.cc — one composite key, one string.
//
// The '\x01' sentinel after EVERY field is what stops ("ab","c") and ("a","bc")
// from colliding; it is the same sentinel the single-key form appended, now
// inside the loop. `out` is a caller-owned scratch buffer reused across rows so
// the probe loop does not allocate per row.
//
// Returns false when any key column is NULL: SQL's NULL never equals anything,
// so such a row can neither be inserted into the build table nor match a probe.
static bool serializeKey(const DataChunk& chunk, const std::vector<int>& key_idx,
                         int r, std::string& out) {
    out.clear();
    for (int c : key_idx) {
        Value v = valueAt(chunk.columns[c], r);   // never read .data directly: bypasses validity
        if (v.isNull()) return false;
        out += v.toString();
        out += '\x01';
    }
    return true;
}

// build phase (open()), replacing the two lines that extracted one key:
    std::string key;                                  // reused across rows
    for (int r : *indices_ptr) {
        if (!serializeKey(*chunk, build_key_idx_, r, key)) continue;   // NULL key: matches nothing
        Row build_row;
        build_row.reserve(chunk->columns.size());
        for (const auto& cv : chunk->columns) build_row.push_back(valueAt(cv, r));
        hash_table_[key].push_back(std::move(build_row));
    }
```

```cpp
// src/execution/vec_hash_join_node.cc — explain() renders names from the child
// schemas, so --explain still prints columns and not integers. Single-key
// output stays byte-identical to the pre-Week-27 string, which is what keeps
// the existing --explain assertions passing.
std::string VecHashJoinNode::explain() const {
    std::string s = "VecHashJoin [";
    for (size_t i = 0; i < probe_key_idx_.size(); ++i) {
        if (i) s += " AND ";
        s += probe_child_->outputSchema().column(probe_key_idx_[i]).name + " = "
           + build_child_->outputSchema().column(build_key_idx_[i]).name;
    }
    s += "] (materialize)";
    if (!cost_decision_.empty()) s += " " + cost_decision_;
    return s;
}
```

```cpp
// src/planner/vectorized_plan_builder.cc — the JOIN case. Resolve once, then
// hand the SAME vectors to whichever (side, algorithm) the cost model picks;
// `swapped` only decides which vector is "probe" and which is "build".
    std::vector<int> left_idx  = leftKeyIndices(join->children[0]->output_schema, join->keys);
    std::vector<int> right_idx = rightKeyIndices(join->children[1]->output_schema, join->keys);

    // SIMD holds build keys in one flat int64 buffer, which a composite key
    // cannot occupy. Decline rather than invent an encoding: an ineligible
    // algorithm is simply not selected, and the hash join is always correct.
    bool int_keys = join->keys.size() == 1
        && from_schema.column(left_idx[0]).type == TypeId::INT
        && jn_schema.column(right_idx[0]).type == TypeId::INT;
    ...
    std::unique_ptr<VecHashJoinNode> join_node = from_builds
        ? std::make_unique<VecHashJoinNode>(std::move(join_child), std::move(from_child),
                                            right_idx, left_idx, join->output_schema, true)
        : std::make_unique<VecHashJoinNode>(std::move(from_child), std::move(join_child),
                                            left_idx, right_idx, join->output_schema, false);
```

```cpp
// src/planner/planner.cc — the Volcano side. HashJoinNode keeps NAMES (both its
// children are single-relation scans, so a bare-name lookup is unambiguous
// there), but they become vectors. The deferred multi-key flag and its throw at
// the bottom of plan() are deleted in this same commit.
        std::vector<std::string> from_cols, join_cols;
        for (const JoinKey& k : keys) { from_cols.push_back(k.from_col);
                                        join_cols.push_back(k.join_col); }
        // (the `multi_key_join = keys.size() != 1;` line and the throw at the
        //  end of plan() both go away here)
```

### Implementation guidance

1. **Order: operators, then builders, then guards.** Change
   `VecHashJoinNode` + `HashJoinNode` first and let the compiler point you at
   every construction site. Nothing outside `src/planner/` constructs these
   nodes — verified: `grep -rn "VecHashJoinNode(\|HashJoinNode(" tests/` is
   empty — so the blast radius is two builder files.
2. Hoist the probe-side `indexOf` out of `HashJoinNode::next()` while you are
   there: it currently re-resolves the probe key on **every row**. With a vector
   that becomes k lookups per row. Resolve in `open()` into a member.
3. Delete `checkKeyCounts` and its call from `checkLowerable`, **and** the
   `multi_key_join` flag plus its throw in `Planner::plan`, **in this commit**.
   Leave `checkLowerable`'s join-count refusal alone — with only one check left
   in it, the "join count first" ordering it documents becomes trivially true,
   so simplify the comment rather than deleting it.
4. **Gotcha — `swapped` and the key vectors must swap together.** The
   `from_builds` branch already swaps the children and the names; it now swaps
   two *vectors*. Passing `left_idx` as the probe vector while passing
   `join_child` as the probe child is a silent wrong answer, not a crash,
   because both vectors are usually the same length.
5. **Gotcha — do not sort or canonicalize `keys`.** Both sides index the same
   vector positionally. Any reordering that touches one side and not the other
   pairs `a.x` with `b.y`.
6. **Gotcha — the tests that pin the current refusal must be *migrated*, not
   deleted.** `VecPlanBuilder.MultiKeyJoinRefusedUntilWeek27` and
   `PlannerTest.MultiKeyJoinRefusedUntilWeek27` become
   `...MultiKeyJoinLowersAndExecutes` tests asserting rows, not messages. A
   deleted test is a lost invariant; a migrated one keeps the query in the
   suite. Also update `PlannerTest.TypeErrorBeatsTheMultiKeyRefusal` and
   `SelectListTypeErrorBeatsTheMultiKeyRefusal` — the *type errors* they assert
   must still fire (that property survives the week), but their comments about
   "beating the multi-key refusal" no longer describe why.

### Verification

Success criteria, in order:

1. **Both engines agree, and both agree with SQLite.**
   ```bash
   for m in "" "--storage columnar" "--execution vectorized --storage columnar" \
            "--execution vectorized --storage columnar --no-optimize"; do
     ./build/swiftql --catalog catalog.json --no-cache $m --query \
       "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id AND l.team = d.team"
   done
   ```
   All four must print the same count. Then check it against SQLite via the
   harness entry you add in Task 5 — or by hand:
   `sqlite3` over the same CSVs with the identical query.
2. **The multi-key answer must differ from the single-key one** on this dataset
   (`... ON l.driver_id = d.driver_id` alone returns more rows). If they match,
   your test is not testing anything — pick columns that actually discriminate.
3. **`--explain` prints both keys:**
   `VecHashJoin [driver_id = driver_id AND team = team]`.
4. **A single-key join's `--explain` output is byte-identical to before.** Run
   `git stash` / compare if unsure. Existing assertions in
   `tests/test_vec_plan_builder.cc` depend on it.
5. `(cd build && ./tests/swiftql_tests)` — **567 tests** is the pre-Week-27
   baseline (note `development.md` still says 524; that number is stale and Task
   5 corrects it).
6. Use the `operator-correctness` skill's hash-join checklist to hand-simulate a
   3-row × 3-row multi-key join including a duplicate key on the build side —
   bucket-with-multiple-rows is the case the composite key most easily breaks.

---

## Task 3 — Multi-way lowering on the vectorized path

### Why it matters

This is the checkpoint itself. It is also, pleasingly, the **smallest** task in
the week — because `Lowering::lowerNode`'s JOIN case already recurses through
`lower(join->children[0])`, and `children[0]` being another `LogicalJoin` is
just another recursion. The Week 26 implementer built the logical layer so that
lowering N joins needs no new lowering code.

What *does* need work is everything around the recursion that quietly assumed
"a join input is a scan of one table": the cost model's row-width lookup, the
build-side label in `--explain`, and the refusal itself.

Downstream: `--explain` starts working for multi-way (for free); `EXPLAIN
ANALYZE` gains a second join node; the cost model starts making a build-side
decision for a join-shaped input for the first time.

### Conceptual explanation

**The pipeline shape.** A three-relation left-deep tree lowers to

```
VecHashJoin        (upper: probes/builds relation 2 against the join below)
  VecHashJoin      (lower: relations 0 and 1)
    VecScan/VecFilter [relation 0]
    VecScan/VecFilter [relation 1]
  VecScan/VecFilter   [relation 2]
```

Both hash joins are **pipeline breakers on their build side**: `open()` drains
the build child completely before any probe row flows. When the upper join
chooses the *lower join* as its build side, it materializes the entire
intermediate result into `hash_table_` as `Row`s. That is correct and it is what
the memory term of `hashJoinCost` is for — but it means the cost model's inputs
must be honest, which is the next paragraph.

**Where "one join input == one table" is baked in.** Two helpers in
`vectorized_plan_builder.cc` walk `children[0]` down to a leaf scan:

- `leafScanTable(node)` — walks `children[0]` until it hits a SCAN. Given a
  join-shaped input it returns *relation 0's* table name. Used for (a) the
  `--no-optimize` raw-row-count fallback and (b) the `build=<table>` label.
- `rowWidth(child, catalog)` — takes that table name, looks up its
  `TableStats`, and sums `avg_width` over **the child's output columns**. For a
  merged schema those columns come from several tables, so every column not
  present in relation 0's stats silently falls back to `8.0` — and worse, a
  column that *is* present by name in relation 0 but actually belongs to
  relation 1 (`team`, again) contributes the **wrong table's** width. The
  numbers are wrong in a plausible direction, which is the hardest kind to
  notice.

Neither is a correctness bug — they feed cost estimates, not results. But a
silently wrong cost input is exactly what makes Week 28's join enumeration
untrustworthy, and the fix is four lines. Make the helper *refuse to guess*: if
the input spans more than one relation, fall back to the documented uniform
`8 bytes/column` proxy, which is the same fallback the stats-less path already
uses.

**The `--no-optimize` fallback.** Under `--no-optimize`, `estimated_rows` is -1
everywhere and the builder falls back to `tables.at(leafScanTable(child)).num_rows`.
For a join-shaped child that is relation 0's *table* size — an underestimate of
the intermediate result, but a deterministic one, and `--no-optimize` is defined
as "the pre-Week-22 heuristic" rather than "a good plan." Leave it. Do not
sneak an estimate into the unoptimized path; the benchmark baseline depends on
it not moving.

**What must not change.** The pruning hint (`pruning_where`) still routes to
`children[0]` only, which sends the top-level `WHERE` down the left spine to
relation 0's scan. That stays safe at any relation count because `ChunkPruner`
ignores any `ColumnRef` with `relation_slot >= 1` (`chunk_pruner.h`, the
`col->relation_slot < 1` test), so a hint mentioning relations 1 and 2 simply
contributes no pruning rather than pruning the wrong table. Do not "improve"
this by routing hints to both sides.

### Code

```cpp
// src/planner/vectorized_plan_builder.cc
//
// True when this join input is exactly one relation: a scan, possibly under
// filters. A join-shaped input's merged schema spans several tables, so neither
// leafScanTable() nor a single TableStats can describe it, and the per-column
// avg_width lookup below would silently attribute one table's widths to
// another's columns whenever a name is shared (laps.team / drivers.team).
bool isSingleRelation(const LogicalPlanNode* node) {
    while (node->type != LogicalNodeType::SCAN) {
        if (node->type == LogicalNodeType::JOIN) return false;
        node = node->children[0].get();
    }
    return true;
}

double rowWidth(const LogicalPlanNode* child, const Catalog& catalog) {
    // Multi-relation input: no single stats entry describes it. Fall back to
    // the documented 8-bytes/column proxy — the same fallback a stats-less
    // table already gets — rather than reporting one relation's widths for
    // another relation's columns. A real per-relation width sum belongs with
    // Week 28's join enumeration, where intermediate widths first change a plan.
    if (!isSingleRelation(child)) return child->output_schema.size() * 8.0;
    const std::string& table = leafScanTable(child);
    if (!catalog.hasStats(table)) return child->output_schema.size() * 8.0;
    ...
}

// build=<label> in the cost decision string: naming relation 0 for a whole join
// subtree claims a build side that isn't the one chosen.
std::string sideLabel(const LogicalPlanNode* n) {
    return isSingleRelation(n) ? leafScanTable(n) : "join-subtree";
}
```

```cpp
// src/planner/vectorized_plan_builder.cc — the refusal, after Task 2 removed
// the key-count half. DELETE countJoins, checkLowerable and the call in build().
//
// -void checkLowerable(const LogicalPlanNode* root) { ... }
//  ...
//  std::unique_ptr<VecPlanNode> VectorizedPlanBuilder::build(...) {
// -    checkLowerable(logical.get());
//      Lowering lowering{columnar_tables, {}, catalog};
```

```cpp
// src/planner/planner.cc — Volcano KEEPS its refusal; only the message changes.
// Deleting it and letting HashJoinNode run would build one join out of two
// clauses and silently drop a relation — the wrong-answer outcome every refusal
// in this codebase exists to prevent. Row/Volcano is the correctness baseline,
// not the feature-complete path; multi-way execution is deliberately
// vectorized-only from Week 27 on.
    if (stmt.joins.size() > 1) {
        throw std::runtime_error(
            "multi-way joins are not supported on the Volcano path; "
            "use --execution vectorized");
    }
```

### Implementation guidance

1. Do the `isSingleRelation` / `rowWidth` / `sideLabel` work **first**, while the
   refusal is still up. It is unobservable until the refusal drops, so landing
   it first means the first multi-way query you ever run already has honest cost
   inputs.
2. Then delete `checkLowerable` and narrow the Volcano message, in one commit.
3. **Gotcha — self-joins and `scan_uses`.** Three scans over two tables is the
   normal shape on this catalog (`laps`, `drivers`, `drivers d2`). `countScans`
   already counts per *table*, and `lowerNode`'s SCAN case copies the table for
   every use but the last. That works at any count — but it means a 3-relation
   query over 2 tables **copies a whole `ColumnarTable`**. Expected, not a leak.
4. **Gotcha — do not reorder `main.cc` to fix `--explain`.** The Starting notes
   say lifting the guard fixes multi-way `--explain` for free, and it does:
   `main.cc` captures the logical section into `logical_lines` *before*
   lowering, and the only reason nothing printed was that lowering threw before
   the print. Verify it rather than restructuring it.
5. **Gotcha — the build side of the upper join may be the lower join.** If your
   3-relation query hangs or eats memory, that is the cost model choosing to
   materialize the intermediate result, not a bug. Check with `--explain` and,
   if the estimate looks wrong, with `--explain-analyze`'s est-vs-actual.
6. Run the `invariants` skill's checklist before committing: join schema
   ordering (`[relation 0, 1, 2]` left to right, independent of build/probe) and
   `relation_slot` resolution are the two invariants a third relation is most
   likely to break.

### Verification

The checkpoint itself. All of these must hold:

```bash
# 1. Three relations execute on the vec path
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized --no-cache \
  --query "SELECT l.team, d.name, d2.name FROM laps l \
           JOIN drivers d  ON l.driver_id = d.driver_id \
           JOIN drivers d2 ON d.team = d2.team \
           WHERE l.lap_id < 20 ORDER BY l.lap_id, d.name, d2.name"

# 2. THE slot-ambiguity test from Task 1 — laps.team and drivers.team both live
#    in the left merged schema, and the third join's key has from_slot 1.
#    A bare-name lookup joins on laps.team and returns a WRONG count with no error.
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized --no-cache \
  --query "SELECT COUNT(*) AS c FROM laps l \
           JOIN drivers d  ON l.driver_id = d.driver_id \
           JOIN drivers d2 ON d.team = d2.team"
#    Compare against SQLite. This single query is the highest-value check of the week.

# 3. --explain now prints all three sections, with two join nodes
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized --explain \
  --query "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id \
           JOIN drivers d2 ON d.team = d2.team"

# 4. Volcano refuses, and says which path to use
./build/swiftql --catalog catalog.json \
  --query "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id \
           JOIN drivers d2 ON d.team = d2.team"
# expected: multi-way joins are not supported on the Volcano path; use --execution vectorized

# 5. Optimized and unoptimized agree
./build/swiftql ... --execution vectorized --storage columnar --no-optimize --query "<query 2>"
```

- **Four relations must also work** — nothing in the design is 3-specific, and a
  4-relation test is what proves the recursion rather than a special case. Add
  one over `sj` in `tests/test_vec_plan_builder.cc` (the test catalog's `sj`
  table exists for exactly this: `a JOIN b JOIN c JOIN d` on `id`/`grp`).
- Migrate `VecPlanBuilder.ThreeWayJoinRefusedUntilWeek27` and
  `VecPlanBuilder.MultiWayAndMultiKeyReportsTheJoinCountFirst` into positive
  tests (`ThreeWayJoinLowersToTwoHashJoins`,
  `MultiWayAndMultiKeyLowersToTwoMultiKeyJoins`). Keep
  `PlannerTest.ThreeWayJoinRefusedUntilWeek27` as a refusal test with the new
  message, and rename it (`ThreeWayJoinRefusedOnVolcano`) so the name stops
  claiming it is temporary.
- `PlannerTest.MultiWayRefusalStillPrecedesTypeChecks` **stays and still
  passes** — Volcano's early multi-way refusal is unchanged in position, only in
  wording. Update the expected substring.

---

## Task 4 — Non-equality `ON` conjuncts as post-join residuals

### Why it matters

TPC-H Q21's self-join uses `l1.l_suppkey <> l2.l_suppkey`; Q19's structure and
several others attach conditions to a join that are not equi-join keys. Today
`classifyJoinCondition` throws on any of them, which means those queries cannot
be *written*, let alone run. This is the last SQL-surface item in the Week 27
bullets and the one that turns dispatch site 18 from a pre-position into a live
check.

It also removes three rejections that were never really about the engine's
capability, only about the key extractor's narrowness: `ON a.x = 5` (an equality
with a literal), `ON a.x = a.y` (both operands from one relation), and
`ON a.x = b.x AND b.val IN (1,2)` (a Week 25 node in an `ON`). SQLite accepts all
three; after this task so does SwiftQL, and they execute as filters.

Downstream: `classifyJoinCondition`'s signature (so: `Validator::validate`,
`LogicalPlanBuilder::build`, `Planner::plan`, and every test that calls it),
`PredicatePushdown` (which then routes the residuals for free), and site 18.

### Conceptual explanation

**The semantic fact that makes this cheap:** for an **inner** join,
`R ⋈_(p ∧ q) S ≡ σ_q(R ⋈_p S)` for any `q`. `ON` and `WHERE` are
interchangeable. So a conjunct of the `ON` clause that is not an equi-join key
does not need a new plan node, a new operator, or a new position — it needs to
be handed to the machinery that already places `WHERE` conjuncts, which since
Week 26 routes each conjunct to the relation slot that owns it
(`soleSlot()` / `distribute()`).

**That equivalence is inner-join-only, and Week 29 breaks it.** For a left outer
join, an `ON` predicate filters the *match test* (unmatched left rows survive
with NULLs) while a `WHERE` predicate filters the *result* (unmatched rows are
discarded). Merging the two is therefore a decision that must be revisited the
moment `LogicalJoin` grows a join type. Write that in the code comment, not just
here — `predicate_pushdown.h` already carries the identical warning for
pushdown, and this is the second instance of the same trap.

**The new classification rule.** Per conjunct of the flattened `AND` chain, in
this order:

1. **Any referenced slot `> right_slot` → error, unchanged.** A forward
   reference (`ON a.x = c.x` while joining `b`) names columns that are not in
   this join's output schema at all, so it cannot be a residual either. Week 26
   built this check deliberately; generalize it from "the equality's operands"
   to "every `ColumnRef` in the conjunct," because a residual can be any shape.
2. **`=` between two `ColumnRef`s, one at `right_slot`, the other below it →
   an equi-join key.** Unchanged.
3. **Everything else → a residual.** Non-equality operators, `OR`, equality with
   a literal or a computed operand, both operands in one relation, Week 25
   nodes, `IS NULL`.

Then one new global check: **at least one key must survive.** SwiftQL has no
cross-product join operator, and a `JOIN` whose `ON` yields zero keys is a
cross product with a filter on top. Refuse it — `ON a.x = b.x OR a.y = b.y` is
the shape that lands here, and it must keep failing (with a new message).

**Ownership.** `classifyJoinCondition` takes a `const Expr*` and does not own
anything; the `ON` trees belong to the `SelectStatement`, which
`LogicalPlanBuilder::build` takes **by value** and destroys on return. A
`LogicalFilter` needs an owning `unique_ptr<Expr>`. So residuals are returned as
borrowed `const Expr*` and the **caller clones** them with `cloneExpr`
(dispatch site 11 — it throws on an unknown subtype, which is why it is the safe
choice). One clone per residual conjunct at plan time is nothing.

**Where the residuals go.** Conjoin them into `stmt.where` *before* the `WHERE`
filter is built. Do **not** create a second `LogicalFilter` above the join:
`PredicatePushdown::apply` only rewrites a FILTER whose direct child is a JOIN,
so a `Filter(WHERE) → Filter(ON-residual) → Join` stack would leave the `WHERE`
unpushed — a silent, across-the-board pushdown regression. One filter, one
push, everything routed by `soleSlot()`.

### Code

```cpp
// src/planner/join_condition.h
//
// Result of decomposing an ON clause. Week 27: conjuncts that are not equi-join
// keys are no longer rejected — for an INNER join, ON and WHERE are
// interchangeable (R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S)), so a residual is handed to the
// same predicate-assignment machinery that places WHERE conjuncts.
//
// !! Week 29 (outer join) must revisit this. For a LEFT OUTER join an ON
// predicate filters the match test while a WHERE predicate filters the result,
// and merging them changes the answer. Same trap predicate_pushdown.h documents.
//
// `residuals` are BORROWED pointers into the statement's ON trees. The caller
// clones what it needs (cloneExpr) — the statement outlives neither the logical
// plan nor Planner::plan's node tree.
struct JoinCondition {
    std::vector<JoinKey> keys;
    std::vector<const Expr*> residuals;
};

JoinCondition classifyJoinCondition(const Expr* condition, int right_slot);
```

```cpp
// src/planner/join_condition.cc — the per-conjunct decision, replacing the
// throw-heavy cascade.
    for (const Expr* c : conjuncts) {
        // (1) Forward reference: names a relation joined LATER, whose columns
        // are absent from this join's output schema, so it cannot even be a
        // residual. Generalized from the equality's two operands to every ref in
        // the conjunct, because a residual can be any expression shape.
        std::unordered_set<int> slots;
        collectSlots(c, slots);                 // shared walker, dispatch site 8
        for (int s : slots) {
            if (s > right_slot) throw std::runtime_error(
                "JOIN ON: a condition may only reference the table being joined "
                "and tables already joined");
        }

        // (2) An equi-join key: '=' between two ColumnRefs, one of them the
        // relation being joined, the other already in the left tree.
        auto* bin = dynamic_cast<const BinaryExpr*>(c);
        const ColumnRef* lc = bin ? dynamic_cast<const ColumnRef*>(bin->left.get())  : nullptr;
        const ColumnRef* rc = bin ? dynamic_cast<const ColumnRef*>(bin->right.get()) : nullptr;
        if (bin && bin->op == "=" && lc && rc && lc->relation_slot != rc->relation_slot) {
            if (rc->relation_slot == right_slot) { out.keys.push_back({lc->column_name, rc->column_name, lc->relation_slot}); continue; }
            if (lc->relation_slot == right_slot) { out.keys.push_back({rc->column_name, lc->column_name, rc->relation_slot}); continue; }
            // neither side is the relation being joined: a condition between two
            // ALREADY-joined relations. Legal as a residual above this join.
        }

        // (3) everything else — non-equality, OR, literal operand, same-relation
        // equality, Week 25 nodes — is a residual predicate, not a key.
        out.residuals.push_back(c);
    }

    // SwiftQL has no cross-product operator: a JOIN whose ON yields no key is a
    // cartesian product with a filter on top. `ON a.x = b.x OR a.y = b.y` is the
    // shape that lands here (an OR is one indivisible conjunct).
    if (out.keys.empty()) {
        throw std::runtime_error(
            "JOIN ON: at least one equality between the joined table and an "
            "already-joined table is required");
    }
```

```cpp
// src/planner/logical_plan.cc — LogicalPlanBuilder::build, inside the joins fold
    std::vector<std::unique_ptr<Expr>> on_residuals;   // declared above the loop
    ...
        JoinCondition jc_parts = classifyJoinCondition(jc.condition.get(), join_slot);
        for (const Expr* r : jc_parts.residuals) on_residuals.push_back(cloneExpr(r));
    ...
// after the fold, BEFORE the `if (stmt.where)` block:
    //
    // Inner join: ON and WHERE are interchangeable, so residual ON conjuncts
    // join the WHERE conjunction rather than getting their own LogicalFilter.
    // That is not cosmetic — PredicatePushdown only rewrites a FILTER whose
    // DIRECT child is a JOIN, so a second stacked filter would leave the WHERE
    // unpushed and silently cost every joined query its pushdown.
    if (!on_residuals.empty()) {
        if (stmt.where) on_residuals.push_back(std::move(stmt.where));
        stmt.where = conjoinAll(std::move(on_residuals));
    }
```

```cpp
// src/parser/expr_utils.h — one shared AND-chain builder, next to cloneExpr.
// predicate_pushdown.cc's file-local `conjoin` becomes a call to this (delete
// the duplicate); logical_plan.cc and planner.cc are the new callers.
inline std::unique_ptr<Expr> conjoinAll(std::vector<std::unique_ptr<Expr>> parts) {
    if (parts.empty()) return nullptr;
    std::unique_ptr<Expr> acc = std::move(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        auto conj = std::make_unique<BinaryExpr>();
        conj->op = "AND";
        conj->left = std::move(acc);
        conj->right = std::move(parts[i]);
        acc = std::move(conj);
    }
    return acc;
}
```

`Planner::plan` (Volcano) needs the same three lines, placed after the join
block and before its `if (stmt.where)`. Volcano gets residuals for its single
join, so the two engines accept exactly the same `ON` clauses — only the
*relation count* differs between them.

### Implementation guidance

1. **Expose `collectSlots` rather than writing a second slot walker.** It is
   currently file-local in `predicate_pushdown.cc`. Declare it in
   `predicate_pushdown.h` and include that from `join_condition.cc`. Writing a
   private copy in `join_condition.cc` would create an **11th silent dispatch
   site**: a future `Expr` subtype missed there makes a forward reference
   invisible, which is a wrong answer. Update `development.md`'s site-8 row to
   say it now has two callers.
2. **Order the checks as written.** Forward reference first (it is the only
   error), then key, then residual. Reversing the first two turns
   `ON a.x = c.x` into a residual referencing a column the schema does not have,
   and the error becomes `column not found` from `inferExprType` — far from the
   cause.
3. **Gotcha — `Planner::plan` passes `stmt.where.get()` to the FROM `SeqScanNode`
   at line ~51, *before* the join block runs.** Conjoining residuals afterwards
   replaces `stmt.where` with a new `BinaryExpr` whose left child is the *moved*
   original. Moving a `unique_ptr` does not relocate the pointee, so the scan's
   raw pointer stays valid and still points at the original `WHERE` subtree —
   the same aliasing argument the vectorized builder's pruning hint relies on.
   It is safe **only** if you *move* the old `where` into the chain. If you ever
   rebuild it by cloning, that pointer dangles. Add the comment.
4. **Gotcha — residual conjuncts are validated by site 18, and by nothing
   else.** `Validator::validate` runs at the *top* of both builders, before the
   residuals are conjoined into `stmt.where`, so they never see
   `validateExpr`. That is correct and intended — `validateJoinCondition` is the
   `ON` clause's column check — but it means a gap in site 18 now produces a
   plan-time `column not found` from `inferExprType` instead of a clean
   `JOIN ON: column 'x' not found in table 'd'`. Test it.
5. **Gotcha — keep site 18's slot check slot-based.** The Starting notes are
   emphatic: never match a bound ref on `table_name`; the Binder rewrites an
   unqualified ref's `table_name` to its relation's table name, so a name match
   lands on whichever relation is aliased to that name. Week 26 shipped a bug
   here and fixed it; don't reintroduce it while adding residual support. **No
   code change is required in `validateJoinCondition` this week** — only tests.
6. **Gotcha — the `AggregateExpr` branch changes which function throws.**
   `ON a.id = SUM(b.grp)` used to fail in `classifyJoinCondition` ("both sides
   must be column references"); now it becomes a residual and fails in
   `validateJoinCondition` ("aggregate functions are not allowed in a join
   condition"). Both are correct and the ordering still puts a real error first
   (`Validator::validate` calls `classifyJoinCondition` then
   `validateJoinCondition`). Update
   `JoinOnValidation.AggregateInsideJoinConditionRejectedOnShape` — including
   its name, which now misdescribes the mechanism.
7. Tests that must be **migrated from rejection to acceptance**:
   `JoinOnValidation.NonEqualityOperatorRejected`, `NotEqualOperatorRejected`,
   `MixedCompoundWithNonEqualityRejected`, `LiteralOperandRejected`,
   `SameRelationBothSidesRejected`, `Week25NodeInsideOnRejectedOnShape`. Each
   becomes "…accepted as a residual" plus an assertion on where the conjunct
   ended up. `OrConditionRejected` **stays a rejection** with the new
   "at least one equality" message.

### Verification

```bash
# residual non-equality conjunct — the Q21 shape
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized --no-cache \
  --query "SELECT COUNT(*) AS c FROM laps l JOIN drivers d \
           ON l.driver_id = d.driver_id AND l.speed > d.age"

# residual that is a single-relation predicate: --explain must show it PUSHED
# below the join, onto the drivers scan — that is predicate assignment working
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized --explain \
  --query "SELECT l.team FROM laps l JOIN drivers d \
           ON l.driver_id = d.driver_id AND d.nationality = 'British'"
# expect a LogicalFilter [nationality = British] directly above LogicalScan [drivers]
# in the Optimized Logical Plan section, NOT above the join.

# the same query with --no-optimize must return the SAME ROWS with the filter
# left above the join (pushdown is an optimization, never a correctness input)
```

- Every one of these must match SQLite, which accepts all of them verbatim.
- A **cross-relation** residual (`l.speed > d.age`) must stay above the join —
  `soleSlot()` returns -1 for it. Assert the plan shape, not just the rows.
- The zero-key case must still fail:
  `ON l.driver_id = d.driver_id OR l.team = d.team` →
  `at least one equality`.
- Site 18 must be the thing that catches a bad column in a residual:
  `ON l.driver_id = d.driver_id AND d.nope > 1` →
  `JOIN ON: column 'nope' not found in table 'd'`, **not** `column not found`
  from `inferExprType`. Assert the message; a test that only asserts "it threw"
  passes for the wrong reason (this is the `development.md` §3 rule, and
  `Validation.UngroupedColumnInsideWeek25NodesIsRejected` is the cautionary
  example).
- Run the `optimizer-diff` skill on a residual query: optimized vs
  `--no-optimize` must differ in plan shape and agree exactly in rows.

---

## Task 5 — Tests, correctness harness, and documentation

### Why it matters

The harness is the only thing in this project that has ever caught cross-mode
divergence, and Week 27 introduces the project's **first deliberate capability
difference between execution modes**. Its current structure — "every query runs
in all four modes" — cannot express that, so it will report a multi-way query as
four failures instead of two passes and two expected refusals. Fixing the
harness is part of the feature, not paperwork.

### Conceptual explanation

`compare_against_sqlite.py` today runs one `QUERIES` list through four modes
(112 × 4 = 448) plus one `WEEK26_REJECTED_QUERIES` list through the same four
(15 × 4 = 60), for a total of 508. Week 27 needs three changes:

1. Queries that are now **executable in every mode** (multi-key, residual `ON`)
   move from `WEEK26_REJECTED_QUERIES` into `QUERIES`, where they are diffed
   against SQLite.
2. Queries that are **executable only on the vectorized path** (three-or-more
   relations) need a new list run through the *two vectorized* modes and diffed
   against SQLite.
3. Those same queries need a **Volcano rejection list** run through the *two
   Volcano* modes, asserting the narrowed message. Without it, "Volcano still
   refuses, and says why" is untested and a future refactor deletes it silently.

The rejection-suite plumbing already supports this: `run_rejection_suite(queries,
label, extra_args)` and `run_query_suite(conn, queries, label, extra_args)` both
take a per-mode `extra_args`, so the change is list membership and two loops,
not new machinery.

### Code

```python
# python_tools/compare_against_sqlite.py

# Week 27 — multi-way join execution. Three or more relations execute on the
# VECTORIZED path only: Planner::plan builds exactly one join and row/Volcano
# never gains multi-way execution (README, Week 27). This is the project's first
# deliberate per-mode capability difference, so it needs both halves — the rows
# where it runs, and the refusal where it does not.
WEEK27_VEC_ONLY_QUERIES = [
    # THE slot-ambiguity query: the third join's key is `team` at slot 1, and
    # `laps.team` sits earlier in the merged left schema. A bare-name lookup
    # joins on the wrong column and returns a plausible wrong count.
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team",
    "SELECT l.team AS t, d.name AS n, d2.name AS n2 FROM laps l "
    "JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team WHERE l.lap_id < 20 ORDER BY t, n, n2",
    # a residual ON conjunct in a multi-way tree, plus a pushed local predicate
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team AND d2.age > 25 WHERE l.season = 2024",
]

WEEK27_VOLCANO_REJECTED = [
    (q, "not supported on the Volcano path") for q in WEEK27_VEC_ONLY_QUERIES
]

# main(): two more suite runs on the vec modes, two more rejection runs on the
# Volcano modes, folded into the same totals.
```

### Implementation guidance

1. **Unit tests, per site.** The silent sites cannot be caught by eye:
   - `tests/test_binder.cc` — key dedupe; residual/key split; forward reference
     for a non-equality conjunct; zero-key refusal; the six migrated
     `JoinOnValidation` rejections; site 18 catching a bad column in a residual.
   - `tests/test_logical_plan.cc` — a 3-relation tree's shape and merged-schema
     slot stamps; residual conjuncts landing in the WHERE filter;
     `LogicalJoin::explain()` for two keys (already covered — keep it green).
   - `tests/test_predicate_pushdown.cc` — a residual `ON` conjunct pushed to its
     own scan; a cross-relation residual staying above the join; a `WHERE`
     conjunct still pushed when residuals are present (the regression the
     stacked-filter mistake would cause).
   - `tests/test_vec_plan_builder.cc` — 3- and 4-relation lowering; multi-key
     lowering; SIMD declined for multi-key; the migrated refusal tests.
   - `tests/test_vectorized.cc` — **drain a 3-relation plan and compare rows**,
     via the file's existing `drainVec` helper. Plan-shape tests do not prove
     execution.
   - `tests/test_planner.cc` — Volcano's narrowed multi-way message; multi-key
     executing on Volcano; the two type-error-precedence tests still passing.
2. **Documentation, all of it in the same commit as the last code change:**
   - `README.md` Week 27 section — mark the checkpoint ✅ and add the "Shipped /
     Why it was required" table this project uses for every closed week, plus a
     *"Starting notes, from Week 27's foundations"* block for Week 28 (the two
     obvious entries: `LogicalJoin::keys` is already the join graph's edge list,
     and `rowWidth`'s multi-relation fallback is the deferred data-volume cost
     term Week 22 pointed at Week 28).
   - `README.md` **Feature Scope** line about `JOIN ... ON` (currently "…as of
     Week 26; executing them is Week 27") and the **Limitations** bullet
     (multi-way now executes, vectorized only).
   - `development.md` — the *"Week 26 join scope"* callout; the **Supported SQL**
     block; site 8's row (second caller) and site 18's row (residuals now reach
     it); **and the two stale counts**: it claims 524 unit tests (actual
     pre-Week-27 baseline is **567**) and 440 harness assertions (actual is
     **508**). Correct both to the new post-Week-27 numbers.
3. Do **not** add benchmark numbers. Week 27 has no benchmark deliverable; the
   join-order benchmark belongs to Week 28.

### Verification

The full gate — this is the `verify` skill's checklist:

```bash
cmake --build build -j$(nproc)
cd build && ./tests/swiftql_tests          # expect > 567, 0 failures
cd .. && python3 python_tools/compare_against_sqlite.py   # 0 failed, 0 errors
```

- The harness total must **go up**, not sideways. If it lands near 508, a list
  was moved but not added to.
- `git status` must show no changes under `data/` (the CSVs are tracked; if you
  regenerated them for a scale test, `git checkout -- data/`).
- Re-read the diff for the two patterns this week most easily leaves behind:
  a bare-name `indexOf` on a merged schema, and a `catch`-free "it threw" test.

---

## Definition of done

| # | Criterion | How it is proven |
|---|---|---|
| 1 | A three-relation join returns SQLite-identical rows on the vectorized path | harness `WEEK27_VEC_ONLY_QUERIES` |
| 2 | A four-relation join does too | unit test over `sj` |
| 3 | The join key that is ambiguous by name resolves by slot | the `d.team = d2.team` count query, vs SQLite |
| 4 | Multi-key joins execute identically in **all four** modes | harness `QUERIES` |
| 5 | Non-equality `ON` conjuncts execute as residuals, and single-relation ones are pushed to their own scan | `--explain` shape test + harness |
| 6 | An `ON` clause with no equality is still refused | migrated `OrConditionRejected` |
| 7 | Volcano refuses multi-way with a message naming the vectorized path | `WEEK27_VOLCANO_REJECTED`, all Volcano modes |
| 8 | `--explain` prints all three sections for a multi-way query | manual + `test_vec_plan_builder` |
| 9 | No single-join plan or `--explain` string changed | existing suite, unmodified assertions |
| 10 | Both engines accept exactly the same `ON` clauses; they differ **only** in relation count | the two rejection lists together |

## What this week does not build

Listed so the temptation is answered in advance:

- **No join ordering.** The tree is written order, left-deep. DP enumeration,
  the greedy fallback, and the data-volume cost term are Week 28.
- **No `JoinGraph` type.** See the scope note at the top.
- **No outer joins**, and no join-type field on `LogicalJoin`. Week 29.
- **No cross-product operator.** A zero-key `ON` stays an error.
- **No multi-way execution on Volcano.** It is the correctness baseline, not the
  feature-complete path — and its single-join code is what the vectorized
  multi-key result is checked against.
- **No composite-key SIMD join.** Declining is correct; the hash join is always
  available.
- **No `Row`-free hash join.** `VecHashJoinNode` materializes build rows, which
  matters more now that a build side can be a whole intermediate result — but
  making it late-materializing is a performance project with its own week.
