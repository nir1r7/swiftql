#include "execution/vec_project_node.h"
#include "execution/evaluator.h"
#include <chrono>
#include <numeric>

VecProjectNode::VecProjectNode(std::unique_ptr<VecPlanNode> child, std::vector<std::unique_ptr<Expr>> expressions, Schema output_schema) : child_(std::move(child)), expressions_(std::move(expressions)), output_schema_(std::move(output_schema)) {}

void VecProjectNode::open(){
    child_->open();
}

void VecProjectNode::close(){
    child_->close();
}

DataChunk* VecProjectNode::nextChunk(){
    DataChunk* filtered = child_->nextChunk();
    if (!filtered) return nullptr;

    auto t0 = std::chrono::high_resolution_clock::now();

    // determine which row indices to materialize.
    // filter_applied=true: sel.indices is authoritative (empty = 0 rows passed)
    // filter_applied=false (i.e, chunk from VecScan): all num_rows rows are valid
    const std::vector<int>* indices_ptr = nullptr;
    std::vector<int> all_indices;

    if (filtered->filter_applied) {
        indices_ptr = &filtered->sel.indices;
    } else {
        all_indices.resize(filtered->num_rows);
        std::iota(all_indices.begin(), all_indices.end(), 0);
        indices_ptr = &all_indices;
    }

    // rebuild output columns, must clear between calls since out_chunk_ is reused
    out_chunk_.columns.clear();
    out_chunk_.sel.indices.clear();
    out_chunk_.sel.size = 0;
    out_chunk_.filter_applied = false; // output is fully materialized; no filter pending

    // pre allocate one COlumnVector per output expression
    for (int c = 0; c < output_schema_.size(); ++c){
        out_chunk_.columns.push_back(makeColumnVector(output_schema_.column(c).type));
    }

    const Schema& child_schema = child_->outputSchema();

    // preclassify each output expression once before the per row loop
    // src_col[c] >= 0: pure ColumnRef, read directly from source column array, no Row needed
    // src_col[c] == -1: complex expression, requires evaluate(), which needs a full Row
    std::vector<int> src_col(expressions_.size(), -1);
    for (int c = 0; c < static_cast<int>(expressions_.size()); ++c) {
        if (auto* cr = dynamic_cast<const ColumnRef*>(expressions_[c].get())){
            src_col[c] = resolveColumnIndex(*cr, child_schema);
        }
    }
    bool has_complex = false;
    for (int idx : src_col) if (idx < 0) {
        has_complex = true;
        break;
    }

    for (int r : *indices_ptr) {
        // build the full Row only when at least one expression needs evaluate().
        Row tmp;
        if (has_complex) {
            tmp.reserve(filtered->columns.size());
            for (const auto& cv : filtered->columns){
                tmp.push_back(valueAt(cv, r));
            }
        }

        for (int c = 0; c < static_cast<int>(expressions_.size()); ++c) {
            Value v;
            if (src_col[c] >= 0) {
                // direct columnar read, no Row, no evaluate() call
                v = valueAt(filtered->columns[src_col[c]], r);
            } else {
                v = evaluate(expressions_[c].get(), tmp, child_schema);
            }
            // a NULL (e.g. x/0 under SQLite division semantics) is carried on the
            // column's validity mask instead of being flattened to a 0 / "NULL"
            // sentinel that is indistinguishable from a real value
            appendColumnValue(out_chunk_.columns[c], v);
        }
    }

    out_chunk_.num_rows = static_cast<int>(indices_ptr->size());

    stats.rows_in += static_cast<int>(indices_ptr->size());
    stats.rows_out += out_chunk_.num_rows;

    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    return &out_chunk_;
}

const Schema& VecProjectNode::outputSchema() const {
    return output_schema_;
}

std::string VecProjectNode::explain() const {
    // "(materialize)" marks this as the late materialization point in EXPLAIN output
    std::string s = "VecProject [";
    for (int i = 0; i < output_schema_.size(); ++i) {
        if (i) s += ", ";
        s += output_schema_.column(i).name;
    }
    return s + "] (materialize)";
}

std::vector<VecPlanNode*> VecProjectNode::children() const {
    return {child_.get()};
}