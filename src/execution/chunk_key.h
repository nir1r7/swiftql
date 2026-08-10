#pragma once

#include "execution/key_encoding.h"
#include "execution/vec_types.h"
#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// One row's k-key tuple, serialized straight out of a DataChunk.
//
// WEEK 38 — this WAS a static function in vec_hash_join_node.cc's anonymous
// namespace, and it moved here for one reason: the Bloom filter pushdown builds
// keys on the BUILD side (in the join) and tests them on the PROBE side (in the
// scan), and the two must produce THE SAME BYTES or the filter deletes rows that
// would have matched. A Bloom filter has no false negatives only if both sides
// hash the same string; two copies of an encoder are exactly how that stops
// being true. Nothing about the encoding changed — the rules still live in
// key_encoding.h, and the join calls the same function it always did.

namespace chunk_key {

// The INT and STRING key fields written STRAIGHT INTO the key buffer.
//
// Each is BYTE-IDENTICAL to appendJoinKeyField (key_encoding.h) on the same
// value, and that identity is the only thing that makes them safe to have: the
// encoding, its injectivity and its `<len>:<bytes>` + '\x01' shape all stay
// exactly where they are defined, and a single-field tuple still skips the
// length prefix. What is skipped here is the std::string keyFieldText() returns
// BY VALUE — for an INT the digits go to a stack buffer instead of a fresh
// string, for a STRING the characters are appended from the column instead of
// being copied into a temporary first. std::to_chars in base 10 writes what
// std::to_string writes, for both the value and the length.
//
// DOUBLE is deliberately absent. keyFieldText's DOUBLE arm is where the exponent
// cliff, the integral-double normalisation and the NaN rule live — restating any
// of that here is how two serializers drift — so a DOUBLE key still goes through
// the shared function.
inline void appendIntKeyField(std::string& out, int64_t v, bool length_prefixed) {
    char digits[24];
    const auto rendered = std::to_chars(digits, digits + sizeof(digits), v);
    const size_t n = static_cast<size_t>(rendered.ptr - digits);
    if (length_prefixed) {
        char len[24];
        const auto lr = std::to_chars(len, len + sizeof(len), n);
        out.append(len, static_cast<size_t>(lr.ptr - len));
        out += ':';
    }
    out.append(digits, n);
    out += '\x01';
}

inline void appendStringKeyField(std::string& out, const std::string& v, bool length_prefixed) {
    if (length_prefixed) {
        char len[24];
        const auto lr = std::to_chars(len, len + sizeof(len), v.size());
        out.append(len, static_cast<size_t>(lr.ptr - len));
        out += ':';
    }
    out += v;
    out += '\x01';
}

// Serialize one row's k-key tuple into `out`, reusing its capacity. The encoding
// itself lives in key_encoding.h, shared with Volcano's HashJoinNode.
//
// Returns false when any key column is NULL. SQL's NULL equals nothing, so with
// k keys the rule composes: one NULL member makes the whole tuple unmatchable,
// on either side. Dropping such rows keeps them out of the hash table instead of
// bucketing them under toString()'s "NULL", which would make NULL = NULL match.
inline bool serializeKey(const DataChunk& chunk, const std::vector<int>& key_idx, int r,
                         std::string& out) {
    const bool prefixed = key_idx.size() > 1;
    out.clear();
    for (int c : key_idx) {
        const ColumnVector& col = chunk.columns[c];
        // The two direct arms below still consult the validity mask FIRST, which
        // is the whole reason valueAt exists: a raw read of the typed vector
        // turns a NULL into the placeholder underneath it.
        if (col.type == TypeId::INT) {
            // isUnmatchableKey on an INT is isNull() and nothing else — its
            // second clause is `type() == DOUBLE && isnan`, which an INT column
            // cannot satisfy.
            if (col.isNull(r)) return false;
            appendIntKeyField(out, std::get<std::vector<int64_t>>(col.data)[r], prefixed);
            continue;
        }
        if (col.type == TypeId::STRING) {
            if (col.isNull(r)) return false;
            appendStringKeyField(out, std::get<std::vector<std::string>>(col.data)[r], prefixed);
            continue;
        }
        Value v = valueAt(col, r);
        if (isUnmatchableKey(v)) return false;
        appendJoinKeyField(out, v, prefixed);
    }
    return true;
}

// The hash the join's chained index buckets on. std::hash over the SERIALIZED
// key, so the only thing this depends on is the byte string key_encoding.h
// already guarantees is injective — two rows hash alike exactly when they may
// match, and a collision is resolved by comparing those same bytes.
inline uint64_t hashKey(std::string_view key) {
    return std::hash<std::string_view>{}(key);
}

} // namespace chunk_key
