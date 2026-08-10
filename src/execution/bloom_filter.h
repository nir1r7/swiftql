#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

// A Bloom filter over SERIALIZED JOIN KEYS, for sideways information passing.
//
// WHAT IT IS FOR. A hash join's build side knows, before a single probe row is
// read, the complete set of keys that can possibly match. Handing a compact
// summary of that set DOWN the probe pipeline lets the scan drop rows that
// cannot match before they reach the join — the join walks fewer chains and the
// scan emits fewer rows. VecHashJoinNode builds one of these at the end of its
// build phase and pushes it into its probe child (VecPlanNode::pushBloomFilter).
//
// THE ONE PROPERTY EVERYTHING RESTS ON. A Bloom filter has FALSE POSITIVES and
// never FALSE NEGATIVES: `maybeContains(k) == false` means k was certainly never
// added. So dropping a row is sound only where a probe row with no match
// produces no output — an INNER join and a SEMI join. It is NOT sound for LEFT
// OUTER (an unmatched probe row must still be emitted, null-extended) or for
// ANTI (unmatched probe rows are exactly the answer). That gate lives at the
// push site in vec_hash_join_node.cc, which is the only place that knows the
// semantics.
//
// THE SECOND PROPERTY, and the one that is easy to break by accident: the bytes
// hashed on the build side and on the probe side must be THE SAME BYTES. Both
// sides go through chunk_key::serializeKey, which is why that function was
// lifted out of the join into its own header — a second copy of the encoder
// would turn "no false negatives" into a silent wrong answer.
//
// SIZING. ~10 bits per key with k = 3 hash functions gives a false-positive rate
// of (1 - e^(-3/10))^3 ≈ 1.7%, and a false positive costs only the hash-table
// lookup the join would have done anyway. The bit count is rounded UP to a power
// of two so a bit index is a mask rather than a modulo.
//
// DOUBLE HASHING. The k bit positions are h1 + i*h2, with h2 derived from h1 by
// a bit mixer (splitmix64's finalizer) rather than by a second pass over the
// key. Kirsch–Mitzenmacher: for a Bloom filter this has the same asymptotic
// false-positive rate as k independent hashes, at the cost of one hash of the
// key bytes instead of k.
// HOW THE KEY REACHES THE FILTER, chosen by the join and OBEYED by the scan.
//
// SERIALIZED is the general form: the key bytes chunk_key::serializeKey
// produces, which is what the join's own hash table compares. It handles every
// key shape — several columns, strings, doubles, mixed INT/DOUBLE pairs whose
// texts the encoding deliberately normalises onto each other.
//
// INT64 exists because SERIALIZED is EXPENSIVE ON THE PROBE SIDE, and measurably
// so: on TPC-H sf0.1 q3 the lineitem scan went 2.6ms -> 10.4ms testing 600572
// rows, ~13ns each, while the pushdown saved only 8.2ms below it. Building a
// string per row to hash ten bytes is the whole of that cost. When the join has
// ONE key column and BOTH sides store it as INT, the int64 IS the key: the
// encoding is injective on int64, so two rows serialize alike exactly when their
// integers are equal, and hashing the integer answers the same membership
// question with no string at all.
//
// BOTH SIDES INT IS THE CONDITION, not one of them, and that is the case the
// mode exists to exclude rather than a formality. The serialized encoding
// normalises an integral DOUBLE onto the INT text (7.0 and 7 both render "7", so
// they JOIN — keyFieldText's rule, matching SQLite's numeric affinity). Hashing
// the raw bits instead would put them in different buckets and the filter would
// reject a row that matches: a FALSE NEGATIVE, which is the one thing a Bloom
// filter must never produce. So the mode is taken only where no DOUBLE can
// appear on either side.
enum class BloomKeyMode : uint8_t { SERIALIZED, INT64 };

class BloomFilter {
public:
    static constexpr int kBitsPerKey = 10;
    static constexpr int kNumHashes  = 3;

    // `expected_keys` sizes the bit array; adding more than that many keys stays
    // correct and only raises the false-positive rate. A build side of zero keys
    // still gets a real (tiny, empty) filter, which rejects everything — exactly
    // right for an inner or semi join over an empty build side.
    explicit BloomFilter(size_t expected_keys, BloomKeyMode mode = BloomKeyMode::SERIALIZED)
        : mode_(mode) {
        size_t bits = 64;
        const size_t wanted = expected_keys * static_cast<size_t>(kBitsPerKey);
        while (bits < wanted) bits <<= 1;
        bit_mask_ = bits - 1;
        words_.assign(bits / 64, 0);
    }

    BloomKeyMode mode() const { return mode_; }

    void add(std::string_view key)                { insertHash(hash(key)); }
    void addInt(int64_t key)                      { insertHash(mix(static_cast<uint64_t>(key))); }

    // false => the key was CERTAINLY never added. true => it may have been.
    bool maybeContains(std::string_view key) const { return testHash(hash(key)); }
    bool maybeContainsInt(int64_t key) const {
        return testHash(mix(static_cast<uint64_t>(key)));
    }

    size_t bitCount() const { return words_.size() * 64; }

private:
    void insertHash(uint64_t h1) {
        const uint64_t h2 = mix(h1) | 1;   // odd, so i*h2 walks all residues
        for (int i = 0; i < kNumHashes; ++i) {
            const uint64_t bit = h1 & bit_mask_;
            words_[bit >> 6] |= (uint64_t(1) << (bit & 63));
            h1 += h2;
        }
    }

    bool testHash(uint64_t h1) const {
        const uint64_t h2 = mix(h1) | 1;
        for (int i = 0; i < kNumHashes; ++i) {
            const uint64_t bit = h1 & bit_mask_;
            if ((words_[bit >> 6] & (uint64_t(1) << (bit & 63))) == 0) return false;
            h1 += h2;
        }
        return true;
    }

    // The same hash the join's chained index uses (chunk_key::hashKey), restated
    // here rather than included so this header stays free of the execution
    // types. It only has to be a good hash of the bytes: nothing outside this
    // class sees the value, and both add() and maybeContains() call it.
    static uint64_t hash(std::string_view key) {
        return std::hash<std::string_view>{}(key);
    }

    // splitmix64's finalizer — avalanches the bits of h1 so the second hash is
    // independent enough for double hashing.
    static uint64_t mix(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    std::vector<uint64_t> words_;
    uint64_t bit_mask_ = 0;
    BloomKeyMode mode_ = BloomKeyMode::SERIALIZED;
};
