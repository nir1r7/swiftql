#include <gtest/gtest.h>

#include "common/column_id.h"
#include "common/schema.h"
#include "common/value.h"
#include "parser/ast.h"
#include "storage/chunk_pruner.h"
#include "storage/columnar_table.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// WHICH SCHEMA THE RAISE SCREEN IS ASKED IN.
//
// ChunkPruner::shouldSkip walks the pruning hint's AND spine in WRITTEN ORDER
// and stops at the first conjunct that may raise, because a zone-map skip
// removes rows BEFORE the filter runs and would mask a raise the filter owed.
// The screen is conjunctMayRaise (parser/expr_totality.h), and it is a function
// of a SCHEMA.
//
// Through fix round 4 the schema it was handed was the SCANNING relation's own,
// and chunk_pruner.h claimed that made the walk conservative: a conjunct naming
// another relation could not be typed, so it would answer "may raise" and stop.
// That claim is what these tests are about, and it is FALSE. staticTypeOf's
// ColumnRef arm resolves slot-first with a BARE-NAME FALLBACK — the rule it
// shares with evaluator.cc's resolveColumnIndex, where it is correct — so a
// slot-1 reference that misses the slot lookup is then resolved against the
// scanned table's column of the SAME NAME. When the two relations declare that
// name with different TYPES the screen does not say "I cannot type this"; it
// says "I typed it", wrongly, and a raiser reads as total.
//
// Three seam audits reached this independently in pass 5 — engine E-20, storage
// S-13, optimizer P5-2 — from three different doors, and it is one fix: both
// scan nodes now carry the schema the hint was WRITTEN against and hand THAT to
// shouldSkip.
//
// EVERY TEST BELOW IS A PAIR, and the pair is the evidence. The same expression
// and the same zone maps are screened twice, once in each schema, and the two
// answers differ — so the tests cannot pass by accident on a build where the
// argument is ignored, and the "wrong schema" half documents the defect at the
// exact call the fix removed. Both halves compile and run against the pre-fix
// tree, because shouldSkip's signature did not change: only what the callers
// pass to it did.
//
// THE SHAPE, from the storage auditor's reproduction:
//
//   big(k INT, x INT, j INT)  JOIN  small2(j INT, x STRING, s STRING)
//   SELECT COUNT(*) FROM big b JOIN small2 s2 ON b.j = s2.j
//   WHERE s2.x AND b.k > 999999
//     row storage       Error: std::get: wrong index for variant
//     columnar storage  0            (chunks_skipped=3/3)
//
// with the RAISER WRITTEN FIRST, which is the case chunk_pruner.h's own
// soundness proof calls unsound.

namespace {

// big's standalone scan schema: everything slot 0.
Schema scanSchema() {
    return Schema({
        ColumnDef{"k", TypeId::INT, 0, false},
        ColumnDef{"x", TypeId::INT, 0, false},
        ColumnDef{"j", TypeId::INT, 0, false},
    });
}

// The schema the WHERE was written against: the join's merged output, with
// small2's columns stamped slot 1. `x` appears twice, INT at slot 0 and STRING
// at slot 1 — the collision the whole defect needs, and an ordinary one for two
// tables written without a per-table column prefix.
Schema hintSchema() {
    return Schema({
        ColumnDef{"k", TypeId::INT, 0, false},
        ColumnDef{"x", TypeId::INT, 0, false},
        ColumnDef{"j", TypeId::INT, 0, false},
        ColumnDef{"j", TypeId::INT, 1, false},
        ColumnDef{"x", TypeId::STRING, 1, false},
        ColumnDef{"s", TypeId::STRING, 1, false},
    });
}

std::unique_ptr<Expr> col(const std::string& name, int slot) {
    auto c = std::make_unique<ColumnRef>();
    c->column_name = name;
    c->id = ColumnId::local(slot);
    return c;
}

std::unique_ptr<Expr> lit(int64_t v) {
    return std::make_unique<Literal>(Value(v));
}

std::unique_ptr<Expr> lit(const std::string& v) {
    return std::make_unique<Literal>(Value(v));
}

std::unique_ptr<Expr> bin(const std::string& op, std::unique_ptr<Expr> l,
                          std::unique_ptr<Expr> r) {
    auto b = std::make_unique<BinaryExpr>();
    b->op = op;
    b->left = std::move(l);
    b->right = std::move(r);
    return b;
}

// One chunk of `big.k`, holding 0..19999 — so `k > 999999` proves a skip and
// `k > 5` proves nothing. Only `k` has a zone map, which is also what makes the
// collected hint unambiguous: nothing here can prune on `x` by accident.
std::unordered_map<std::string, std::vector<ColumnChunk>> zoneMaps() {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zm;
    zm["k"] = {ColumnChunk{0, 8192, Value(static_cast<int64_t>(0)),
                           Value(static_cast<int64_t>(19999))}};
    return zm;
}

}  // namespace

