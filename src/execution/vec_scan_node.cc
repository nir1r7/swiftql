#include "execution/vec_scan_node.h"
#include "storage/rle_column.h"
#include "storage/dictionary_encoder.h"
#include "storage/chunk_pruner.h"
#include <algorithm>
#include <chrono>

VecScanNode::VecScanNode(std::string table_name, ColumnarTable columnar_table, Schema schema, const Expr* pruning_where)
    : table_name_(std::move(table_name)), columnar_table_(std::move(columnar_table)), schema_(std::move(schema)), pruning_where_(pruning_where) {}

void VecScanNode::open(){
    row_cursor_ = 0;
    skipped_chunks_ = 0;
    executed_ = true;
}

DataChunk* VecScanNode::nextChunk(){
    // skip zone-map chunks (CHUNK_SIZE=8192) that cannot contain matching rows
    while (pruning_where_ && row_cursor_ < columnar_table_.num_rows
           && row_cursor_ % CHUNK_SIZE == 0) {
        int chunk_idx = row_cursor_ / CHUNK_SIZE;
        if (!ChunkPruner::shouldSkip(pruning_where_, columnar_table_.zone_maps, chunk_idx, schema_))
            break;
        row_cursor_ += std::min(CHUNK_SIZE, columnar_table_.num_rows - row_cursor_);
        ++skipped_chunks_;
    }

    if (row_cursor_ >= columnar_table_.num_rows){
        return nullptr;
    }

    int start = row_cursor_;
    int count = std::min(BATCH_SIZE, columnar_table_.num_rows - start);
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
        const ColumnArray& arr = columnar_table_.columns.at(col_name);

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

    stats.rows_out += count;
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
            int total = (columnar_table_.num_rows + CHUNK_SIZE - 1) / CHUNK_SIZE;
            s += " chunks_skipped=" + std::to_string(skipped_chunks_) + "/" + std::to_string(total);
        } else {
            s += " pruning=on";
        }
    }
    return s;
}

std::vector<VecPlanNode*> VecScanNode::children() const {
    return {};
}