#pragma once

#include "type_id.h"
#include <variant>
#include <string>

class Value{
    public:
        explicit Value(int64_t v);
        explicit Value(double v);
        explicit Value(std::string v);

        TypeId type() const;

        // accessors
        int64_t asInt() const;
        double asDouble() const;
        const std::string& asString() const;

        void checkMatchingType(const Value& other) const;

        // comparisions
        bool operator==(const Value& other) const;
        bool operator!=(const Value& other) const;
        bool operator<(const Value& other) const;
        bool operator>(const Value& other) const;
        bool operator<=(const Value& other) const;
        bool operator>=(const Value& other) const;

        std::string toString() const;

    private:
        std::variant<int64_t, double, std::string> data_;
};