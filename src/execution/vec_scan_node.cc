#include "execution/vec_scan_node.h"
#include "execution/chunk_key.h"
#include "storage/rle_column.h"
#include "storage/dictionary_encoder.h"
#include "storage/chunk_pruner.h"
#include <algorithm>
#include <chrono>
#include <utility>

VecScanNode::VecScanNode(std::string table_name, std::shared_ptr<const ColumnarTable> columnar_table, Schema schema, const Expr* pruning_where, const Schema* hint_schema)
    : table_name_(std::move(table_name)), columnar_table_(std::move(columnar_table)), schema_(std::move(schema)), pruning_where_(pruning_where) {
    // Body, not the mem-init list: it defaults to `schema_`, and reading one
    // member from another there depends on DECLARATION order, not on the order
    // written in the list.
    hint_schema_ = hint_schema ? *hint_schema : schema_;
}

VecScanNode::VecScanNode(std::string table_name, ColumnarTable columnar_table, Schema schema, const Expr* pruning_where, const Schema* hint_schema)
    : VecScanNode(std::move(table_name),
                  std::make_shared<const ColumnarTable>(std::move(columnar_table)),
                  std::move(schema), pruning_where, hint_schema) {}

void VecScanNode::open(){
    row_cursor_ = 0;
    skipped_chunks_ = 0;
    bloom_rejected_ = 0;
    bloom_tested_ = 0;
    bloom_gave_up_ = false;
    executed_ = true;
    // bloom_ is deliberately NOT cleared here — see the member declaration. (A
    // re-opened plan whose filter was abandoned mid-run gets a fresh one: the
    // join pushes on every open(), and always after opening its probe child.)
}

// The keys are indices into the PROBE CHILD's schema, which for this node is its
// own. Out of range means the pushing join's probe pipeline is not the shape
// this scan can answer for, and the right response is to decline: a filter that
// is never installed costs a missed optimization, one installed against the
// wrong column costs the answer.
void VecScanNode::pushBloomFilter(const std::vector<int>& key_indices,
                                  std::shared_ptr<const BloomFilter> filter) {
    if (key_indices.empty()) return;
    for (int i : key_indices) {
        if (i < 0 || i >= schema_.size()) return;
    }
    // The INT64 mode is the join's claim about the key COLUMNS, and this side
    // checks it against its own schema rather than trusting it. They agree by
    // construction (the join read the schema of the child this push travelled
    // down from), so a disagreement is a plan-shape bug — and declining is the
    // response that cannot be wrong.
    if (filter && filter->mode() == BloomKeyMode::INT64
        && (key_indices.size() != 1
            || schema_.column(key_indices[0]).type != TypeId::INT)) {
        return;
    }
    bloom_key_idx_ = key_indices;
    bloom_armed_ = true;
    if (filter) bloom_ = std::move(filter);
}

// LATE MATERIALIZATION, exactly as VecFilterNode does it: the surviving rows are
// named by a SelectionVector and no column is copied or compacted.
//
// A scan is a LEAF, so its chunk never arrives carrying a selection — there is
// nothing to intersect with, and the loop starts from every physical row. (The
// zone-map skip above happens at whole-chunk granularity and never sets `sel`.)
//
// THE NULL RULE IS THE CONSERVATIVE ONE AND IT IS DELIBERATE. A row whose key
// contains a NULL (or a NaN) fails serializeKey, cannot match anything, and is
// PASSED THROUGH here to be rejected by the join exactly as it already is.
// Dropping it would be right for an inner join and is not this node's call to
// make: the filter carries no semantics, and the gate that decided a drop is
// safe lives at the push site.
//
// A CHUNK THAT LOSES NOTHING IS LEFT DENSE. Stamping an identity selection is
// not wrong, but it puts every operator above onto its gather-by-index path for
// no reduction at all — the case where the pushdown is pure overhead is exactly
// the case where it must add as little as possible.
void VecScanNode::applyBloomFilter(DataChunk& chunk) {
    std::vector<int>& out = chunk.sel.indices;
    out.clear();
    out.reserve(chunk.num_rows);

    // The INT64 fast path: no string is built, and this is where the mode pays
    // for itself — it is one hash and three bit tests per row against ~13ns of
    // key serialization. The column is INT by the check in pushBloomFilter.
    if (bloom_->mode() == BloomKeyMode::INT64) {
        const ColumnVector& col = chunk.columns[bloom_key_idx_[0]];
        const auto& data = std::get<std::vector<int64_t>>(col.data);
        for (int r = 0; r < chunk.num_rows; ++r) {
            // A NULL key matches nothing and is passed through anyway — see the
            // NULL rule above — so it never reaches maybeContainsInt.
            if (col.isNull(r) || bloom_->maybeContainsInt(data[r])) out.push_back(r);
        }
    } else {
        for (int r = 0; r < chunk.num_rows; ++r) {
            if (!chunk_key::serializeKey(chunk, bloom_key_idx_, r, bloom_key_buf_)
                || bloom_->maybeContains(bloom_key_buf_)) {
                out.push_back(r);
            }
        }
    }

    const int kept = static_cast<int>(out.size());
    bloom_rejected_ += chunk.num_rows - kept;
    bloom_tested_ += chunk.num_rows;
    // The sample verdict — see kBloomSampleRows. Dropping the filter here is the
    // whole mechanism: nothing below re-reads it.
    if (bloom_tested_ >= kBloomSampleRows
        && bloom_rejected_ * kBloomMinRejectRatio < bloom_tested_) {
        bloom_.reset();
        bloom_gave_up_ = true;
    }
    if (kept == chunk.num_rows) {
        out.clear();     // leave the chunk dense; sel/filter_applied stay reset
        return;
    }
    chunk.sel.size = kept;
    chunk.filter_applied = true;
}

