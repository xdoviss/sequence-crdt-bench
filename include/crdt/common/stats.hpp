// summary statistics for benchmark measurements
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "crdt/common/random_generator.hpp"

namespace crdt {

struct Summary {
    size_t count = 0;
    double mean = 0, median = 0, p95 = 0, p99 = 0, min = 0, max = 0, stddev = 0;
    double ci95 = 0; // the mean is within +-ci95 of the true mean with 95% confidence
};

// percentiles are interpolated between the closest measurements
Summary summarize(std::vector<double> samples);

// collects measuruments
// has max capacity (reservoir sampling) and if full new values replaces random old ones
// long simulation -> fair picture without requiring too much memory
// count and mean are always exact though
class SampleSink {
public:
    SampleSink(size_t capacity, uint64_t seed) : capacity_(capacity), random_(seed) {}

    void add(double value) {
        ++seen_;
        total_ += value;

        if (samples_.size() < capacity_) {
            samples_.push_back(value);
            return;
        }

        uint64_t slot = random_.below(seen_);
        if (slot < capacity_) 
            samples_[static_cast<size_t>(slot)] = value;
    }

    Summary summary() const {
        Summary result = summarize(samples_);
        result.count = seen_;
        result.mean = seen_ ? total_ / static_cast<double>(seen_) : 0;

        return result;
    }

private:
    size_t capacity_;
    RandomGenerator random_;
    std::vector<double> samples_;
    uint64_t seen_ = 0;
    double total_ = 0;
};

}  // namespace crdt
