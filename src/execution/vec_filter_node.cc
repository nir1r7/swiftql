#include "execution/vec_filter_node.h"
#include "execution/columnar_eval.h"
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
    // child reuses its internal DataChunk on next call
    out_chunk_ = *raw;

    // pass input_sel so evalPredicate skips rows already rejected by an upstream filter
    const SelectionVector* input_sel = raw->filter_applied ? &raw->sel : nullptr;
    out_chunk_.sel = evalPredicate(predicate_.get(), *raw, child_->outputSchema(), input_sel);

    // mark that a filter was applied so VecProjectNode treats empty
    // sel.indices as "zero rows passed" rather than "all rows valid"
    out_chunk_.filter_applied = true;

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