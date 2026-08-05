#pragma once

#include "planner/vec_plan_node.h"
#include "execution/expression_executor.h"
#include "parser/ast.h"
#include "common/schema.h"
#include <memory>
#include <vector>

class VecProjectNode : public VecPlanNode {
    public:
        VecProjectNode(std::unique_ptr<VecPlanNode> child, std::vector<std::unique_ptr<Expr>> expressions, Schema output_schema);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

    private:
        // Classify each output expression once, in open(), rather than per chunk.
        void prepare();

        std::unique_ptr<VecPlanNode> child_;
        std::vector<std::unique_ptr<Expr>> expressions_;
        Schema output_schema_;
        DataChunk out_chunk_; // rebuilt every call

        // Per-expression plan, parallel to expressions_. Exactly one of the three
        // applies to each output column:
        //   src_col_[c] >= 0   plain ColumnRef — copy straight from the source column
        //   compiled_[c]       chunk-at-a-time executor
        //   otherwise          scalar evaluate() per row, needing a rebuilt Row
        std::vector<int> src_col_;
        std::vector<std::unique_ptr<ExpressionExecutor>> compiled_;
        bool needs_row_ = false;   // any column on the scalar path
        bool prepared_ = false;
};
