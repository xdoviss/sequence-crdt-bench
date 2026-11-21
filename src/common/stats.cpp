#include "crdt/common/stats.hpp"

#include <algorithm>
#include <cmath>

namespace crdt {

namespace {

// the value below which `quantile` (e.g. 0.95) of the sorted samples lie,
// interpolating between the two nearest samples.
double percentile(const std::vector<double>& sorted, double quantile) {
    if (sorted.empty()) return 0;

    double position = quantile * static_cast<double>(sorted.size() - 1);
    size_t lower = static_cast<size_t>(std::floor(position));
    size_t upper = static_cast<size_t>(std::ceil(position));
    double fraction = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

}  // namespace

Summary summarize(std::vector<double> samples) {
    Summary result;
    result.count = samples.size();
    if (samples.empty()) return result;

    std::sort(samples.begin(), samples.end());
    const double count = static_cast<double>(samples.size());

    double sum = 0;
    for (double sample : samples) sum += sample;
    result.mean = sum / count;

    double squared_deviation = 0;
    for (double sample : samples) squared_deviation += (sample - result.mean) * (sample - result.mean);
    result.stddev = samples.size() > 1 ? std::sqrt(squared_deviation / (count - 1)) : 0;
    result.ci95 = samples.size() > 1 ? 1.96 * result.stddev / std::sqrt(count) : 0;

    result.min = samples.front();
    result.max = samples.back();
    result.median = percentile(samples, 0.5);
    result.p95 = percentile(samples, 0.95);
    result.p99 = percentile(samples, 0.99);
    return result;
}

}  // namespace crdt
