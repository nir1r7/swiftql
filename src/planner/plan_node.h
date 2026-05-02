#pragma once

#include "common/schema.h"
#include "common/value.h"
#include <vector>
#include <memory>

using Row = std::vector<Value>;

class PlanNode {
public:
    virtual ~PlanNode() = default;

    virtual void open() = 0;
    virtual Row* next() = 0;
    virtual void close() = 0;

    // the schema of rows the node outputs
    virtual const Schema& outputSchema() const = 0;
};