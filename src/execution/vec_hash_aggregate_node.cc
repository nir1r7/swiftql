#include "execution/vec_hash_aggregate_node.h"
#include "execution/evaluator.h"
#include "execution/expression_executor.h"
#include "parser/expr_utils.h"
#include "execution/key_encoding.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <type_traits>

namespace {
    // The encoding is shared with every other key serializer (key_encoding.h):
    // length-prefixed so it is injective for any bytes, 'N' for NULL so a NULL
    // group cannot collide with the string value "NULL", and exact rather than
    // display text for DOUBLE — `%.15g` gave two distinct doubles one group.
    void serializeKey(const std::vector<Value>& key, std::string& out) {
        out.clear();
        for (const auto& v : key) appendGroupKeyField(out, v);
    }

    // `<len>` written straight into the buffer. std::to_string would give the
    // same digits but materializes a std::string to do it, once per key field
    // per row.
    void appendDecimal(std::string& out, size_t v) {
        char buf[20];
        char* p = buf + sizeof(buf);
        do { *--p = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        out.append(p, static_cast<size_t>(buf + sizeof(buf) - p));
    }

    // `<len>:<bytes>\x01` for bytes already in hand. Splitting this out is what
    // lets the STRING arm below prefix a chunk cell IN PLACE — appendGroupKeyField
    // has to go via keyFieldText, which returns the cell's text by value and so
    // copies every string key column of every input row.
    void appendLengthPrefixed(std::string& out, const char* bytes, size_t len) {
        appendDecimal(out, len);
        out += ':';
        out.append(bytes, len);
        out += '\x01';
    }

    // std::to_chars renders an integer with the same digits std::to_string does
    // (that is the guarantee that keeps this byte-identical to keyFieldText),
    // into a stack buffer rather than into a fresh std::string.
    void appendIntField(std::string& out, int64_t v) {
        char buf[24];
        const auto res = std::to_chars(buf, buf + sizeof(buf), v);
        appendLengthPrefixed(out, buf, static_cast<size_t>(res.ptr - buf));
    }

    // One key field read STRAIGHT OUT OF A COLUMN, byte-for-byte what
    // appendGroupKeyField(valueAt(cv, row)) writes — same 'N' for NULL, same
    // length prefix, and for DOUBLE the same keyFieldText rules (integral doubles
    // through the integer text so 7.0 keys with the INT 7; both NaN signs to
    // "nan" so a NaN is one group of its own; %.17g otherwise so two distinct
    // doubles never share a group).
    //
    // The point of the duplication is that it never builds a Value: the row loop
    // below used to construct one per key column per row, each a
    // std::variant<int64_t,double,std::string> copy of the cell, purely to read
    // its text back out. Dispatch is on the VARIANT alternative, not on cv.type,
    // matching readColumnValue.
    void appendGroupKeyFromColumn(std::string& out, const ColumnVector& cv, int row) {
        if (cv.isNull(row)) {
            out += 'N';
            out += '\x01';
            return;
        }
        std::visit([&](const auto& vec) {
            using T = std::decay_t<decltype(vec[0])>;
            if constexpr (std::is_same_v<T, std::string>) {
                const std::string& s = vec[row];
                appendLengthPrefixed(out, s.data(), s.size());
            } else if constexpr (std::is_same_v<T, double>) {
                const double d = vec[row];
                if (std::isnan(d)) {
                    appendLengthPrefixed(out, "nan", 3);
                } else if (std::isfinite(d) && std::trunc(d) == d &&
                           d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
                    appendIntField(out, static_cast<int64_t>(d));
                } else {
                    char buf[40];
                    const int n = snprintf(buf, sizeof(buf), "%.17g", d);
                    appendLengthPrefixed(out, buf, static_cast<size_t>(n));
                }
            } else {
                appendIntField(out, vec[row]);
            }
        }, cv.data);
    }

    double toDouble(const Value& v) {
        return v.type() == TypeId::DOUBLE ? v.asDouble() : static_cast<double>(v.asInt());
    }

    // Which accumulator update an AggregateSpec wants, decided ONCE per query
    // instead of per spec per row. The inner loop used to ask
    // `spec.function == "SUM"` and up to three more std::string comparisons for
    // every aggregate of every input row; on q1 that is eight specs times 60k
    // rows. COUNT needs no update beyond non_null_count, which the loop already
    // maintains for every kind.
    enum class AggKind { COUNT, SUM_AVG, MIN, MAX };

    AggKind aggKindOf(const std::string& function) {
        if (function == "SUM" || function == "AVG") return AggKind::SUM_AVG;
        if (function == "MIN") return AggKind::MIN;
        if (function == "MAX") return AggKind::MAX;
        return AggKind::COUNT;
    }
}

VecHashAggregateNode::VecHashAggregateNode( std::unique_ptr<VecPlanNode> child, std::vector<GroupByColumn> group_by_cols, std::vector<AggregateSpec> specs, Schema output_schema) : child_(std::move(child)), group_by_cols_(std::move(group_by_cols)), specs_(std::move(specs)), output_schema_(std::move(output_schema)) {}

void VecHashAggregateNode::open() {
    child_->open();
    materialized_ = false;
    cursor_ = 0;
    groups_.clear();
    group_index_.clear();
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

    // Function-name dispatch, resolved once (see AggKind).
    std::vector<AggKind> agg_kinds;
    agg_kinds.reserve(specs_.size());
    for (const auto& spec : specs_) agg_kinds.push_back(aggKindOf(spec.function));

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

    // Every group key that is a plain column OR a compiled expression is a
    // ColumnVector cell, and the key can be serialized straight out of it. The
    // one key shape that cannot is an expression compile() declined, which only
    // exists as a per-row evaluate() into a Value. all_key_cols says no such key
    // is present, which is the case for every TPC-H group-by and lets the row
    // loop skip building a Value vector per row.
    // A compiled result is indexed by POSITION (dense in selection order); a
    // plain column by the chunk ROW index. src.by_pos carries which.
    struct KeySource { const ColumnVector* cv; bool by_pos; };
    std::vector<KeySource> key_srcs(group_by_cols_.size(), {nullptr, false});
    bool all_key_cols = true;
    for (size_t gi = 0; gi < group_idxs.size(); ++gi) {
        if (group_idxs[gi] < 0 && !group_execs[gi]) { all_key_cols = false; break; }
    }
    if (!all_key_cols) key_vals_.assign(group_idxs.size(), Value());

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

        // Bind each key to the chunk column it reads, once per chunk. The
        // executor results only exist after the loop above, so this cannot be
        // hoisted out of the chunk loop with the index resolution.
        if (all_key_cols) {
            for (size_t gi = 0; gi < group_idxs.size(); ++gi) {
                key_srcs[gi] = group_idxs[gi] < 0
                    ? KeySource{group_vecs[gi], true}
                    : KeySource{&chunk->columns[group_idxs[gi]], false};
            }
        }

        // Iterate by position, not by row index: executor results are dense in
        // selection order, so result index `pos` corresponds to chunk row `r`.
        const int n_rows = static_cast<int>(indices_ptr->size());
        Row tmp;   // reused; clear() keeps its capacity across rows
        for (int pos = 0; pos < n_rows; ++pos) {
            const int r = (*indices_ptr)[pos];
            stats.rows_in++;

            // full Row only for expressions that fell back to evaluate()
            if (needs_row) {
                tmp.clear();
                tmp.reserve(chunk->columns.size());
                for (const auto& cv : chunk->columns) {
                    tmp.push_back(valueAt(cv, r));
                }
            }

            // Serialize the group key into the reused buffer. The columnar path
            // writes each field straight from its column; only an expression
            // compile() declined needs a Value, and then only for that field.
            if (all_key_cols) {
                key_buf_.clear();
                for (size_t gi = 0; gi < key_srcs.size(); ++gi) {
                    appendGroupKeyFromColumn(key_buf_, *key_srcs[gi].cv,
                                             key_srcs[gi].by_pos ? pos : r);
                }
            } else {
                for (size_t gi = 0; gi < group_idxs.size(); ++gi) {
                    if (group_idxs[gi] < 0) {
                        if (group_vecs[gi]) readColumnValue(*group_vecs[gi], pos, key_vals_[gi]);
                        else key_vals_[gi] = evaluate(group_by_cols_[gi].expr.get(), tmp, child_schema);
                        continue;
                    }
                    readColumnValue(chunk->columns[group_idxs[gi]], r, key_vals_[gi]);
                }
                serializeKey(key_vals_, key_buf_);
            }

            // initialize accumulator on first encounter. find() before insert so
            // a repeat row — the overwhelmingly common case — never copies the
            // key string; only a genuinely new group pays for one.
            auto it = group_index_.find(key_buf_);
            uint32_t gidx;
            if (it == group_index_.end()) {
                gidx = static_cast<uint32_t>(groups_.size());
                groups_.emplace_back();
                Accumulator& fresh = groups_.back();
                fresh.per_spec.resize(specs_.size());
                if (all_key_cols) {
                    fresh.group_vals.resize(key_srcs.size());
                    for (size_t gi = 0; gi < key_srcs.size(); ++gi) {
                        readColumnValue(*key_srcs[gi].cv,
                                        key_srcs[gi].by_pos ? pos : r,
                                        fresh.group_vals[gi]);
                    }
                } else {
                    fresh.group_vals = key_vals_;
                }
                group_index_.emplace(key_buf_, gidx);
            } else {
                gidx = it->second;
            }

            Accumulator& acc = groups_[gidx];
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

                switch (agg_kinds[i]) {
                    case AggKind::SUM_AVG:
                        sa.sum += toDouble(val);
                        break;
                    // MIN/MAX are order statistics: they return an element of the
                    // input domain, so they keep the argument's own Value (and
                    // type, per aggregateResultType) instead of coercing to
                    // double. Value's comparison operators coerce INT/DOUBLE and
                    // compare STRING lexicographically, matching SQLite.
                    case AggKind::MIN:
                        if (sa.min_val.isNull() || val < sa.min_val) sa.min_val = val;
                        break;
                    case AggKind::MAX:
                        if (sa.max_val.isNull() || val > sa.max_val) sa.max_val = val;
                        break;
                    case AggKind::COUNT:
                        break;  // non_null_count above is the whole update
                }
            }
        }

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    // SQL: a scalar aggregate (no GROUP BY) over empty input still emits one row
    // (COUNT -> 0, SUM/AVG/MIN/MAX -> NULL). A default accumulator yields exactly that.
    if (group_by_cols_.empty() && groups_.empty()) {
        groups_.emplace_back();
        groups_.back().per_spec.resize(specs_.size());
    }
}

void VecHashAggregateNode::materializeResults() {
    auto t0 = std::chrono::high_resolution_clock::now();

    // emit in insertion order for stable output — groups_ is already in it
    for (const Accumulator& acc : groups_) {
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
        // Armed by the plan when an expression above divides with this column
        // BY AN INTEGER. MIN/MAX hand back the argument's own Value, so a
        // mixed-type CASE argument arrives here as an INT under a DOUBLE
        // declaration — see refuseObservableIntNarrowing in vec_types.h.
        cv.int_narrowing = c < static_cast<int>(int_narrowing_.size())
                               ? int_narrowing_[c] : IntNarrowing::RENDERED;
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
