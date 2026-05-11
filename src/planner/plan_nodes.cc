#include "plan_nodes.h"
#include "execution/evaluator.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <chrono>

// SeqScanNode
SeqScanNode::SeqScanNode(std::string table_name, std::vector<Row> rows, Schema schema) : table_name_(std::move(table_name)), rows_(std::move(rows)), schema_(std::move(schema)), cursor_(0) {}

void SeqScanNode::open() {
    cursor_ = 0;
}

Row* SeqScanNode::next() {
    auto t0 = std::chrono::high_resolution_clock::now();
    Row* row = nullptr;

    if (cursor_ < static_cast<int>(rows_.size())){
        row = &rows_[cursor_++];
    }
    if (row) stats.rows_out++;
    stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    
    return row;
}

void SeqScanNode::close() {}

const Schema& SeqScanNode::outputSchema() const {
    return schema_;
}

std::string SeqScanNode::explain() const {
    return "SeqScan [" + table_name_ + ", " + std::to_string(schema_.columns().size()) + " columns]";
}

std::vector<PlanNode*> SeqScanNode::children() const {
    return {};
}


// FilterNode
FilterNode::FilterNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}

void FilterNode::open() {
    child_->open();
}

Row* FilterNode::next() {
    auto t0 = std::chrono::high_resolution_clock::now();

    while (Row* row = child_->next()){
        Value result = evaluate(predicate_.get(), *row, child_->outputSchema());
        // pass rows if predicate is true (not zero + not null)
        if (!result.isNull() && result.asInt() != 0){
            stats.rows_out++;
            stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
            return row;
        }
    }
    
    stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    return nullptr;
}

void FilterNode::close() {
    child_->close();
}

const Schema& FilterNode::outputSchema() const {
    return child_->outputSchema();
}

std::string FilterNode::explain() const {
    return "Filter [" + exprToString(predicate_.get()) + "]";
}

std::vector<PlanNode*> FilterNode::children() const {
    return {child_.get()};
}


// ProjectNode
ProjectNode::ProjectNode(std::unique_ptr<PlanNode> child, std::vector<std::unique_ptr<Expr>> expressions, Schema output_schema) : child_(std::move(child)), expressions_(std::move(expressions)), output_schema_(std::move(output_schema)) {}

void ProjectNode::open() {
    child_->open();
}

Row* ProjectNode::next() {
    auto t0 = std::chrono::high_resolution_clock::now();
    Row* row = child_->next();
    if (!row){
        stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        return nullptr;
    }
    stats.rows_in++;
    current_row_.clear();
    current_row_.reserve(expressions_.size());

    for (const auto& expr : expressions_){
        current_row_.push_back(evaluate(expr.get(), *row, child_->outputSchema()));
    }
    stats.rows_out++;
    stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    return &current_row_;
}

void ProjectNode::close() {
    child_->close();
}

const Schema& ProjectNode::outputSchema() const {
    return output_schema_;
}

std::string ProjectNode::explain() const {
    std::string s = "Project [";
    for (size_t i = 0; i < output_schema_.columns().size(); ++i) {
        if (i) s += ", ";
        s += output_schema_.columns()[i].name;
    }
    return s + "]";
}

std::vector<PlanNode*> ProjectNode::children() const {
    return {child_.get()};
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

    std::string exprToString(const Expr* expr) {
        if (!expr) return "?";
        if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
            return col->table_name.empty()
                ? col->column_name
                : col->table_name + "." + col->column_name;
        }
        if (auto* lit = dynamic_cast<const Literal*>(expr)) {
            return lit->value.toString();
        }
        if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
            return exprToString(bin->left.get())
                + " " + bin->op + " "
                + exprToString(bin->right.get());
        }
        if (auto* n = dynamic_cast<const IsNullExpr*>(expr)) {
            return exprToString(n->operand.get())
                + (n->is_not_null ? " IS NOT NULL" : " IS NULL");
        }
        if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
            std::string arg = agg->is_star ? "*" : exprToString(agg->argument.get());
            return agg->function_name + "(" + arg + ")";
        }
        return "?";
    }

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
    auto t0 = std::chrono::high_resolution_clock::now();
    child_->open();
    const Schema& child_schema = child_->outputSchema();

    std::unordered_map<std::string, std::vector<AggAccumulator>> accumulators;
    std::unordered_map<std::string, std::vector<Value>> group_keys;

    while (Row* row = child_->next()){
        stats.rows_in++;
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
    stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
}

Row* HashAggregateNode::next() {
    if (cursor_ >= static_cast<int>(results_.size())) return nullptr;
    stats.rows_out++;
    return &results_[cursor_++];
}

void HashAggregateNode::close() {
    child_->close();
}

