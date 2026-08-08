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

        // Output columns whose INT -> DOUBLE narrowing is observable to a
        // division somewhere above this node. vectorized_plan_builder.cc
        // computes the mask over the logical plan; vec_types.h's
        // refuseObservableIntNarrowing enforces it. Empty — the default and the
        // case for every node in almost every plan — means none.
        void setIntObservableColumns(std::vector<bool> mask) { int_observable_ = std::move(mask); }

    private:
        // Classify each output expression once, in open(), rather than per chunk.
        void prepare();

        bool intObservable(int c) const {
            return c < static_cast<int>(int_observable_.size()) && int_observable_[c];
        }

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
        std::vector<bool> int_observable_;   // parallel to expressions_ when non-empty
        bool needs_row_ = false;   // any column on the scalar path
        bool prepared_ = false;
};
