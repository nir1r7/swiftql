#pragma once

#include "common/value.h"
#include "common/type_id.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

// Serialized key tuples — join keys, GROUP BY keys and DISTINCT keys.
//
// Six serializers in the engine (two hash joins, two hash aggregates, two
// distinct operators) turn a row's key columns into one string and compare the
// strings. They all depend on the same two properties, and each one that
// restated the rules locally got at least one of them wrong. The rules live here
// so there is one place to be right:
//
// 1. THE ENCODING MUST BE INJECTIVE. One field per key column, in key order,
//    length-prefixed (`<len>:<bytes>`) and terminated by a `'\x01'` sentinel.
//    The sentinel alone is not enough: nothing stops a field from CONTAINING it
//    — `CSVLoader::parseField` returns a STRING cell verbatim — so
//    ("A\x01B","C") and ("A","B\x01C") would serialize alike and two rows
//    differing in BOTH keys would compare equal. A length prefix is injective
//    for any bytes at fixed arity: read digits to the ':', take that many bytes,
//    skip the sentinel, repeat.
//
// 2. THE TEXT MUST IDENTIFY THE VALUE, NOT DISPLAY IT. `Value::toString()`
//    formats a DOUBLE with `%.15g` for human output, which is lossy: over the
//    shipped 10k-row dataset `sector_1 + sector_2` takes 3245 distinct values
//    but only 2526 distinct `%.15g` texts, so DISTINCT and GROUP BY collapsed
//    706 pairs of genuinely different numbers — and every collapsed group's
//    COUNT(*) was wrong with them. `keyFieldText` below is exact instead.
//
// NULL is where the three uses legitimately differ, so it stays the caller's
// decision rather than being baked in here. A join drops the row (SQL: NULL
// equals nothing, so it can never match); GROUP BY and DISTINCT keep it as a
// group of its own, which is why they need a marker that cannot collide with any
// value's text — `'N'`, which no non-NULL field can start with, since those
// start with a decimal length digit.

// Exact text for one key field. Never for display — `Value::toString()` is that.
//
// DOUBLE is the only interesting case:
//   - an integral double goes through the integer path, so 7.0 and the INT 7
//     produce the same text and still join, matching SQLite's numeric affinity.
//     Doing it by digits rather than by `%g` removes an exponent cliff: `%.17g`
//     renders 1e17 as "1e+17" while the INT prints "100000000000000000", so the
//     two stopped matching above that magnitude. The bound is the exact `int64_t`
//     domain — the two powers of two are the only cast-safe comparison points,
//     and a round number near them leaves a narrower cliff rather than none
//     (2^63 - 1024 is exactly representable and exactly equal to its INT, and
//     SQLite's INTEGER-vs-REAL comparison is exact, so it must still match).
//     -0.0 falls out of this branch as "0", which is the normalisation IEEE and
//     SQLite both want — they call the two zeros equal, their texts differ.
//   - anything else gets `%.17g`, which round-trips, so distinct doubles get
//     distinct text: 0.1 + 0.2 -> "0.30000000000000004" against 0.3 ->
//     "0.29999999999999999". `%.15g` gave both "0.3".
//   - NaN renders "nan" and therefore groups with itself. `%.17g` alone does not
//     deliver that — it prints a sign-bit-set NaN as "-nan", which split the two
//     signs into two groups — so the sign is dropped here. That is a grouping
//     choice, not an equality one; a join must NOT match two NaNs, which is what
//     isUnmatchableKey below is for. A NaN group still exists where SQLite has
//     none (it converts NaN to NULL on storage); see README's Limitations.
inline std::string keyFieldText(const Value& v) {
    if (!v.isNull() && v.type() == TypeId::DOUBLE) {
        const double d = v.asDouble();
        if (std::isnan(d)) return "nan";
        if (std::isfinite(d) && std::trunc(d) == d &&
            d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
            return std::to_string(static_cast<int64_t>(d));
        }
        char buf[40];
        snprintf(buf, sizeof(buf), "%.17g", d);
        return buf;
    }
    return v.toString();
}

// A key value that can never compare equal to anything, including a copy of
// itself, so a join must drop the row rather than encode it.
//
// NULL is the SQL rule. NaN is the IEEE one, and it matters precisely because
// the encoding compares text: two NaNs serialize identically and would land in
// one bucket, while `Value::operator==` on the same pair is false — so the join
// would match a pair that the identical predicate in a WHERE clause rejects.
// SQLite never has the case at all (it stores NaN as NULL), so dropping agrees
// with it too.
inline bool isUnmatchableKey(const Value& v) {
    if (v.isNull()) return true;
    return v.type() == TypeId::DOUBLE && std::isnan(v.asDouble());
}

// One field of a JOIN key tuple. Callers test isUnmatchableKey first and drop
// the row, so NULL and NaN never reach here.
//
// A single-field tuple skips the length prefix: `k + '\x01'` is already
// injective as a map from k, and skipping it keeps single-key joins
// byte-identical to what they hashed before Week 27.
inline void appendJoinKeyField(std::string& out, const Value& v, bool length_prefixed) {
    const std::string text = keyFieldText(v);
    if (length_prefixed) {
        out += std::to_string(text.size());
        out += ':';
    }
    out += text;
    out += '\x01';
}

// One field of a GROUP BY or DISTINCT key tuple. Always length-prefixed — the
// arity here is the whole key list's width and there is no pre-Week-27 encoding
// to stay compatible with — and NULL is a group of its own, marked `'N'`.
// Without that marker a NULL and the literal string 'NULL' share a bucket:
// `SELECT DISTINCT CASE WHEN speed > 300 THEN 'NULL' END FROM laps` returned one
// row where SQLite returns two.
inline void appendGroupKeyField(std::string& out, const Value& v) {
    if (v.isNull()) {
        out += 'N';
    } else {
        const std::string text = keyFieldText(v);
        out += std::to_string(text.size());
        out += ':';
        out += text;
    }
    out += '\x01';
}
