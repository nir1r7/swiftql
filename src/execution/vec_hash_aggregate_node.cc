#include "execution/vec_hash_aggregate_node.h"
#include <algorithm>
#include <chrono>
#include <numeric>

namespace {
    // mirrors the anonymous namespace helpers in plan_nodes.cc
    std::string serializeKey(const std::vector<Value>& key) {
        std::string result;
        for (const auto& v : key) {
            std::string s = v.toString();
            result += std::to_string(s.size());
            result += ':';
            result += s;
            result += '\x01';
        }
        return result;
    }

    double toDouble(const Value& v) {
        return v.type() == TypeId::DOUBLE ? v.asDouble() : static_cast<double>(v.asInt());
    }
}

VecHashAggregateNode::VecHashAggregateNode( std::unique_ptr<VecPlanNode> child, std::vector<std::string> group_by_cols, std::vector<AggregateSpec> specs, Schema output_schema) : child_(std::move(child)), group_by_cols_(std::move(group_by_cols)), specs_(std::move(specs)), output_schema_(std::move(output_schema)) {}

void VecHashAggregateNode::open() {
    child_->open();
    materialized_ = false;
    cursor_ = 0;
    groups_.clear();
    group_order_.clear();
    result_rows_.clear();
}

void VecHashAggregateNode::consumeAll() {
    const Schema& child_schema = child_->outputSchema();

    // resolve column indices once outside the chunk loop
    std::vector<int> group_idxs;
    for (const auto& col : group_by_cols_){
        group_idxs.push_back(child_schema.indexOf(col));
    }

    std::vector<int> agg_idxs;
    for (const auto& spec : specs_){
        if (spec.is_star) { agg_idxs.push_back(-1); continue; }
        // slot-aware: distinguishes join sides sharing a column name (e.g. AVG(l2.speed))
        int idx = spec.relation_slot >= 0
            ? child_schema.indexOf(spec.column, spec.relation_slot)
            : -1;
        if (idx < 0) idx = child_schema.indexOf(spec.column);
        agg_idxs.push_back(idx);
    }

    while (DataChunk* chunk = child_->nextChunk()) {
        // child time excluded
        auto t0 = std::chrono::high_resolution_clock::now();

        // determine valid row indices
        const std::vector<int>* indices_ptr = nullptr;
        std::vector<int> all_indices;
        if (chunk->filter_applied) {
            indices_ptr = &chunk->sel.indices;
        }
        else {
            all_indices.resize(chunk->num_rows);
            std::iota(all_indices.begin(), all_indices.end(), 0);
            indices_ptr = &all_indices;
        }

        for (int r : *indices_ptr) {
            stats.rows_in++;

            // build group key vector from group by columns
            std::vector<Value> key;
            key.reserve(group_idxs.size());
            for (int ci : group_idxs) {
                std::visit([&](const auto& vec) {
                    key.push_back(Value(vec[r]));
                }, chunk->columns[ci].data);
            }
            std::string key_str = serializeKey(key);

            // initialize accumulator on first encounter
            auto [it, inserted] = groups_.try_emplace(key_str);
            if (inserted) {
                it->second.group_vals = key;
                it->second.per_spec.resize(specs_.size());
                group_order_.push_back(key_str);
            }

            Accumulator& acc = it->second;
            acc.count++;

            // update each aggregate
            for (size_t i = 0; i < specs_.size(); ++i) {
                const AggregateSpec& spec = specs_[i];
                if (spec.is_star) continue;  // COUNT(*), already handled by acc.count

                int ci = agg_idxs[i];
                Value val;
                std::visit([&](const auto& vec) { val = Value(vec[r]); }, chunk->columns[ci].data);

                if (val.isNull()) continue;

                Accumulator::SpecAccum& sa = acc.per_spec[i];
                sa.non_null_count++;
                double d = toDouble(val);

                if (spec.function == "SUM" || spec.function == "AVG") {
                    sa.sum += d;
                }
                if (spec.function == "MIN") {
                    if (sa.min_val.isNull() || d < toDouble(sa.min_val)){
                        sa.min_val = Value(d);
                    }
                }
                if (spec.function == "MAX") {
                    if (sa.max_val.isNull() || d > toDouble(sa.max_val)){
                        sa.max_val = Value(d);
                    }
                }
            }
        }

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }
}

