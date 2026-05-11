#include "plan_nodes.h"
#include "execution/evaluator.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>

// SeqScanNode
SeqScanNode::SeqScanNode(std::vector<Row> rows, Schema schema) : rows_(std::move(rows)), schema_(std::move(schema)), cursor_(0) {}

void SeqScanNode::open() {
    cursor_ = 0;
}

Row* SeqScanNode::next() {
    if (cursor_ < static_cast<int>(rows_.size())) {
        return &rows_[cursor_++];
    }
    return nullptr;
}

void SeqScanNode::close() {}

const Schema& SeqScanNode::outputSchema() const {
    return schema_;
}


// FilterNode
FilterNode::FilterNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}

void FilterNode::open() {
    child_->open();
}

Row* FilterNode::next() {
    while (Row* row = child_->next()){
        Value result = evaluate(predicate_.get(), *row, child_->outputSchema());
        // pass rows if predicate is true (not zero + not null)
        if (!result.isNull() && result.asInt() != 0){
            return row;
        }
    }
    return nullptr;
}

void FilterNode::close() {
    child_->close();
}

const Schema& FilterNode::outputSchema() const {
    return child_->outputSchema();
}


// ProjectNode
ProjectNode::ProjectNode(std::unique_ptr<PlanNode> child, std::vector<std::unique_ptr<Expr>> expressions, Schema output_schema) : child_(std::move(child)), expressions_(std::move(expressions)), output_schema_(std::move(output_schema)) {}

void ProjectNode::open() {
    child_->open();
}

Row* ProjectNode::next() {
    Row* row = child_->next();
    if (!row) return nullptr;

    current_row_.clear();
    current_row_.reserve(expressions_.size());

    for (const auto& expr : expressions_){
        current_row_.push_back(evaluate(expr.get(), *row, child_->outputSchema()));
    }
    return &current_row_;
}

void ProjectNode::close() {
    child_->close();
}

const Schema& ProjectNode::outputSchema() const {
    return output_schema_;
}


// HashAggregateNode
HashAggregateNode::HashAggregateNode(std::unique_ptr<PlanNode> child,std::vector<std::string> group_by_cols, std::vector<AggregateSpec> aggregates, Schema output_schema) : child_(std::move(child)), group_by_cols_(std::move(group_by_cols)), aggregates_(std::move(aggregates)), output_schema_(std::move(output_schema)), cursor_(0) {}

// anonymous namespace
// everything in the following is private to this file
namespace {
    struct AggAccumulator {
        int64_t count = 0;
        int64_t non_null_count = 0;
        double sum = 0.0;
        Value min_val;
        Value max_val;
    };

    std::string serializeKey(const std::vector<Value>& key) {
        std::string result;
        for (const auto& v : key) {
            result += v.toString();
            result += '\x01'; // non-printable separator unlikely to appear in data
        }
        return result;
    }

    double toDouble(const Value& v) {
        return v.type() == TypeId::DOUBLE ? v.asDouble() : static_cast<double>(v.asInt());
    }
}

void HashAggregateNode::open() {
    child_->open();
    const Schema& child_schema = child_->outputSchema();

    std::unordered_map<std::string, std::vector<AggAccumulator>> accumulators;
    std::unordered_map<std::string, std::vector<Value>> group_keys;

    while (Row* row = child_->next()){
        // build group key
        std::vector<Value> key;
        for (const auto& col : group_by_cols_){
            key.push_back((*row)[child_schema.indexOf(col)]);
        }
        std::string key_str = serializeKey(key);

        // initialize accumulators on first time
        if (accumulators.find(key_str) == accumulators.end()) {
            accumulators[key_str].resize(aggregates_.size());
            group_keys[key_str] = key;
        }

        // update each aggregate
        for (size_t i = 0; i < aggregates_.size(); ++i) {
            const AggregateSpec& spec = aggregates_[i];
            auto& acc = accumulators[key_str][i];
            acc.count++;

            if (!spec.is_star) {
                Value val = (*row)[child_schema.indexOf(spec.column)];
                // skip NULLs (except COUNT(*))
                if (!val.isNull()) {
                    acc.non_null_count++;
                    double d = toDouble(val);
                    if (spec.function == "SUM" || spec.function == "AVG") {
                        acc.sum += d;
                    }
                    if (spec.function == "MIN") {
                        if (acc.min_val.isNull() || d < toDouble(acc.min_val)) acc.min_val = Value(d);
                    }
                    if (spec.function == "MAX") {
                        if (acc.max_val.isNull() || d > toDouble(acc.max_val)) acc.max_val = Value(d);
                    }
                }
            }
        }
    }

    // materialize results
    results_.clear();
    for (auto& [key_str, group_accs] : accumulators) {
        Row result_row = group_keys[key_str];   // group-by column values come first

        for (size_t i = 0; i < aggregates_.size(); ++i) {
            const AggregateSpec& spec = aggregates_[i];
            const AggAccumulator& acc = group_accs[i];

            if (spec.function == "COUNT") {
                int64_t n = spec.is_star ? acc.count : acc.non_null_count;
                result_row.push_back(Value(n));
            } else if (spec.function == "SUM") {
                result_row.push_back(Value(acc.sum));
            } else if (spec.function == "AVG") {
                double avg = acc.non_null_count > 0 ? acc.sum / static_cast<double>(acc.non_null_count) : 0.0;
                result_row.push_back(Value(avg));
            } else if (spec.function == "MIN") {
                result_row.push_back(acc.min_val);
            } else if (spec.function == "MAX") {
                result_row.push_back(acc.max_val);
            }
        }
        results_.push_back(std::move(result_row));
    }
    cursor_ = 0;
}

