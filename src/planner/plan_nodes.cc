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
            // NULL marker 'N' can never collide with the non-NULL encoding,
            // which always starts with a decimal length digit. Mirrors
            // serializeKey in vec_hash_aggregate_node.cc.
            if (v.isNull()) {
                result += 'N';
            } else {
                std::string s = v.toString();
                result += std::to_string(s.size());
                result += ':';
                result += s;
            }
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
    executed_ = true;
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
        // the counter is a runtime value; before execution only report that a
        // pruning hint is attached (plain --explain never runs the plan)
        if (executed_) {
            int total = (columnar_table_.num_rows + CHUNK_SIZE - 1) / CHUNK_SIZE;
            s += " chunks_skipped=" + std::to_string(skipped_chunks_) + "/" + std::to_string(total);
        } else {
            s += " pruning=on";
        }
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
        // the named join side even when both sides share the column name;
        // expression keys (GROUP BY season - 1) are evaluated per row
        std::vector<Value> key;
        for (const auto& g : group_by_cols_) {
            if (g.expr) {
                key.push_back(evaluate(g.expr.get(), *row, child_schema));
                continue;
            }
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
                Value val;
                if (spec.argument && spec.column.empty()) {
                    // expression argument (e.g. SUM(speed * 2)): evaluate per row
                    val = evaluate(spec.argument, *row, child_schema);
                } else {
                    int agg_idx = spec.relation_slot >= 0
                        ? child_schema.indexOf(spec.column, spec.relation_slot)
                        : -1;
                    if (agg_idx < 0) agg_idx = child_schema.indexOf(spec.column);
                    if (agg_idx < 0) {
                        // (*row)[-1] is out-of-bounds on the Row vector. Unreachable
                        // today (Validator + buildScanSchema guarantee the column is
                        // present), so this is the shape of a planner bug — and it
                        // matches the message the vectorized node raises for the same
                        // state, instead of reading past the end of a vector.
                        throw std::runtime_error(
                            "aggregate input column not found: " + spec.column);
                    }
                    val = (*row)[agg_idx];
                }
                // skip NULLs (except COUNT(*))
                if (!val.isNull()) {
                    acc.non_null_count++;
                    if (spec.function == "SUM" || spec.function == "AVG") {
                        acc.sum += toDouble(val);
                    }
                    // MIN/MAX are order statistics: they return an element of
                    // the input domain, so they keep the argument's own Value
                    // (and type, per aggregateResultType) rather than coercing
                    // to double, which crashed on STRING columns.
                    if (spec.function == "MIN") {
                        if (acc.min_val.isNull() || val < acc.min_val) acc.min_val = val;
                    }
                    if (spec.function == "MAX") {
                        if (acc.max_val.isNull() || val > acc.max_val) acc.max_val = val;
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
        if (group_by_cols_[i].expr) {
            s += exprToString(group_by_cols_[i].expr.get());
            continue;
        }
        if (!group_by_cols_[i].table_name.empty()) s += group_by_cols_[i].table_name + ".";
        s += group_by_cols_[i].column_name;
    }
    for (const auto& agg : aggregates_) {
        // expression arguments have no plain column; output_name is the full
        // FUNC((expr)) string
        s += ", agg=" + (agg.column.empty() && agg.argument && !agg.is_star
            ? agg.output_name
            : agg.function + "(" + (agg.is_star ? "*" : agg.column) + ")");
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
    // compareForSort, not Value::operator< — see the comment in value.h. Comparing
    // with the SQL operators is not a strict weak ordering once a sort key can be
    // NULL, which is undefined behaviour in stable_sort, not just odd placement.
    std::stable_sort(sorted_rows_.begin(), sorted_rows_.end(), [&](const Row& a, const Row& b) {
        for (const auto& item : order_by_) {
            int c = compareForSort(evaluate(item.expr.get(), a, schema),
                                   evaluate(item.expr.get(), b, schema));
            if (c != 0) return item.desc ? c > 0 : c < 0;
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
HashJoinNode::HashJoinNode(std::unique_ptr<PlanNode> left, std::unique_ptr<PlanNode> right, std::vector<std::string> left_cols, std::vector<std::string> right_cols, Schema output_schema, bool swapped) : left_(std::move(left)), right_(std::move(right)), left_cols_(std::move(left_cols)), right_cols_(std::move(right_cols)), output_schema_(std::move(output_schema)), swapped_(swapped) {}

namespace {

// Serialize one row's k-key tuple. Identical encoding to VecHashJoinNode's: the
// '\x01' sentinel after every field is what keeps ("ab","c") and ("a","bc")
// apart, and a one-key tuple is byte-identical to the pre-Week-27 form.
//
// Returns false when any key member is NULL — SQL's NULL equals nothing, so the
// row can neither be inserted nor matched. Volcano used to bucket a NULL key
// under toString()'s "NULL", making NULL = NULL match where the vectorized path
// dropped it; unreachable today (CSV cannot express NULL and a join key comes
// straight off a scan), but Week 29's outer join puts real NULLs on join inputs.
bool serializeRowKey(const Row& row, const std::vector<int>& key_idx, std::string& out) {
    out.clear();
    for (int c : key_idx) {
        const Value& v = row[c];
        if (v.isNull()) return false;
        out += v.toString();
        out += '\x01';
    }
    return true;
}

} // namespace

void HashJoinNode::open() {
    left_->open();
    right_->open();

    // build phase
    hash_table_.clear();
    const Schema& build_schema = right_->outputSchema();
    right_key_idx_.clear();
    for (const std::string& c : right_cols_) right_key_idx_.push_back(build_schema.indexOf(c));

    // resolved here rather than per row in next(), which re-resolved the probe
    // key for every row it pulled
    const Schema& probe_schema = left_->outputSchema();
    left_key_idx_.clear();
    for (const std::string& c : left_cols_) left_key_idx_.push_back(probe_schema.indexOf(c));

    std::string key;
    while (true){
        Row* row = right_->next();
        auto t0 = std::chrono::high_resolution_clock::now();

        if (!row){
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            break;
        }

        if (serializeRowKey(*row, right_key_idx_, key)) {
            hash_table_[key].push_back(*row);
        }

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    current_probe_row_ = nullptr;
    bucket_idx_ = 0;
}

Row* HashJoinNode::next() {
    while (true){
        if (current_probe_row_ != nullptr){
            // check if there are more matches in the current probe row's bucket
            auto it = hash_table_.find(probe_key_);

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
            // a NULL key member matches nothing: leave current_probe_row_ null
            // so the loop pulls the next probe row instead of looking it up
            if (serializeRowKey(*probe_row, left_key_idx_, probe_key_)) {
                current_probe_row_ = probe_row;
                bucket_idx_ = 0;
            }

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
    // one key renders exactly the pre-Week-27 string
    std::string s = "HashJoin [";
    for (size_t i = 0; i < left_cols_.size(); ++i) {
        if (i) s += " AND ";
        s += left_cols_[i] + " = " + right_cols_[i];
    }
    return s + "]";
}

std::vector<PlanNode*> HashJoinNode::children() const {
    return {left_.get(), right_.get()};
}