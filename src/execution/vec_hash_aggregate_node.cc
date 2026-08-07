#include "execution/vec_hash_aggregate_node.h"
#include "execution/evaluator.h"
#include "execution/expression_executor.h"
#include "parser/expr_utils.h"
#include "execution/key_encoding.h"
#include <algorithm>
#include <chrono>
#include <numeric>

namespace {
    // The encoding is shared with every other key serializer (key_encoding.h):
    // length-prefixed so it is injective for any bytes, 'N' for NULL so a NULL
    // group cannot collide with the string value "NULL", and exact rather than
    // display text for DOUBLE — `%.15g` gave two distinct doubles one group.
    std::string serializeKey(const std::vector<Value>& key) {
        std::string result;
        for (const auto& v : key) appendGroupKeyField(result, v);
        return result;
    }

    double toDouble(const Value& v) {
        return v.type() == TypeId::DOUBLE ? v.asDouble() : static_cast<double>(v.asInt());
    }
}

VecHashAggregateNode::VecHashAggregateNode( std::unique_ptr<VecPlanNode> child, std::vector<GroupByColumn> group_by_cols, std::vector<AggregateSpec> specs, Schema output_schema) : child_(std::move(child)), group_by_cols_(std::move(group_by_cols)), specs_(std::move(specs)), output_schema_(std::move(output_schema)) {}

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

    // resolve column indices once outside the chunk loop; slot-first so a
    // qualified GROUP BY reads the named join side on shared column names.
    // -1 = expression group key (GROUP BY season - 1): evaluated per row.
    std::vector<int> group_idxs;
    for (const auto& g : group_by_cols_){
        if (g.expr) { group_idxs.push_back(-1); continue; }
        int idx = g.id.isResolved()
            ? child_schema.indexOf(g.column_name,
                                   g.id.localSlot("VecHashAggregateNode group key"))
            : -1;
        if (idx < 0) idx = child_schema.indexOf(g.column_name);
        group_idxs.push_back(idx);
    }

    // -1 = COUNT(*) (never read) or expression argument (evaluated per row)
    std::vector<int> agg_idxs;
    for (const auto& spec : specs_){
        if (spec.is_star || (spec.argument && spec.column.empty())) {
            agg_idxs.push_back(-1);
            continue;
        }
        // slot-aware: distinguishes join sides sharing a column name (e.g. AVG(l2.speed))
        int idx = spec.id.isResolved()
            ? child_schema.indexOf(spec.column,
                                   spec.id.localSlot("VecHashAggregateNode argument"))
            : -1;
        if (idx < 0) idx = child_schema.indexOf(spec.column);
        agg_idxs.push_back(idx);
    }

    // Compile expression group keys and expression aggregate arguments into
    // chunk-at-a-time executors, once for the whole scan. This is the hot path
    // for TPC-H revenue aggregates: SUM(l_extendedprice * (1 - l_discount))
    // over 6M rows used to pay a dynamic_cast tree walk per row.
    // A null entry means compile() declined the shape — that expression keeps
    // the per-row evaluate() path, so an uncovered shape is slow, not wrong.
    std::vector<std::unique_ptr<ExpressionExecutor>> group_execs(group_by_cols_.size());
    for (size_t gi = 0; gi < group_by_cols_.size(); ++gi) {
        if (group_by_cols_[gi].expr) {
            group_execs[gi] = ExpressionExecutor::compile(group_by_cols_[gi].expr.get(), child_schema);
        }
    }
    std::vector<std::unique_ptr<ExpressionExecutor>> arg_execs(specs_.size());
    for (size_t i = 0; i < specs_.size(); ++i) {
        if (!specs_[i].is_star && specs_[i].argument && specs_[i].column.empty()) {
            arg_execs[i] = ExpressionExecutor::compile(specs_[i].argument, child_schema);
        }
    }

    // A full Row is only needed for expressions that fell back to evaluate();
    // a compiled expression reads its dense result column by position instead.
    bool needs_row = false;
    for (size_t gi = 0; gi < group_by_cols_.size(); ++gi) {
        if (group_by_cols_[gi].expr && !group_execs[gi]) { needs_row = true; break; }
    }
    if (!needs_row) {
        for (size_t i = 0; i < specs_.size(); ++i) {
            if (!specs_[i].is_star && specs_[i].argument && specs_[i].column.empty()
                && !arg_execs[i]) { needs_row = true; break; }
        }
    }

    // per-chunk results of the compiled executors; null where none was compiled
    std::vector<const ColumnVector*> group_vecs(group_by_cols_.size(), nullptr);
    std::vector<const ColumnVector*> arg_vecs(specs_.size(), nullptr);

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

        // Evaluate every compiled expression once for the whole chunk, before
        // the row loop. This is the dispatch hoist: one typed loop per node per
        // chunk instead of a dynamic_cast tree walk per node per row.
        for (size_t gi = 0; gi < group_execs.size(); ++gi) {
            if (group_execs[gi]) group_vecs[gi] = &group_execs[gi]->execute(*chunk, *indices_ptr);
        }
        for (size_t i = 0; i < arg_execs.size(); ++i) {
            if (arg_execs[i]) arg_vecs[i] = &arg_execs[i]->execute(*chunk, *indices_ptr);
        }

        // Iterate by position, not by row index: executor results are dense in
        // selection order, so result index `pos` corresponds to chunk row `r`.
        const int n_rows = static_cast<int>(indices_ptr->size());
        for (int pos = 0; pos < n_rows; ++pos) {
            const int r = (*indices_ptr)[pos];
            stats.rows_in++;

            // full Row only for expressions that fell back to evaluate()
            Row tmp;
            if (needs_row) {
                tmp.reserve(chunk->columns.size());
                for (const auto& cv : chunk->columns) {
                    tmp.push_back(valueAt(cv, r));
                }
            }

            // build group key vector from group by columns
            std::vector<Value> key;
            key.reserve(group_idxs.size());
            for (size_t gi = 0; gi < group_idxs.size(); ++gi) {
                if (group_idxs[gi] < 0) {
                    if (group_vecs[gi]) {
                        key.emplace_back();
                        readColumnValue(*group_vecs[gi], pos, key.back());
                    } else {
                        key.push_back(evaluate(group_by_cols_[gi].expr.get(), tmp, child_schema));
                    }
                    continue;
                }
                // emplace-then-read avoids a temporary Value per key column per row
                key.emplace_back();
                readColumnValue(chunk->columns[group_idxs[gi]], r, key.back());
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
                if (ci < 0) {
                    // A plain-column spec reaching here means its column did not
                    // resolve against the child schema. Say so, with the same
                    // message the Volcano HashAggregateNode uses: falling through
                    // to evaluate() would report a confusing "column not found in
                    // schema" against an intentionally-empty Row instead.
                    // Unreachable today — Validator and buildScanSchema guarantee
                    // resolution — so this is the shape of a planner bug.
                    if (!spec.argument || !spec.column.empty()) {
                        throw std::runtime_error("aggregate input column not found: " + spec.column);
                    }
                    // expression argument (e.g. SUM(speed * 2)): read the
                    // pre-evaluated chunk result, or fall back to per-row
                    // evaluate() when compile() declined the shape
                    if (arg_vecs[i]) readColumnValue(*arg_vecs[i], pos, val);
                    else             val = evaluate(spec.argument, tmp, child_schema);
                } else {
                    readColumnValue(chunk->columns[ci], r, val);
                }

                if (val.isNull()) continue;

                Accumulator::SpecAccum& sa = acc.per_spec[i];
                sa.non_null_count++;
                if (spec.distinct) {
                    std::string dk;
                    appendGroupKeyField(dk, val);
                    sa.distinct_keys.insert(std::move(dk));
                }

                if (spec.function == "SUM" || spec.function == "AVG") {
                    sa.sum += toDouble(val);
                }
                // MIN/MAX are order statistics: they return an element of the
                // input domain, so they keep the argument's own Value (and type,
                // per aggregateResultType) instead of coercing to double.
                // Value's comparison operators coerce INT/DOUBLE and compare
                // STRING lexicographically, matching SQLite.
                if (spec.function == "MIN") {
                    if (sa.min_val.isNull() || val < sa.min_val) sa.min_val = val;
                }
                if (spec.function == "MAX") {
                    if (sa.max_val.isNull() || val > sa.max_val) sa.max_val = val;
                }
            }
        }

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    // SQL: a scalar aggregate (no GROUP BY) over empty input still emits one row
    // (COUNT -> 0, SUM/AVG/MIN/MAX -> NULL). A default accumulator yields exactly that.
    if (group_by_cols_.empty() && groups_.empty()) {
        auto [it, inserted] = groups_.try_emplace("");
        it->second.per_spec.resize(specs_.size());
        group_order_.push_back("");
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
                // Same three-way rule as HashAggregateNode (plan_nodes.cc):
                // COUNT(*) counts rows, COUNT(x) non-NULL values,
                // COUNT(DISTINCT x) distinct non-NULL values.
                int64_t n = spec.is_star  ? acc.count
                          : spec.distinct ? static_cast<int64_t>(sa.distinct_keys.size())
                                          : sa.non_null_count;
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
        ColumnVector cv = makeColumnVector(output_schema_.column(c).type);
        for (int i = start; i < start + count; ++i) {
            // a null aggregate result (SUM/AVG/MIN/MAX over a group with no
            // non-NULL input) is carried on the validity mask, not flattened
            // to a 0 / "NULL" sentinel
            appendColumnValue(cv, result_rows_[i][c]);
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
            if (group_by_cols_[i].expr) {
                s += exprToString(group_by_cols_[i].expr.get());
                continue;
            }
            if (!group_by_cols_[i].table_name.empty()) s += group_by_cols_[i].table_name + ".";
            s += group_by_cols_[i].column_name;
        }
        if (!specs_.empty()) s += ", ";
    }
    if (!specs_.empty()) {
        s += "agg=";
        for (size_t i = 0; i < specs_.size(); ++i) {
            if (i) s += ",";
            s += specs_[i].column.empty() && specs_[i].argument && !specs_[i].is_star
                ? specs_[i].output_name
                : specs_[i].function + "(" + (specs_[i].is_star ? "*" : specs_[i].column) + ")";
        }
    }
    return s + "] (materialize)";
}

std::vector<VecPlanNode*> VecHashAggregateNode::children() const {
    return {child_.get()};
}
