#pragma once

#include "planner/vec_plan_node.h"
#include "storage/columnar_table.h"

class VecScanNode : public VecPlanNode {
    public:
        VecScanNode(std::string table_name, ColumnarTable columnar_table, Schema schema);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

    private:
        std::string table_name_;
        ColumnarTable columnar_table_;
        Schema schema_;
        int row_cursor_ = 0;
        DataChunk current_chunk_; // reused each call
};