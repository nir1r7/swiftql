#include "execution/vec_filter_node.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include <chrono>

VecFilterNode::VecFilterNode(std::unique_ptr<VecPlanNode> child, std::unique_ptr<Expr> predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}

void VecFilterNode::open(){
    child_->open();
}

void VecFilterNode::close(){
    child_->close();
}

DataChunk* VecFilterNode::nextChunk(){
    DataChunk* raw = child_->nextChunk();
    if (!raw){
        return nullptr;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    // copy child columns into our owned buffer
    // child reuses its internal DataCHunk on next call
    out_chunk_ = *raw;
    out_chunk_.sel.indices.clear();

    const Schema& schema = child_->outputSchema();

    // tight loop to evaluate predicate over every row in the batch.
    for (int r = 0; r < raw->num_rows; ++r) {
        // reconstruct a temporary Row for this row idx
        Row tmp;
        tmp.reserve(raw->columns.size());
        for (const auto& cv : raw->columns) {
            std::visit([&](const auto& vec) {
                tmp.push_back(Value(vec[r]));
            }, cv.data);
        }
        Value v = evaluate(predicate_.get(), tmp, schema);
        if (!v.isNull() && v.asInt() != 0) {
            out_chunk_.sel.indices.push_back(r);
        }
    }

    stats.rows_in  += raw->num_rows;
    stats.rows_out += static_cast<int>(out_chunk_.sel.indices.size());
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    return &out_chunk_;
}

const Schema& VecFilterNode::outputSchema() const {
    return child_->outputSchema();
}

std::string VecFilterNode::explain() const{
    return "VecFilter [" + exprToString(predicate_.get()) + "]";
}

std::vector<VecPlanNode*> VecFilterNode::children() const {
    return {child_.get()};
}