#include "execution/vec_project_node.h"
#include "execution/evaluator.h"
#include <chrono>
#include <numeric>

VecProjectNode::VecProjectNode(std::unique_ptr<VecPlanNode> child, std::vector<std::unique_ptr<Expr>> expressions, Schema output_schema) : child_(std::move(child)), expressions_(std::move(expressions)), output_schema_(std::move(output_schema)) {}

void VecProjectNode::open(){
    child_->open();
    prepare();
}

void VecProjectNode::close(){
    child_->close();
}

// Classify each output expression ONCE per query. This used to happen per chunk,
// and the expression case then walked the AST with dynamic_cast per row on a
// rebuilt Row — 153ms per million rows for SELECT speed*2 against 8.8ms for the
// plain column. Compiling here moves that dispatch out of the row loop entirely.
void VecProjectNode::prepare(){
    if (prepared_) return;
    prepared_ = true;

    const Schema& child_schema = child_->outputSchema();
    src_col_.assign(expressions_.size(), -1);
    compiled_.clear();
    compiled_.resize(expressions_.size());
    needs_row_ = false;

    for (int c = 0; c < static_cast<int>(expressions_.size()); ++c) {
        if (auto* cr = dynamic_cast<const ColumnRef*>(expressions_[c].get())){
            src_col_[c] = resolveColumnIndex(*cr, child_schema);
            if (src_col_[c] >= 0) continue;
            // an unresolved ColumnRef is not a plain column read; try to compile
            // it like any other expression so the error comes from one place
        }
        compiled_[c] = ExpressionExecutor::compile(expressions_[c].get(), child_schema);
        // compile() guarantees type() == inferExprType, and buildProjectSchema
        // derives the output type from the same function — but only take the
        // whole-column path when they agree exactly. A mismatch (e.g. a hand-built
        // schema declaring DOUBLE for an INT expression) falls back to the
        // per-value append, which narrows INT into a DOUBLE column and REFUSES
        // the narrowing when it would be observable (narrowToDoubleColumn,
        // vec_types.h). This is Pass 2's route for a mixed-branch CASE, which
        // compileNode declines on other grounds.
        if (compiled_[c] && compiled_[c]->type() != output_schema_.column(c).type) {
            compiled_[c].reset();
        }
        // An ARMED column must take the per-value path: the refusal lives in
        // appendColumnValue, and a compiled executor writes its typed vector
        // directly without ever appending a Value. compileNode already declines
        // every shape that can arm a column (it declines CaseExpr by name), so
        // this makes a coincidence deliberate rather than inventing a
        // requirement — and it costs nothing on the columns that are not armed,
        // which is all of them in almost every plan.
        if (compiled_[c] && intObservable(c)) {
            compiled_[c].reset();
        }
        if (!compiled_[c]) needs_row_ = true;
    }
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
    out_chunk_.columns.resize(output_schema_.size());
    out_chunk_.sel.indices.clear();
    out_chunk_.sel.size = 0;
    out_chunk_.filter_applied = false; // output is fully materialized; no filter pending

    const Schema& child_schema = child_->outputSchema();
    const int n_rows = static_cast<int>(indices_ptr->size());

    // Pass 1 — column at a time, no Row and no per-row dispatch.
    for (int c = 0; c < static_cast<int>(expressions_.size()); ++c) {
        if (src_col_[c] >= 0) {
            // plain column: gather the selected rows, carrying validity across
            const ColumnVector& src = filtered->columns[src_col_[c]];
            ColumnVector out = makeColumnVector(output_schema_.column(c).type);
            out.int_observable = intObservable(c);
            for (int i = 0; i < n_rows; ++i) {
                appendColumnValue(out, valueAt(src, (*indices_ptr)[i]));
            }
            out_chunk_.columns[c] = std::move(out);
        } else if (compiled_[c]) {
            // compiled expression: the executor's dense result IS the output
            // column, already the right type and carrying its own validity mask,
            // so copy it whole instead of appending value by value
            out_chunk_.columns[c] = compiled_[c]->execute(*filtered, *indices_ptr);
        } else {
            // Pass 2's column: the arming has to be stamped here, before the
            // row loop below appends the first Value into it.
            out_chunk_.columns[c] = makeColumnVector(output_schema_.column(c).type);
            out_chunk_.columns[c].int_observable = intObservable(c);
        }
    }

    // Pass 2 — only for expressions compile() declined: rebuild a Row per row and
    // call the scalar evaluate(), exactly as before.
    if (needs_row_) {
        for (int i = 0; i < n_rows; ++i) {
            const int r = (*indices_ptr)[i];
            Row tmp;
            tmp.reserve(filtered->columns.size());
            for (const auto& cv : filtered->columns){
                tmp.push_back(valueAt(cv, r));
            }
            for (int c = 0; c < static_cast<int>(expressions_.size()); ++c) {
                if (src_col_[c] >= 0 || compiled_[c]) continue;
                appendColumnValue(out_chunk_.columns[c],
                                  evaluate(expressions_[c].get(), tmp, child_schema));
            }
        }
    }

    out_chunk_.num_rows = n_rows;

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