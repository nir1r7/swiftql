#pragma once

#include "common/schema.h"
#include <string>

struct TableMetadata {
    std::string name; 
    std::string filepath;
    Schema schema;
};