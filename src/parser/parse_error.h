#pragma once

#include "token.h"
#include <stdexcept>
#include <string>

class ParseError : public std::runtime_error {
    public:
        explicit ParseError(const std::string& message, const Token& token) : std::runtime_error(
            "Parse error at line " + std::to_string(token.line) +
            ", col " + std::to_string(token.col) +
            ": " + message +
            " (got '" + token.value + "')")
        {}
};