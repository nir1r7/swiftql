#include "type_id.h"
#include "value.h"
#include <variant>
#include <string>
#include <stdexcept>

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

const std::string& Value::asString() const {
    return std::get<std::string>(data_);
}

bool Value::operator==(const Value& other) const {
    if (is_null_ || other.is_null_) return false;
    checkMatchingType(other);
    return data_ == other.data_;
}

bool Value::operator!=(const Value& other) const {
    if (is_null_ || other.is_null_) return false;
    checkMatchingType(other);
    return data_ != other.data_;
}

bool Value::operator<(const Value& other) const {
    if (is_null_ || other.is_null_) return false;
    checkMatchingType(other);
    return data_ < other.data_;
}

bool Value::operator>(const Value& other) const {
    if (is_null_ || other.is_null_) return false;
    checkMatchingType(other);
    return data_ > other.data_;
}

bool Value::operator<=(const Value& other) const {
    if (is_null_ || other.is_null_) return false;
    checkMatchingType(other);
    return data_ <= other.data_;
}

bool Value::operator>=(const Value& other) const {
    if (is_null_ || other.is_null_) return false;
    checkMatchingType(other);
    return data_ >= other.data_;
}

std::string Value::toString() const {
    if (is_null_) return "NULL";
    switch(data_.index()){
        case 0: return std::to_string(std::get<int64_t>(data_));
        case 1: return std::to_string(std::get<double>(data_));
        case 2: return std::get<std::string>(data_);
        default:
            throw std::runtime_error("Unknown type in Value");
    }
}