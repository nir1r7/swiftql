#pragma once

#include "common/schema.h"
#include "common/value.h"
#include <vector>
#include <memory>

using Row = std::vector<Value>;

struct NodeStats {
    int rows_in  = 0;
    int rows_out = 0;
    double elapsed_us = 0.0;
};

class PlanNode {
public:
    NodeStats stats;
    
    virtual ~PlanNode() = default;

    virtual void open() = 0;
    virtual Row* next() = 0;
    virtual void close() = 0;

    // the schema of rows the node outputs
    virtual const Schema& outputSchema() const = 0;

    // legible label for this node
    virtual std::string explain() const = 0;

    // immediate children of this node
    virtual std::vector<PlanNode*> children() const = 0;
};