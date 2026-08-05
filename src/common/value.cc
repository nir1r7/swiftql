#include "type_id.h"
#include "value.h"
#include <variant>
#include <string>
#include <stdexcept>
#include <cstdio>

Value::Value()                : data_(int64_t(0)), is_null_(true)  {}
Value::Value(int64_t v)      : data_{v},          is_null_(false) {}
Value::Value(double v)       : data_{v},          is_null_(false) {}
Value::Value(std::string v)  : data_{v},          is_null_(false) {}

Value Value::null() { return Value(); }
bool  Value::isNull() const { return is_null_; }

TypeId Value::type() const{
    if (is_null_) throw std::runtime_error("Cannot get type of null Value");
    switch(data_.index()){
        case 0: return TypeId::INT;
        case 1: return TypeId::DOUBLE;
        case 2: return TypeId::STRING;
        default:
            throw std::runtime_error("Unknown type in Value");
    }
}

int64_t Value::asInt() const {
    return std::get<int64_t>(data_);
}

void Value::checkMatchingType(const Value& other) const {
    if (data_.index() != other.data_.index()) {
        throw std::runtime_error("Type mismatch in Value comparison");
    }
}

double Value::asDouble() const {
    return std::get<double>(data_);
}

double Value::toNumeric() const {
    if (data_.index() == 0) return static_cast<double>(std::get<int64_t>(data_));
    return std::get<double>(data_);
}

const std::string& Value::asString() const {
    return std::get<std::string>(data_);
}

// When one operand is INT and the other is DOUBLE, coerce both to double.
// STRING vs anything else still throws.
#define NUMERIC_COERCE(op) \
    if (is_null_ || other.is_null_) return false; \
    if (data_.index() != other.data_.index()) { \
        if (data_.index() <= 1 && other.data_.index() <= 1) \
            return toNumeric() op other.toNumeric(); \
        throw std::runtime_error("Type mismatch in Value comparison"); \
    }

bool Value::operator==(const Value& other) const {
    NUMERIC_COERCE(==)
    return data_ == other.data_;
}

bool Value::operator!=(const Value& other) const {
    NUMERIC_COERCE(!=)
    return data_ != other.data_;
}

bool Value::operator<(const Value& other) const {
    NUMERIC_COERCE(<)
    return data_ < other.data_;
}

bool Value::operator>(const Value& other) const {
    NUMERIC_COERCE(>)
    return data_ > other.data_;
}

bool Value::operator<=(const Value& other) const {
    NUMERIC_COERCE(<=)
    return data_ <= other.data_;
}

bool Value::operator>=(const Value& other) const {
    NUMERIC_COERCE(>=)
    return data_ >= other.data_;
}

#undef NUMERIC_COERCE

int compareForSort(const Value& a, const Value& b) {
    const bool na = a.isNull(), nb = b.isNull();
    if (na || nb) {
        if (na && nb) return 0;      // two NULLs tie; the next sort key decides
        return na ? -1 : 1;          // NULL is smaller than every value (SQLite)
    }
    // both non-NULL: the SQL operators are a correct total order here, and they
    // coerce INT against DOUBLE. STRING against a number still throws, as it does
    // everywhere else.
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

std::string Value::toString() const {
    if (is_null_) return "NULL";
    switch(data_.index()){
        case 0: return std::to_string(std::get<int64_t>(data_));
        case 1: { char buf[32]; snprintf(buf, sizeof(buf), "%.15g", std::get<double>(data_)); return buf; }
        case 2: return std::get<std::string>(data_);
        default:
            throw std::runtime_error("Unknown type in Value");
    }
}