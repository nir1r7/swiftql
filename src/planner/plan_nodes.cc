#include "plan_nodes.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include "storage/chunk_pruner.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <chrono>


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


// SeqScanNode
SeqScanNode::SeqScanNode(std::string table_name, std::vector<Row> rows, Schema schema) : table_name_(std::move(table_name)), rows_(std::move(rows)), schema_(std::move(schema)), cursor_(0) {}

SeqScanNode::SeqScanNode(std::string table_name, ColumnarTable columnar_table, Schema schema, const Expr* pruning_where) : table_name_(std::move(table_name)), columnar_table_(std::move(columnar_table)), schema_(std::move(schema)), cursor_(0), use_columnar_(true), pruning_where_(pruning_where) {}


void SeqScanNode::open() {
    cursor_ = 0;
    skipped_chunks_ = 0;
}

Row* SeqScanNode::next() {
    auto t0 = std::chrono::high_resolution_clock::now();
    Row* row = nullptr;

    if (use_columnar_){
        while (cursor_ < columnar_table_.num_rows) {
            // decide whether to skip the entire chunk
            if (pruning_where_ && cursor_ % CHUNK_SIZE == 0) {
                int chunk_idx = cursor_/CHUNK_SIZE;
                if (ChunkPruner::shouldSkip(pruning_where_, columnar_table_.zone_maps, chunk_idx)) {
                    // advance past the whole chunk, never call getValue()
                    cursor_ += std::min(CHUNK_SIZE, columnar_table_.num_rows - cursor_);
                    ++skipped_chunks_;
                    continue;
                }
            }
            // not skipped, reconstruct this row and return it
            reconstructed_row_.clear();
            for (int c = 0; c < schema_.size(); ++c){
                reconstructed_row_.push_back(columnar_table_.getValue(schema_.column(c).name, cursor_));
            }
            ++cursor_;
            row = &reconstructed_row_;
            break;
        }
    }
    else{
        if (cursor_ < static_cast<int>(rows_.size())){
            row = &rows_[cursor_++];
        }
    }

    if (row) stats.rows_out++;
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    
    return row;
}

void SeqScanNode::close() {}

const Schema& SeqScanNode::outputSchema() const {
    return schema_;
}

std::string SeqScanNode::explain() const {
    std::string s = "SeqScan [" + table_name_ + ", " + std::to_string(schema_.columns().size()) + " columns]";
    if (use_columnar_ && pruning_where_){
        int total = (columnar_table_.num_rows + CHUNK_SIZE - 1) / CHUNK_SIZE;
        s += " chunks_skipped=" + std::to_string(skipped_chunks_) + "/" + std::to_string(total);
    }
    return s;
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
    while (true) {
        Row* row = child_->next();  // child time excluded from self-clock
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!row) {
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            return nullptr;
        }
        stats.rows_in++;
        Value result = evaluate(predicate_.get(), *row, child_->outputSchema());
        // pass rows if predicate is true (not zero + not null)
        if (!result.isNull() && result.asInt() != 0) {
            stats.rows_out++;
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            return row;
        }
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }
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
    Row* row = child_->next();  // child time excluded from self-clock
    auto t0 = std::chrono::high_resolution_clock::now();
    if (!row) {
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
        return nullptr;
    }
    stats.rows_in++;
    current_row_.clear();
    current_row_.reserve(expressions_.size());

    for (const auto& expr : expressions_) {
        current_row_.push_back(evaluate(expr.get(), *row, child_->outputSchema()));
    }
    stats.rows_out++;
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
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
HashAggregateNode::HashAggregateNode(std::unique_ptr<PlanNode> child,std::vector<GroupByColumn> group_by_cols, std::vector<AggregateSpec> aggregates, Schema output_schema) : child_(std::move(child)), group_by_cols_(std::move(group_by_cols)), aggregates_(std::move(aggregates)), output_schema_(std::move(output_schema)), cursor_(0) {}

