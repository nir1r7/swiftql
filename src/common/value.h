#pragma once

#include "type_id.h"
#include <variant>
#include <string>

class Value{
    public:
        Value();                    // constructs a null value
        explicit Value(int64_t v);
        explicit Value(double v);
        explicit Value(std::string v);

        static Value null();
        bool isNull() const;

        TypeId type() const;

        // accessors
        int64_t asInt() const;
        double asDouble() const;
        const std::string& asString() const;
        double toNumeric() const; // coerces INT or DOUBLE to double

        // helper for comparisions
        void checkMatchingType(const Value& other) const;

        // SQL comparison — three-valued, so EVERY operator returns false when
        // either side is NULL (the caller is expected to test isNull() first, as
        // evaluate() does). These do NOT define an ordering over NULL; use
        // compareForSort() for that.
        bool operator==(const Value& other) const;
        bool operator!=(const Value& other) const;
        bool operator<(const Value& other) const;
        bool operator>(const Value& other) const;
        bool operator<=(const Value& other) const;
        bool operator>=(const Value& other) const;

        std::string toString() const;

    private:
        std::variant<int64_t, double, std::string> data_;
        bool is_null_;
};

// Total order for ORDER BY. Returns -1, 0, or +1.
//
// WHY THIS IS NOT Value::operator<
// The SQL comparison operators are three-valued: every one of them returns false
// when either operand is NULL. That makes NULL *equivalent* to every value under
// `!(a<b) && !(b<a)`, and equivalence then stops being transitive — 1 ≡ NULL and
// NULL ≡ 2, but 1 ≢ 2. A predicate like that is not a strict weak ordering, and
// handing it to std::stable_sort is undefined behaviour rather than merely odd
// NULL placement. In practice it inverted the order of the NON-NULL keys and
// dropped rows from the result set under LIMIT.
//
// So sorting needs its own total order, with NULL as a real element. SQLite treats
// NULL as smaller than every value: NULLs first ascending, last descending. Two
// NULLs are equal, which lets the next ORDER BY key break the tie.
int compareForSort(const Value& a, const Value& b);