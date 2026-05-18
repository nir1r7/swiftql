#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// dictionary encode a strong column
// store unique strings in dict as int32_t index
// reduce memory from 32bytes per row to 4 bytes per row
struct DictionaryEncoder {
    std::vector<std::string> dict; // maintain order
    std::vector<int32_t> codes; // codes[i] = dict index for row i

    static DictionaryEncoder encode(const std::vector<std::string>& raw){
        DictionaryEncoder enc;
        std::unordered_map<std::string, int32_t> str_to_id;

        enc.codes.reserve(raw.size());

        for (const auto& s: raw){
            auto it = str_to_id.find(s);
            
            if (it == str_to_id.end()){
                int32_t id = static_cast<int32_t>(enc.dict.size());
                str_to_id[s] = id;
                enc.dict.push_back(s);
                enc.codes.push_back(id);
            }
            else {
                enc.codes.push_back(it->second);
            }
        }
        return enc;
    }

    const std::string& decode(int row_idx) const {
        return dict[codes[row_idx]];
    }

    size_t byteSize() const {
        size_t sz = codes.size() * sizeof(int32_t);
        for (const auto& s : dict){
            sz += sizeof(std::string) + s.size();
        }
        return sz;
    }
};