void HashAggregateNode::open() {
    child_->open();  // child time excluded from self-clock
    const Schema& child_schema = child_->outputSchema();

    std::unordered_map<std::string, std::vector<AggAccumulator>> accumulators;
    std::unordered_map<std::string, std::vector<Value>> group_keys;

    while (true) {
        Row* row = child_->next();  // child time excluded from self-clock
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!row) {
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            break;
        }
        stats.rows_in++;
        // build group key, resolved slot-first so a qualified GROUP BY reads
        // the named join side even when both sides share the column name
        std::vector<Value> key;
        for (const auto& g : group_by_cols_) {
            int idx = g.relation_slot >= 0
                ? child_schema.indexOf(g.column_name, g.relation_slot)
                : -1;
            if (idx < 0) idx = child_schema.indexOf(g.column_name);
            key.push_back((*row)[idx]);
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
                int agg_idx = spec.relation_slot >= 0
                    ? child_schema.indexOf(spec.column, spec.relation_slot)
                    : -1;
                if (agg_idx < 0) agg_idx = child_schema.indexOf(spec.column);
                Value val = (*row)[agg_idx];
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
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    // SQL: a scalar aggregate (no GROUP BY) over empty input still emits one row
    // (COUNT -> 0, SUM/AVG/MIN/MAX -> NULL). A default accumulator yields exactly that.
    if (group_by_cols_.empty() && accumulators.empty()) {
        accumulators[""].resize(aggregates_.size());
        group_keys[""] = {};
    }

    // materialize results — self-work only, no child calls
    auto t0 = std::chrono::high_resolution_clock::now();
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
                result_row.push_back(acc.non_null_count > 0 ? Value(acc.sum) : Value::null());
            } else if (spec.function == "AVG") {
                Value avg_val = acc.non_null_count > 0
                    ? Value(acc.sum / static_cast<double>(acc.non_null_count))
                    : Value::null();
                result_row.push_back(avg_val);
            } else if (spec.function == "MIN") {
                result_row.push_back(acc.min_val);
            } else if (spec.function == "MAX") {
                result_row.push_back(acc.max_val);
            }
        }
        results_.push_back(std::move(result_row));
    }
    cursor_ = 0;
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
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
        if (!group_by_cols_[i].table_name.empty()) s += group_by_cols_[i].table_name + ".";
        s += group_by_cols_[i].column_name;
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
    while (true) {
        Row* row = child_->next();  // child time excluded from self-clock
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!row) {
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            return nullptr;
        }
        stats.rows_in++;
        Value result = evaluate(predicate_.get(), *row, child_->outputSchema());
        if (!result.isNull() && result.asInt() != 0) {
            stats.rows_out++;
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            return row;
        }
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }
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
    while (true) {
        Row* row = child_->next();  // child time excluded from self-clock
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!row) {
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            return nullptr;
        }
        stats.rows_in++;
        std::string key;
        for (const auto& val : *row) {
            std::string s = val.toString();
            key += std::to_string(s.size());
            key += ':';
            key += s;
            key += '\x01';
        }
        if (seen_.insert(key).second) {
            // insert returns {iterator, bool}
            stats.rows_out++;
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            return row;
        }
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }
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
SortNode::SortNode(std::unique_ptr<PlanNode> child, std::vector<OrderByItem> order_by) : child_(std::move(child)), order_by_(std::move(order_by)), cursor_(0) {}

