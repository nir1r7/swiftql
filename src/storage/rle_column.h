#pragma once

#include <algorithm>
#include <vector>
#include <utility>
#include <cstdint>

// run length encodes an integer column
// efficient for low cardinality or sorted-value columns
struct RLEColumn {
    // pairs (value, run_length) in row order
    std::vector<std::pair<int64_t, int32_t>> runs;
    // run_starts[i] = first row index of run i (prefix-sum of run lengths)
    std::vector<int32_t> run_starts;

    static RLEColumn encode(const std::vector<int64_t>& raw){
        RLEColumn rle;
        if (raw.empty()) return rle;

        rle.runs.emplace_back(raw[0], 1);
        rle.run_starts.push_back(0);

        for (size_t i = 1; i < raw.size(); ++i){
            if (raw[i] == rle.runs.back().first){
                ++rle.runs.back().second;
            } else {
                // capture old run's final length before mutating runs
                int32_t next_start = rle.run_starts.back() + rle.runs.back().second;
                rle.runs.emplace_back(raw[i], 1);
                rle.run_starts.push_back(next_start);
            }
        }

        return rle;
    }

    // O(log n_runs); precondition: 0 <= row_idx < total_rows, column non-empty
    int64_t get(int32_t row_idx) const {
        auto it = std::upper_bound(run_starts.begin(), run_starts.end(), row_idx);
        --it;
        return runs[it - run_starts.begin()].first;
    }

    // decode the rows in range [start_rows, start_rows + count] into a vector
    // decode range must fit as valid indices
    std::vector<int64_t> decodeRange(int32_t start_row, int32_t count) const {
        std::vector<int64_t> result;
        result.reserve(count);
        if (count == 0) return result;

        // find run containing start_row
        // run_idx = index of the greatest run_start <= start_row
        int run_idx = static_cast<int>(std::upper_bound(run_starts.begin(), run_starts.end(), start_row) - run_starts.begin()) - 1;
    
        int32_t end_row = start_row + count;
        int32_t row = start_row;

        while (row < end_row){
            // if there is another run after this one, then this run ends where the next run starts
            int32_t run_end = (run_idx + 1 < static_cast<int>(run_starts.size())) ? run_starts[run_idx + 1] : run_starts[run_idx] + runs[run_idx].second;
        
            int32_t fill_end = std::min(run_end, end_row);
            int64_t val = runs[run_idx].first;

            for (int32_t r = row; r < fill_end; ++r){
                result.push_back(val);
            }
        
            row = fill_end;
            ++run_idx;
        }
        return result;
    }

    // value(8) + run_length(4) + prefix_sum(4) = 16 bytes/run
    size_t byteSize() const {
        return runs.size() * (sizeof(int64_t) + 2 * sizeof(int32_t));
    }
};
