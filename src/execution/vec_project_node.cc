#include "execution/vec_project_node.h"
#include "execution/evaluator.h"
#include <chrono>
#include <stdexcept>
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

    // determine which row indices to materialize
    // empty sel.indices means "all rows"
    const std::vector<int>* indices_ptr = nullptr;
    std::vector<int> all_indices;

    if (!filtered->sel.indices.empty()){
        indices_ptr = &filtered->sel.indices;
    }
    else{
        all_indices.resize(filtered->num_rows);
        std::iota(all_indices.begin(), all_indices.end(), 0);
        indices_ptr = &all_indices;
    }

    // rebuild output columns
    out_chunk_.columns.clear();
    out_chunk_.sel.indices.clear();

    // pre allocate one COlumnVector per output expression
    for (int c = 0; c < output_schema_.size(); ++c){
        ColumnVector cv;
        cv.type = output_schema_.column(c).type;
        switch (cv.type){
            case TypeId::INT:
                cv.data = std::vector<int64_t>(); break;
            case TypeId::DOUBLE:
                cv.data = std::vector<double>(); break;
            case TypeId::STRING:
                cv.data = std::vector<std::string>(); break;
        }
        out_chunk_.columns.push_back(std::move(cv));
    }

    const Schema& child_schema = child_->outputSchema();

    // reconstruct survivings rows
    for (int r : *indices_ptr){
        Row tmp;
        tmp.reserve(filtered->columns.size());
        for (const auto& cv : filtered->columns){
            std::visit([&](const auto& vec){
                tmp.push_back(Value(vec[r]));
            }, cv.data);
        }

        for (int c = 0; c < static_cast<int>(expressions_.size()); ++c) {
            Value v = evaluate(expressions_[c].get(), tmp, child_schema);
            // Append v to the right ColumnVector using the output schema type.
            switch (output_schema_.column(c).type) {
                case TypeId::INT:
                    std::get<std::vector<int64_t>>(out_chunk_.columns[c].data).push_back(v.asInt()); break;
                case TypeId::DOUBLE:
                    std::get<std::vector<double>>(out_chunk_.columns[c].data).push_back(v.asDouble()); break;
                case TypeId::STRING:
                    std::get<std::vector<std::string>>(out_chunk_.columns[c].data).push_back(v.asString()); break;
            }
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