DataChunk* VecScanNode::nextChunk(){
    // skip zone-map chunks (CHUNK_SIZE=8192) that cannot contain matching rows
    while (pruning_where_ && row_cursor_ < columnar_table_->num_rows
           && row_cursor_ % CHUNK_SIZE == 0) {
        int chunk_idx = row_cursor_ / CHUNK_SIZE;
        if (!ChunkPruner::shouldSkip(pruning_where_, columnar_table_->zone_maps, chunk_idx, hint_schema_))
            break;
        row_cursor_ += std::min(CHUNK_SIZE, columnar_table_->num_rows - row_cursor_);
        ++skipped_chunks_;
    }

    if (row_cursor_ >= columnar_table_->num_rows){
        return nullptr;
    }

    int start = row_cursor_;
    int count = std::min(BATCH_SIZE, columnar_table_->num_rows - start);
    row_cursor_ += count;

    auto t0 = std::chrono::high_resolution_clock::now();


    // rebuild cols each call; sel/filter_applied must reset too — a parent
    // VecFilterNode stamps them onto this buffer (pass-through), and stale
    // state would leak into the next batch
    current_chunk_.num_rows = count;
    current_chunk_.columns.clear();
    current_chunk_.columns.reserve(schema_.size());
    current_chunk_.filter_applied = false;
    current_chunk_.sel.indices.clear();
    current_chunk_.sel.size = 0;

    // iterate in schema order
    for (int i = 0; i < schema_.size(); ++i){
        const std::string& col_name = schema_.column(i).name;
        TypeId col_type = schema_.column(i).type;
        const ColumnArray& arr = columnar_table_->columns.at(col_name);

        ColumnVector cv;
        cv.type = col_type;

        if (std::holds_alternative<std::vector<int64_t>>(arr)){
            const auto& raw = std::get<std::vector<int64_t>>(arr);
            cv.data = std::vector<int64_t>(raw.begin() + start, raw.begin() + start + count);
        }
        else if (std::holds_alternative<std::vector<double>>(arr)) {
            const auto& raw = std::get<std::vector<double>>(arr);
            cv.data = std::vector<double>(raw.begin() + start, raw.begin() + start + count);
        }
        else if (std::holds_alternative<std::vector<std::string>>(arr)) {
            const auto& raw = std::get<std::vector<std::string>>(arr);
            cv.data = std::vector<std::string>(raw.begin() + start, raw.begin() + start + count);
        }
        else if (std::holds_alternative<RLEColumn>(arr)) {
            // decodeRange: one binary search + linear fill instead of 1024 binary searches
            cv.data = std::get<RLEColumn>(arr).decodeRange(
            static_cast<int32_t>(start), static_cast<int32_t>(count));
        }
        else if (std::holds_alternative<DictionaryEncoder>(arr)) {
            // no batch interface for DictionaryEncoder, decode per row
            const auto& enc = std::get<DictionaryEncoder>(arr);
            std::vector<std::string> decoded;
            decoded.reserve(count);
            for (int r = start; r < start + count; ++r){
                decoded.push_back(enc.decode(r));
            }
            cv.data = std::move(decoded);
        }

        current_chunk_.columns.push_back(std::move(cv));
    }

    // rows_out is what this node HANDS ON, which is the filter node's own
    // convention: with a Bloom filter installed the rejected rows are still
    // materialized (they have to be, to be tested) but never leave here.
    if (bloom_) {
        applyBloomFilter(current_chunk_);
        stats.rows_out += current_chunk_.filter_applied
            ? static_cast<int>(current_chunk_.sel.indices.size())
            : count;
    } else {
        stats.rows_out += count;
    }
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    return &current_chunk_;
}

void VecScanNode::close() {}

const Schema& VecScanNode::outputSchema() const {
    return schema_;
}

std::string VecScanNode::explain() const {
    std::string s = "VecScan [" + table_name_ + ", " + std::to_string(schema_.size()) + " columns]";
    if (pruning_where_) {
        // the counter is a runtime value; before execution only report that a
        // pruning hint is attached (plain --explain never runs the plan)
        if (executed_) {
            int total = (columnar_table_->num_rows + CHUNK_SIZE - 1) / CHUNK_SIZE;
            s += " chunks_skipped=" + std::to_string(skipped_chunks_) + "/" + std::to_string(total);
        } else {
            s += " pruning=on";
        }
    }
    // Week 38. Two states, because the filter itself only exists at runtime:
    // plain --explain (which never executes the plan) reports that the pushdown
    // is wired, --explain-analyze reports what it actually removed.
    if (bloom_armed_) {
        // "gave_up" is reported rather than hidden: a scan that abandoned an
        // unselective filter looks identical to one that kept a useless one,
        // and the difference is the whole reason the sample exists.
        s += bloom_gave_up_ ? " bloom=gave_up" : " bloom=on";
        if (executed_) {
            s += " rows_rejected=" + std::to_string(bloom_rejected_)
               + "/" + std::to_string(bloom_tested_);
        }
    }
    return s;
}

std::vector<VecPlanNode*> VecScanNode::children() const {
    return {};
}