void SortNode::open() {
    child_->open();  // child time excluded from self-clock
    sorted_rows_.clear();

    while (true) {
        Row* row = child_->next();  // child time excluded from self-clock
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!row) {
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            break;
        }
        stats.rows_in++;
        sorted_rows_.push_back(*row);  // copy
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    // sort — self-work only, no child calls
    auto t0 = std::chrono::high_resolution_clock::now();
    const Schema& schema = child_->outputSchema();
    std::stable_sort(sorted_rows_.begin(), sorted_rows_.end(), [&](const Row& a, const Row& b) {
        for (const auto& item : order_by_) {
            Value va = evaluate(item.expr.get(), a, schema);
            Value vb = evaluate(item.expr.get(), b, schema);
            if (va < vb) return !item.desc;
            if (vb < va) return  item.desc;
        }
        return false;
    });
    cursor_ = 0;
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
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
    for (size_t i = 0; i < order_by_.size(); ++i) {
        if (i) s += ", ";
        s += exprToString(order_by_[i].expr.get());
        if (order_by_[i].desc) s += " DESC";
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
    // self-work: check limit
    auto t0 = std::chrono::high_resolution_clock::now();
    if (count_ >= limit_) {
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
        return nullptr;
    }
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    Row* row = child_->next();  // child time excluded from self-clock

    // self-work: update count
    t0 = std::chrono::high_resolution_clock::now();
    if (row) {
        stats.rows_in++;
        stats.rows_out++;
        ++count_;
    }
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

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
HashJoinNode::HashJoinNode(std::unique_ptr<PlanNode> left, std::unique_ptr<PlanNode> right, std::string left_col, std::string right_col, Schema output_schema, bool swapped) : left_(std::move(left)), right_(std::move(right)), left_col_(std::move(left_col)), right_col_(std::move(right_col)), output_schema_(std::move(output_schema)), swapped_(swapped) {}

void HashJoinNode::open() {
    left_->open();
    right_->open();

    // build phase
    hash_table_.clear();
    const Schema& build_schema = right_->outputSchema();
    int build_key_idx = build_schema.indexOf(right_col_);

    while (true){
        Row* row = right_->next();
        auto t0 = std::chrono::high_resolution_clock::now();

        if (!row){
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            break;
        }

        const Value& key_val = (*row)[build_key_idx];
        std::string key = key_val.toString() + '\x01'; // same operator that serializeKey() uses
        hash_table_[key].push_back(*row);

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    current_probe_row_ = nullptr;
    bucket_idx_ = 0;
}

Row* HashJoinNode::next() {
    const Schema& probe_schema = left_->outputSchema();
    int probe_key_idx = probe_schema.indexOf(left_col_);

    while (true){
        if (current_probe_row_ != nullptr){
            // check if there are more matches in the current probe row's bucket
            std::string key = (*current_probe_row_)[probe_key_idx].toString() + '\x01';
            auto it = hash_table_.find(key);

            if (it != hash_table_.end() && bucket_idx_ < static_cast<int>(it->second.size())){
                auto t0 = std::chrono::high_resolution_clock::now();

                const Row& build_row = it->second[bucket_idx_++];

                current_row_.clear();
                if (swapped_) {
                    for (const Value& v : build_row) current_row_.push_back(v);
                    for (const Value& v : *current_probe_row_) current_row_.push_back(v);
                } else {
                    for (const Value& v : *current_probe_row_) current_row_.push_back(v);
                    for (const Value& v : build_row) current_row_.push_back(v);
                }

                stats.rows_out++;
                stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
                return &current_row_;
            } else {
                current_probe_row_ = nullptr;
            }
        } else {
            // fetch next probe row
            Row* probe_row = left_->next();
            auto t0 = std::chrono::high_resolution_clock::now();

            if (!probe_row){
                stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
                return nullptr;
            }

            stats.rows_in++;
            current_probe_row_ = probe_row;
            bucket_idx_ = 0;

            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
        }
    }
}

void HashJoinNode::close() {
    left_->close();
    right_->close();
    hash_table_.clear();
}

const Schema& HashJoinNode::outputSchema() const {
    return output_schema_;
}

std::string HashJoinNode::explain() const {
    return "HashJoin [" + left_col_ + " = " + right_col_ + "]";
}

std::vector<PlanNode*> HashJoinNode::children() const {
    return {left_.get(), right_.get()};
}