void VecHashAggregateNode::materializeResults() {
    auto t0 = std::chrono::high_resolution_clock::now();

    // emit in insertion order for stable output
    for (const auto& key_str : group_order_) {
        const Accumulator& acc = groups_[key_str];

        // group by column values first (matching buildAggregateSchema order)
        Row row = acc.group_vals;

        for (size_t i = 0; i < specs_.size(); ++i) {
            const AggregateSpec& spec = specs_[i];
            const Accumulator::SpecAccum& sa = acc.per_spec[i];
            if (spec.function == "COUNT") {
                int64_t n = spec.is_star ? acc.count : sa.non_null_count;
                row.push_back(Value(n));
            }
            else if (spec.function == "SUM") {
                row.push_back(sa.non_null_count > 0 ? Value(sa.sum) : Value::null());
            }
            else if (spec.function == "AVG") {
                row.push_back(sa.non_null_count > 0 ? Value(sa.sum/static_cast<double>(sa.non_null_count)) : Value::null());
            }
            else if (spec.function == "MIN") {
                row.push_back(sa.min_val);
            }
            else if (spec.function == "MAX") {
                row.push_back(sa.max_val);
            }
        }

        result_rows_.push_back(std::move(row));
    }

    stats.rows_out = static_cast<int>(result_rows_.size());
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
}

void VecHashAggregateNode::fillChunk(int start, int count) {
    out_chunk_.columns.clear();
    out_chunk_.num_rows = count;
    out_chunk_.filter_applied = false;
    out_chunk_.sel.indices.clear();
    out_chunk_.sel.size = 0;

    for (int c = 0; c < output_schema_.size(); ++c) {
        ColumnVector cv;
        cv.type = output_schema_.column(c).type;
        switch (cv.type) {
            case TypeId::INT: cv.data = std::vector<int64_t>(); break;
            case TypeId::DOUBLE: cv.data = std::vector<double>(); break;
            case TypeId::STRING: cv.data = std::vector<std::string>(); break;
        }
        for (int i = start; i < start + count; ++i) {
            const Value& v = result_rows_[i][c];
            if (v.isNull()) {
                // TODO: ColumnVector has no null bitmap, so SQL NULL cannot be represented
                // in a DataChunk column. Null aggregate results (e.g. AVG over an empty group)
                // are emitted as 0 / 0.0 / "NULL" sentinels — indistinguishable from real
                // zero values or a string column containing "NULL". Fix requires a validity
                // vector on ColumnVector (Phase 4).
                switch (cv.type) {
                    case TypeId::INT:
                        std::get<std::vector<int64_t>>(cv.data).push_back(0); break;
                    case TypeId::DOUBLE:
                        std::get<std::vector<double>>(cv.data).push_back(0.0); break;
                    case TypeId::STRING:
                        std::get<std::vector<std::string>>(cv.data).push_back("NULL"); break;
                }
            } else {
                switch (cv.type) {
                    case TypeId::INT:
                        std::get<std::vector<int64_t>>(cv.data).push_back(v.asInt()); break;
                    case TypeId::DOUBLE:
                        std::get<std::vector<double>>(cv.data).push_back(v.asDouble()); break;
                    case TypeId::STRING:
                        std::get<std::vector<std::string>>(cv.data).push_back(v.asString()); break;
                }
            }
        }
        out_chunk_.columns.push_back(std::move(cv));
    }
}

DataChunk* VecHashAggregateNode::nextChunk() {
    if (!materialized_) {
        consumeAll();
        materializeResults();
        materialized_ = true;
    }

    if (cursor_ >= static_cast<int>(result_rows_.size())) return nullptr;

    int batch = std::min(BATCH_SIZE, static_cast<int>(result_rows_.size()) - cursor_);
    fillChunk(cursor_, batch);
    cursor_ += batch;
    return &out_chunk_;
}

void VecHashAggregateNode::close() {
    child_->close();
}

const Schema& VecHashAggregateNode::outputSchema() const {
    return output_schema_;
}

std::string VecHashAggregateNode::explain() const {
    std::string s = "VecHashAggregate [";
    if (!group_by_cols_.empty()) {
        s += "group_by=";
        for (size_t i = 0; i < group_by_cols_.size(); ++i) {
            if (i) s += ",";
            s += group_by_cols_[i];
        }
        if (!specs_.empty()) s += ", ";
    }
    if (!specs_.empty()) {
        s += "agg=";
        for (size_t i = 0; i < specs_.size(); ++i) {
            if (i) s += ",";
            s += specs_[i].function + "(" + (specs_[i].is_star ? "*" : specs_[i].column) + ")";
        }
    }
    return s + "] (materialize)";
}

std::vector<VecPlanNode*> VecHashAggregateNode::children() const {
    return {child_.get()};
}