// ── 1. The truth-value form: `WHERE s2.x AND b.k > 999999` ──────────────────
//
// `s2.x` is a bare conjunct, so the filter takes its TRUTH VALUE with
// `v.asInt() != 0`, and asInt() on a STRING is std::bad_variant_access
// (value.cc:28). conjunctMayRaise catches that through the "a conjunct whose
// static type is not INT can raise" rule — but only if it types `s2.x` as the
// STRING column it is.

TEST(ChunkPrunerHintSchema, ARaiserNamingAnotherRelationStopsTheWalk) {
    auto where = bin("AND", col("x", 1),
                     bin(">", col("k", 0), lit(static_cast<int64_t>(999999))));
    EXPECT_FALSE(ChunkPruner::shouldSkip(where.get(), zoneMaps(), 0, hintSchema()));
}

TEST(ChunkPrunerHintSchema, TheDefectItselfTheScanSchemaSkipsThatChunk) {
    // The counterfactual, kept as a test so the reason the argument above is
    // load-bearing cannot be deleted by accident: screened in the SCANNING
    // relation's schema, `x@1` falls back to the bare name, finds big.x (INT),
    // types as INT, answers "cannot raise" — and `k > 999999` then proves the
    // skip that masks the raise. This is what shipped before Week 37.
    auto where = bin("AND", col("x", 1),
                     bin(">", col("k", 0), lit(static_cast<int64_t>(999999))));
    EXPECT_TRUE(ChunkPruner::shouldSkip(where.get(), zoneMaps(), 0, scanSchema()));
}

// ── 2. The comparison form: `WHERE s2.x > 5 AND b.k > 999999` ───────────────
//
// STRING against a numeric literal is Value's NUMERIC_COERCE throw
// ("Type mismatch in Value comparison"), the raise class pass 4's P4-1 was
// about, reached here through the screen rather than through canSkipChunk.

TEST(ChunkPrunerHintSchema, AStringBoundaryComparisonOnAnotherRelationStopsTheWalk) {
    auto where = bin("AND", bin(">", col("x", 1), lit(static_cast<int64_t>(5))),
                     bin(">", col("k", 0), lit(static_cast<int64_t>(999999))));
    EXPECT_FALSE(ChunkPruner::shouldSkip(where.get(), zoneMaps(), 0, hintSchema()));
    EXPECT_TRUE(ChunkPruner::shouldSkip(where.get(), zoneMaps(), 0, scanSchema()));
}

// ── 3. The written-order rule still decides, and still in the right direction ─
//
// A conjunct written AHEAD of every raiser may still prune: nothing it removes
// was ever going to be evaluated by the raiser, because the cascade stops at the
// first conjunct that is false.

TEST(ChunkPrunerHintSchema, GuardAPrunableConjunctWrittenFirstStillPrunes) {
    auto where = bin("AND", bin(">", col("k", 0), lit(static_cast<int64_t>(999999))),
                     col("x", 1));
    EXPECT_TRUE(ChunkPruner::shouldSkip(where.get(), zoneMaps(), 0, hintSchema()));
}