Row* HashAggregateNode::next() {
    if (cursor_ >= static_cast<int>(results_.size())) return nullptr;
    return &results_[cursor_++];
}

void HashAggregateNode::close() {
    child_->close();
}

const Schema& HashAggregateNode::outputSchema() const {
    return output_schema_;
}


// HavingNode
HavingNode::HavingNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}

void HavingNode::open() {
    child_->open();
}

Row* HavingNode::next() {
    while (Row* row = child_->next()){
        Value result = evaluate(predicate_.get(), *row, child_->outputSchema());
        if (!result.isNull() && result.asInt() != 0) return row;
    }
    return nullptr;
}

void HavingNode::close() {
    child_->close();
} 

const Schema& HavingNode::outputSchema() const {
    return child_->outputSchema();
}


// DistinctNode
DistinctNode::DistinctNode(std::unique_ptr<PlanNode> child) : child_(std::move(child)) {}

void DistinctNode::open() {
    child_->open();
    seen_.clear();
}

Row* DistinctNode::next() {
    while (Row* row = child_->next()){
        // serialize the row to a string key
        std::string key;
        for (const auto& val : *row){
            key += val.toString();
            key += '\x01';
        }
        if (seen_.insert(key).second){
            // insert returns {interator, bool}
            return row;
        }
    }
    return nullptr;
}

void DistinctNode::close() {
    child_->close();
    seen_.clear();
}

const Schema& DistinctNode::outputSchema() const { return child_->outputSchema(); }


// SortNode
SortNode::SortNode(std::unique_ptr<PlanNode> child, std::vector<std::string> sort_cols) : child_(std::move(child)), sort_cols_(std::move(sort_cols)), cursor_(0) {}

void SortNode::open() {
    child_->open();
    sorted_rows_.clear();

    while (Row* row = child_->next()) {
        sorted_rows_.push_back(*row);  // copy
    }

    const Schema& schema = child_->outputSchema();

    std::sort(sorted_rows_.begin(), sorted_rows_.end(),[&](const Row& a, const Row& b) {
        for (const auto& col : sort_cols_) {
            int idx = schema.indexOf(col);
            if (a[idx] < b[idx]) return true;
            if (b[idx] < a[idx]) return false;
        }
        return false;  // equal
    });

    cursor_ = 0;
}

Row* SortNode::next() {
    if (cursor_ >= static_cast<int>(sorted_rows_.size())) return nullptr;
    return &sorted_rows_[cursor_++];
}

void SortNode::close() {
    child_->close();
}

const Schema& SortNode::outputSchema() const {
    return child_->outputSchema();
}


// HashJoinNode
LimitNode::LimitNode(std::unique_ptr<PlanNode> child, int limit) : child_(std::move(child)), limit_(limit), count_(0) {}

void LimitNode::open() {
    child_->open();
    count_ = 0;
}

Row* LimitNode::next() {
    if (count_ >= limit_) return nullptr;
    Row* row = child_->next();
    if (!row) return nullptr;
    ++count_;
    return row;
}

void LimitNode::close() {
    child_->close();
}

const Schema& LimitNode::outputSchema() const {
    return child_->outputSchema();
}


// HashJoinNode
HashJoinNode::HashJoinNode(std::unique_ptr<PlanNode> left, std::unique_ptr<PlanNode> right, std::string left_col, std::string right_col, Schema output_schema) : left_(std::move(left)), right_(std::move(right)), left_col_(std::move(left_col)), right_col_(std::move(right_col)), output_schema_(std::move(output_schema)) {}

void HashJoinNode::open() {
    throw std::runtime_error("HashJoin not implemented until Phase 2");
}

Row* HashJoinNode::next() { 
    return nullptr;
}

void HashJoinNode::close() {}

const Schema& HashJoinNode::outputSchema() const {
    return output_schema_;
}