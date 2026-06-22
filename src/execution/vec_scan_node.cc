#include "execution/vec_scan_node.h"
#include "storage/rle_column.h"
#include "storage/dictionary_encoder.h"
#include <algorithm>

VecScanNode::VecScanNode(std::string table_name, ColumnarTable columnar_table, Schema schema) : table_name_(std::move(table_name)), columnar_table_(std::move(columnar_table)), schema_(std::move(schema)) {}

void VecScanNode::open(){
    row_cursor_ = 0;
}

DataChunk* VecScanNode::nextChunk(){
    if (row_cursor_ >= columnar_table_.num_rows){
        return nullptr;
    }

    int start = row_cursor_;
    int count = std::min(BATCH_SIZE, columnar_table_.num_rows - start);
    row_cursor_ += count;

    auto t0 = std::chrono::high_resolution_clock::now();


    // rebuild cols each call
    current_chunk_.num_rows = count;
    current_chunk_.columns.clear();
    current_chunk_.columns.reserve(schema_.size());

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
    return "VecScan [" + table_name_ + ", " + std::to_string(schema_.size()) + " columns]";
}

std::vector<VecPlanNode*> VecScanNode::children() const {
    return {};
}