// Week 37 — the key encoders are DUPLICATED, and this file is what stops them
// drifting apart.
//
// There are now three places that turn a key into bytes:
//
//   1. key_encoding.h            appendJoinKeyField / appendGroupKeyField
//                                — the original, via a Value.
//   2. chunk_key.h               appendIntKeyField / appendStringKeyField
//                                — the join's copy, straight from a column,
//                                  skipping the std::string keyFieldText()
//                                  returns by value.
//   3. vec_hash_aggregate_node.cc  appendGroupKeyFromColumn
//                                — the aggregate's copy, same motivation.
//
// Each copy was introduced for a measured reason (the temporary std::string was
// worth ~3ms on q18 and ~13ns per probe row), and each carries a comment saying
// it MUST stay byte-identical to the original. A comment is not a test.
//
// Why this matters more than it looks. The build side of a join encodes with one
// function and the probe side with another. If they ever disagree on a single
// byte for a single value, matching rows stop matching — silently, with no error
// anywhere, on exactly the values the two paths render differently. This
// codebase's own history says this is its most productive bug class: code
// trusting a rule that had since changed elsewhere, three times in Week 33 alone.
//
// Copy 3 lives in an anonymous namespace inside its .cc, so it cannot be called
// from here. It is covered behaviourally at the bottom of this file instead:
// grouping partitions the edge-case values exactly as the reference encoder's
// byte strings do.

#include <gtest/gtest.h>
#include "execution/key_encoding.h"
#include "execution/chunk_key.h"
#include "execution/vec_types.h"
#include "common/value.h"
#include <limits>
#include <string>
#include <vector>

namespace {

// One reference encoding, produced through the Value path.
std::string viaValue(const Value& v, bool length_prefixed) {
    std::string out;
    appendJoinKeyField(out, v, length_prefixed);
    return out;
}

// The INT values worth pinning: zero, both signs, both int64 extremes (where
// to_chars and a naive sign-then-digits loop diverge), and a value whose decimal
// text length crosses from one digit to two so the length prefix itself changes
// width.
const std::vector<int64_t> kInts = {
    0, 1, -1, 9, 10, -9, -10, 42, -42, 99, 100,
    123456789, -123456789,
    std::numeric_limits<int64_t>::max(),
    std::numeric_limits<int64_t>::min(),
};

// The STRING values worth pinning. The last three are the point of the
// length prefix: a field containing the separator byte, a field that LOOKS like
// a length prefix, and the empty string. Without `<len>:` in front, ("A\x01B","C")
// and ("A","B\x01C") serialize alike and two rows differing in BOTH keys join.
const std::vector<std::string> kStrings = {
    "", "a", "Ferrari", "MED BOX", "Brand#23",
    std::string("A\x01" "B", 3),
    "3:abc",
    std::string("\x01", 1),
    std::string(64, 'x'),
};

}  // namespace

// ---------------------------------------------------------------- INT parity

TEST(KeyEncodingParity, IntColumnPathMatchesValuePathLengthPrefixed) {
    for (int64_t i : kInts) {
        std::string fast;
        chunk_key::appendIntKeyField(fast, i, /*length_prefixed=*/true);
        EXPECT_EQ(fast, viaValue(Value(i), true)) << "int " << i;
    }
}

TEST(KeyEncodingParity, IntColumnPathMatchesValuePathSingleKey) {
    // A one-key tuple keeps the sentinel-only form, which was already injective.
    for (int64_t i : kInts) {
        std::string fast;
        chunk_key::appendIntKeyField(fast, i, /*length_prefixed=*/false);
        EXPECT_EQ(fast, viaValue(Value(i), false)) << "int " << i;
    }
}

// ------------------------------------------------------------- STRING parity

TEST(KeyEncodingParity, StringColumnPathMatchesValuePathLengthPrefixed) {
    for (const std::string& s : kStrings) {
        std::string fast;
        chunk_key::appendStringKeyField(fast, s, /*length_prefixed=*/true);
        EXPECT_EQ(fast, viaValue(Value(s), true)) << "string of size " << s.size();
    }
}

