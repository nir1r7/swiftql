
#include "csv_to_columnar.h"
#include <iostream>
#include <iomanip>

ColumnarTable CSVToColumnar::convert(const std::vector<Row>& rows, const Schema& schema){
    ColumnarTable table(schema, static_cast<int>(rows.size()));

    // pass 1: initialize one typed raw array per column
    // pass 2: populate raw values
    // pass 3:
    //   (a) apply encodings (selective RLE for INT, dictionary for STRING)
    //   (b) build zone maps (min/max per chunk)

    // pass 1
    for (int c = 0; c < schema.size(); ++c){
        const std::string& name = schema.column(c).name;
        switch (schema.column(c).type){
            case TypeId::INT: {
                auto& v = table.columns[name].emplace<std::vector<int64_t>>();
                v.reserve(rows.size());
                break;
            }
            case TypeId::DOUBLE: {
                auto& v = table.columns[name].emplace<std::vector<double>>();
                v.reserve(rows.size());
                break;
            }
            case TypeId::STRING: {
                auto& v = table.columns[name].emplace<std::vector<std::string>>();
                v.reserve(rows.size());
                break;
            }
        }
    }

    // pass 2
    for (const auto& row : rows){
        for (int c = 0; c < schema.size(); ++c){
            const std::string& name = schema.column(c).name;
            switch (schema.column(c).type){
                case TypeId::INT:
                    std::get<std::vector<int64_t>>(table.columns[name]).push_back(row[c].asInt());
                    break;
                case TypeId::DOUBLE:
                    std::get<std::vector<double>>(table.columns[name]).push_back(row[c].asDouble());
                    break;
                case TypeId::STRING:
                    std::get<std::vector<std::string>>(table.columns[name]).push_back(row[c].asString());
                    break;
            }
        }
    }

    // calculate storage before encoding (for stats)
    size_t raw_bytes = 0;
    for (int c = 0; c < schema.size(); ++c){
        raw_bytes += columnByteSize(table.columns[schema.column(c).name]);
    }

    // pass 3
    for (int c = 0; c < schema.size(); ++c){
        const std::string& name = schema.column(c).name;

        // apply encodings
        size_t before_bytes = columnByteSize(table.columns[name]);
        switch (schema.column(c).type){
            case TypeId::INT: {
                auto& raw_col = std::get<std::vector<int64_t>>(table.columns[name]);
                size_t col_raw_bytes = raw_col.size() * sizeof(int64_t);
                RLEColumn rle = RLEColumn::encode(raw_col);
                if (rle.byteSize()*2 < col_raw_bytes){
                    table.columns[name] = std::move(rle);
                }
                break;
            }
            case TypeId::DOUBLE:
                break;
            case TypeId::STRING: {
                auto& raw_col = std::get<std::vector<std::string>>(table.columns[name]);
                DictionaryEncoder enc = DictionaryEncoder::encode(raw_col);
                table.columns[name] = std::move(enc);
                break;
            }
        }
        std::cout << "  " << name << ": " << before_bytes / 1024 << " KB -> ";
        std::cout << columnByteSize(table.columns[name]) / 1024 << " KB\n";

        // build zone maps
        std::vector<ColumnChunk> chunks;
        int num_rows = static_cast<int>(rows.size());
        for (int start = 0; start < num_rows; start += CHUNK_SIZE){
            // last row may have fewer than 8192
            int count = std::min(CHUNK_SIZE, num_rows - start);
            Value first = table.getValue(name, start);
            Value mn = first;
            Value mx = first;

            for (int r = start + 1; r < start + count; ++r){
                Value v = table.getValue(name, r);
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            chunks.push_back({start, count, mn, mx});
        }
        table.zone_maps[name] = std::move(chunks);
    }

    // calculate storage after encoding (for sstats)
    size_t encoded_bytes = columnarTableByteSize(table);
    std::cout << "[columnar] raw=" << raw_bytes / 1024 << " KB";
    std::cout << "  encoded=" << encoded_bytes / 1024 << " KB";
    std::cout << "  ratio=" << std::fixed << std::setprecision(2);
    std::cout << (double)encoded_bytes / raw_bytes << "\n";

    return table;
}