const Schema& HashAggregateNode::outputSchema() const {
    return output_schema_;
}

std::string HashAggregateNode::explain() const {
    std::string s = "Aggregate [group_by=";
    for (size_t i = 0; i < group_by_cols_.size(); ++i) {
        if (i) s += ", ";
        s += group_by_cols_[i];
    }
    for (const auto& agg : aggregates_) {
        s += ", agg=" + agg.function + "(" + (agg.is_star ? "*" : agg.column) + ")";
    }
    return s + "]";
}

std::vector<PlanNode*> HashAggregateNode::children() const {
    return {child_.get()};
}


// HavingNode
HavingNode::HavingNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}

void HavingNode::open() {
    child_->open();
}

Row* HavingNode::next() {
    auto t0 = std::chrono::high_resolution_clock::now();

    while (Row* row = child_->next()){
        stats.rows_in++;
        Value result = evaluate(predicate_.get(), *row, child_->outputSchema());
        if (!result.isNull() && result.asInt() != 0){
            stats.rows_out++;
            stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
            return row;
        }
    }

    stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    return nullptr;
}

void HavingNode::close() {
    child_->close();
} 

const Schema& HavingNode::outputSchema() const {
    return child_->outputSchema();
}

std::string HavingNode::explain() const {
    return "Having [" + exprToString(predicate_.get()) + "]";
}

std::vector<PlanNode*> HavingNode::children() const {
    return {child_.get()};
}


// DistinctNode
DistinctNode::DistinctNode(std::unique_ptr<PlanNode> child) : child_(std::move(child)) {}

void DistinctNode::open() {
    child_->open();
    seen_.clear();
}

Row* DistinctNode::next() {
    auto t0 = std::chrono::high_resolution_clock::now();
    while (Row* row = child_->next()){
        stats.rows_in++;
        // serialize the row to a string key
        std::string key;
        for (const auto& val : *row){
            key += val.toString();
            key += '\x01';
        }
        if (seen_.insert(key).second){
            // insert returns {interator, bool}
            stats.rows_out++;
            stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
            return row;
        }
    }
    stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    return nullptr;
}

void DistinctNode::close() {
    child_->close();
    seen_.clear();
}

const Schema& DistinctNode::outputSchema() const {
    return child_->outputSchema();
}

std::string DistinctNode::explain() const {
    return "Distinct";
}

std::vector<PlanNode*> DistinctNode::children() const {
    return {child_.get()};
}


// SortNode
SortNode::SortNode(std::unique_ptr<PlanNode> child, std::vector<std::string> sort_cols) : child_(std::move(child)), sort_cols_(std::move(sort_cols)), cursor_(0) {}

void SortNode::open() {
    auto t0 = std::chrono::high_resolution_clock::now();
    child_->open();
    sorted_rows_.clear();

    while (Row* row = child_->next()) {
        stats.rows_in++;
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
    stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
}

Row* SortNode::next() {
    if (cursor_ >= static_cast<int>(sorted_rows_.size())) return nullptr;
    stats.rows_out++;
    return &sorted_rows_[cursor_++];
}

void SortNode::close() {
    child_->close();
}

const Schema& SortNode::outputSchema() const {
    return child_->outputSchema();
}

std::string SortNode::explain() const {
    std::string s = "Sort [";
    for (size_t i = 0; i < sort_cols_.size(); ++i) {
        if (i) s += ", ";
        s += sort_cols_[i];
    }
    return s + "]";
}

std::vector<PlanNode*> SortNode::children() const {
    return {child_.get()};
}


// LimitNode
LimitNode::LimitNode(std::unique_ptr<PlanNode> child, int limit) : child_(std::move(child)), limit_(limit), count_(0) {}

void LimitNode::open() {
    child_->open();
    count_ = 0;
}

Row* LimitNode::next() {
    auto t0 = std::chrono::high_resolution_clock::now();

    if (count_ >= limit_){
        stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        return nullptr;
    }
    Row* row = child_->next();
    if (row) {
        stats.rows_in++;
        stats.rows_out++;
        ++count_;
    }
    stats.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    
    return row;
}

void LimitNode::close() {
    child_->close();
}

const Schema& LimitNode::outputSchema() const {
    return child_->outputSchema();
}

std::string LimitNode::explain() const {
    return "Limit [" + std::to_string(limit_) + "]";
}

std::vector<PlanNode*> LimitNode::children() const {
    return {child_.get()};
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

std::string HashJoinNode::explain() const {
    return "HashJoin [" + left_col_ + " = " + right_col_ + "]";
}

std::vector<PlanNode*> HashJoinNode::children() const {
    return {left_.get(), right_.get()};
}