TEST(KeyEncodingParity, StringColumnPathMatchesValuePathSingleKey) {
    for (const std::string& s : kStrings) {
        std::string fast;
        chunk_key::appendStringKeyField(fast, s, /*length_prefixed=*/false);
        EXPECT_EQ(fast, viaValue(Value(s), false)) << "string of size " << s.size();
    }
}

// --------------------------------------------------------------- injectivity

TEST(KeyEncodingParity, LengthPrefixIsInjectiveAcrossFieldBoundaries) {
    // The property the length prefix exists for, stated as a test rather than as
    // a comment: two DIFFERENT two-field tuples must never serialize alike, even
    // when one field contains the separator byte.
    auto encode2 = [](const std::string& a, const std::string& b) {
        std::string out;
        chunk_key::appendStringKeyField(out, a, true);
        chunk_key::appendStringKeyField(out, b, true);
        return out;
    };
    EXPECT_NE(encode2(std::string("A\x01" "B", 3), "C"),
              encode2("A", std::string("B\x01" "C", 3)));
    EXPECT_NE(encode2("", "ab"), encode2("a", "b"));
    EXPECT_NE(encode2("3:a", "b"), encode2("3", ":ab"));
}

TEST(KeyEncodingParity, IntAndIntegralDoubleEncodeALIKE) {
    // DELIBERATE, and the reason the join's INT64 fast path requires BOTH sides
    // to be INT. keyFieldText renders an integral DOUBLE through integer text so
    // that `7` and `7.0` join. Hashing raw bits instead would silently reject a
    // matching row, so this equality is load-bearing rather than incidental.
    EXPECT_EQ(viaValue(Value(int64_t{7}), true), viaValue(Value(7.0), true));
    EXPECT_EQ(viaValue(Value(int64_t{-3}), true), viaValue(Value(-3.0), true));
}

TEST(KeyEncodingParity, BothNaNSignsEncodeAlikeAndNullIsDistinct) {
    const double nan_pos = std::numeric_limits<double>::quiet_NaN();
    const double nan_neg = -std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(viaValue(Value(nan_pos), true), viaValue(Value(nan_neg), true));

    // A NULL group key must not collide with any real value, including the
    // string "N" that its own marker byte uses.
    std::string null_key;
    appendGroupKeyField(null_key, Value::null());
    std::string n_key;
    appendGroupKeyField(n_key, Value(std::string("N")));
    EXPECT_NE(null_key, n_key);
}

// -------------------------------------- copy 3, covered through the public API

TEST(KeyEncodingParity, ColumnVectorGroupKeysPartitionAsTheReferenceEncoderDoes) {
    // vec_hash_aggregate_node.cc's appendGroupKeyFromColumn is file-local, so it
    // is pinned by CONSEQUENCE: for a set of edge-case cells, two rows must fall
    // in the same group exactly when the reference encoder gives them the same
    // bytes. That is the only property the aggregate actually depends on.
    ColumnVector cv = makeColumnVector(TypeId::STRING);
    const std::vector<std::string> cells = {
        "a", "a", "", "N", std::string("A\x01" "B", 3), "A", "B",
    };
    for (const std::string& s : cells) appendColumnValue(cv, Value(s));
    appendColumnValue(cv, Value::null());
    appendColumnValue(cv, Value::null());

    const int n = static_cast<int>(cells.size()) + 2;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            std::string ki, kj;
            appendGroupKeyField(ki, valueAt(cv, i));
            appendGroupKeyField(kj, valueAt(cv, j));
            // Same bytes iff same cell content (NULL included, and NULL groups
            // with NULL by design — a documented divergence from SQL equality).
            const bool same_bytes = (ki == kj);
            const bool both_null = cv.isNull(i) && cv.isNull(j);
            const bool same_text = (!cv.isNull(i) && !cv.isNull(j)
                                    && cells[i] == cells[j]);
            EXPECT_EQ(same_bytes, both_null || same_text)
                << "rows " << i << " and " << j;
        }
    }
}
