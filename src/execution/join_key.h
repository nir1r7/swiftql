#pragma once

#include "common/value.h"
#include "common/type_id.h"
#include <cstdio>
#include <string>

// Serialized equi-join key tuples, shared by every hash-join operator so the two
// engines cannot drift apart on the encoding.
//
// The rule is one field per key column, in key order, terminated by a `'\x01'`
// sentinel. That alone is uniquely decodable only if no field can CONTAIN the
// sentinel — and nothing enforces that: `CSVLoader::parseField` returns a STRING
// cell verbatim and `Value::toString()` hands back those bytes, so a `0x01` byte
// in a CSV cell reaches the key. Without a length prefix, ("A\x01B","C") and
// ("A","B\x01C") both serialize to `A 01 B 01 C 01` and two rows that differ in
// BOTH keys land in one bucket and join — a silent wrong answer.
//
// So a multi-field tuple length-prefixes every field (`<len>:<bytes>`), which is
// injective for any bytes at fixed arity: read digits to the ':', take that many
// bytes, skip the sentinel, repeat. A single-field tuple does not — `k + '\x01'`
// is already injective for any k, and skipping the prefix keeps single-key joins
// byte-identical to the pre-Week-27 encoding.
//
// NULL is not encoded at all: callers test `isNull()` first and drop the row,
// because SQL's NULL equals nothing and one NULL member makes the whole tuple
// unmatchable.
//
// A DOUBLE field does NOT go through `Value::toString()`, which formats for
// display with `%.15g` and therefore renders `0.1 + 0.2` and `0.3` — two
// distinct doubles — as the same "0.3", joining rows SQLite keeps apart (it
// compares REALs exactly). `%.17g` is round-trippable, so distinct doubles get
// distinct text. Two details keep the rest of the semantics intact: an integral
// double still renders as bare digits (`7.0` -> "7"), so an INT key and a DOUBLE
// key holding the same number still join, which is SQLite's numeric affinity;
// and -0.0 is normalised to 0.0, since IEEE and SQLite both call them equal
// while their texts differ.
inline void appendJoinKeyField(std::string& out, const Value& v, bool length_prefixed) {
    std::string text;
    if (!v.isNull() && v.type() == TypeId::DOUBLE) {
        double d = v.asDouble();
        if (d == 0.0) d = 0.0;                 // collapses -0.0, which == 0.0
        char buf[40];
        snprintf(buf, sizeof(buf), "%.17g", d);
        text = buf;
    } else {
        text = v.toString();
    }
    if (length_prefixed) {
        out += std::to_string(text.size());
        out += ':';
    }
    out += text;
    out += '\x01';
}
