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

    // late materialization: stamp the SelectionVector onto the child's chunk
    // and pass the pointer through — no column data is copied. evalPredicate
    // returns a fresh vector by value, so reading raw->sel as input_sel (rows
    // already rejected upstream) before the assignment below is safe.
    const SelectionVector* input_sel = raw->filter_applied ? &raw->sel : nullptr;
    SelectionVector out = evalPredicate(predicate_.get(), *raw, child_->outputSchema(), input_sel);
    raw->sel = std::move(out);

    // mark that a filter was applied so VecProjectNode treats empty
    // sel.indices as "zero rows passed" rather than "all rows valid"
    raw->filter_applied = true;

    stats.rows_in  += raw->num_rows;
    stats.rows_out += static_cast<int>(raw->sel.indices.size());
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    return raw;
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