#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Overflow-checked INT arithmetic, shared by the two expression evaluators.
//
// WHY: `l + r` on int64_t is signed overflow — undefined behaviour, not
// wraparound. The compiler is entitled to assume it cannot happen and optimise
// accordingly at -O2, and a UBSan build aborts. SwiftQL used to compute
// 9223372036854775807 + 1 as -9223372036854775808 by accident of the target.
//
// SQLite promotes an overflowing INT expression to REAL. SwiftQL cannot: the
// INT/INT result type is fixed at plan time (inferExprType), the vectorized path
// pre-allocates an INT column from it, and INT/INT truncating division depends on
// staying INT. So the choice is between defined wraparound and a clear error, and
// an analytical engine should not answer with a silently wrong number.
//
// Divergence from SQLite is documented in readme.md's Limitations.

[[noreturn]] inline void throwIntOverflow(const std::string& op) {
    throw std::runtime_error(
        "integer overflow in '" + op + "': the result does not fit in a 64-bit "
        "integer (SwiftQL does not promote to DOUBLE)");
}

inline int64_t checkedAdd(int64_t l, int64_t r) {
    int64_t out;
    if (__builtin_add_overflow(l, r, &out)) throwIntOverflow("+");
    return out;
}

inline int64_t checkedSub(int64_t l, int64_t r) {
    int64_t out;
    if (__builtin_sub_overflow(l, r, &out)) throwIntOverflow("-");
    return out;
}

inline int64_t checkedMul(int64_t l, int64_t r) {
    int64_t out;
    if (__builtin_mul_overflow(l, r, &out)) throwIntOverflow("*");
    return out;
}

// Caller handles r == 0 (SQLite semantics: x/0 is NULL). The remaining overflow
// is INT64_MIN / -1, whose true value is INT64_MAX + 1.
inline int64_t checkedDiv(int64_t l, int64_t r) {
    if (l == INT64_MIN && r == -1) throwIntOverflow("/");
    return l / r;
}

// -INT64_MIN overflows for the same reason.
inline int64_t checkedNegate(int64_t v) {
    if (v == INT64_MIN) throwIntOverflow("unary -");
    return -v;
}