// ── 4. The pruning the previous round lost (storage S-14) ───────────────────
//
// This is the half that is NOT a correctness fix, and it is the reason the
// schema is threaded rather than the cheaper guard (refuse to screen any
// conjunct holding a ref that does not resolve at its own slot) being applied.
// A cross-relation conjunct that CANNOT raise must not stop the walk — and in
// the scanning relation's schema it always did, because the name is simply
// absent there and staticTypeOf answers "don't know".
//
// Measured cost of that on the shipped catalog: +21% on a 2-chunk table and
// 2.80x on TPC-H sf0.01's 8-chunk lineitem, in col-volcano on BOTH optimizer
// settings, decided purely by which conjunct the user wrote first.

TEST(ChunkPrunerHintSchema, ATotalCrossRelationConjunctDoesNotStopTheWalk) {
    // `s2.s = 'q'` is STRING vs STRING: total, so `b.k > 999999` behind it may
    // still prove the skip.
    auto where = bin("AND", bin("=", col("s", 1), lit(std::string("q"))),
                     bin(">", col("k", 0), lit(static_cast<int64_t>(999999))));
    EXPECT_TRUE(ChunkPruner::shouldSkip(where.get(), zoneMaps(), 0, hintSchema()));
}

TEST(ChunkPrunerHintSchema, TheLostPruningTheScanSchemaCannotTypeThatConjunct) {
    // The same expression in the scanning relation's schema: `s` is in no
    // schema big has, staticTypeOf fails, the conjunct reads as may-raise, and
    // the walk stops before reaching the one that prunes. Correct-and-slower,
    // which is why it was a performance finding and not a correctness one — but
    // it is the SAME missing argument as the tests above.
    auto where = bin("AND", bin("=", col("s", 1), lit(std::string("q"))),
                     bin(">", col("k", 0), lit(static_cast<int64_t>(999999))));
    EXPECT_FALSE(ChunkPruner::shouldSkip(where.get(), zoneMaps(), 0, scanSchema()));
}

// ── 5. The collection guard is unchanged, and it still has to be ────────────
//
// The screen decides whether the walk CONTINUES; `relation_slot < 1` decides
// what is COLLECTED. Typing a foreign conjunct correctly must not turn it into a
// hint about this table's zone maps — invariant 12's subject. `s2.j = 50001`
// types as INT vs INT, cannot raise, and the walk continues past it; if it were
// collected it would be matched against big's zone maps BY NAME and, since
// small2 shares the name `j`, would prune on the wrong relation's values.

TEST(ChunkPrunerHintSchema, GuardATotalForeignConjunctIsStillNotCollected) {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zm;
    // big.j holds 0..9, so a foreign `j = 50001` matched by name would skip.
    zm["j"] = {ColumnChunk{0, 8192, Value(static_cast<int64_t>(0)),
                           Value(static_cast<int64_t>(9))}};
    auto where = bin("=", col("j", 1), lit(static_cast<int64_t>(50001)));
    EXPECT_FALSE(ChunkPruner::shouldSkip(where.get(), zm, 0, hintSchema()));
}

// ── 6. Nothing about a single-relation query changes ────────────────────────
//
// For a scan with no join above it the two schemas ARE the same object, which is
// what the nullptr default on both scan-node constructors means. Pinned so a
// future change cannot make the common path depend on a hint schema being
// supplied.

TEST(ChunkPrunerHintSchema, GuardASingleRelationHintIsUnaffected) {
    auto where = bin(">", col("k", 0), lit(static_cast<int64_t>(999999)));
    EXPECT_TRUE(ChunkPruner::shouldSkip(where.get(), zoneMaps(), 0, scanSchema()));
    auto not_prunable = bin(">", col("k", 0), lit(static_cast<int64_t>(5)));
    EXPECT_FALSE(ChunkPruner::shouldSkip(not_prunable.get(), zoneMaps(), 0, scanSchema()));
}
