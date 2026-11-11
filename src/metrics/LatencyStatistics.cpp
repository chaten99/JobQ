#include "metrics/LatencyStatistics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace jobq {

void LatencySamples::reserve(std::size_t capacity) {
    samples_.reserve(capacity);
}

void LatencySamples::accumulate(std::uint64_t value) noexcept {
    sumLow_ += value;

    if (sumLow_ < value) {
        ++sumHigh_;
    }
}

void LatencySamples::add(std::chrono::nanoseconds duration) {
    if (duration.count() < 0) {
        throw std::invalid_argument(
            "Latency sample cannot be negative"
        );
    }

    samples_.push_back(duration.count());
    accumulate(static_cast<std::uint64_t>(duration.count()));
}

void LatencySamples::merge(const LatencySamples& other) {
    samples_.insert(
        samples_.end(),
        other.samples_.begin(),
        other.samples_.end()
    );

    sumLow_ += other.sumLow_;

    if (sumLow_ < other.sumLow_) {
        ++sumHigh_;
    }

    sumHigh_ += other.sumHigh_;
}

std::size_t LatencySamples::count() const noexcept {
    return samples_.size();
}

bool LatencySamples::empty() const noexcept {
    return samples_.empty();
}

long double LatencySamples::totalAsLongDouble() const noexcept {
    return std::ldexp(static_cast<long double>(sumHigh_), 64) +
           static_cast<long double>(sumLow_);
}

std::optional<std::chrono::nanoseconds> LatencySamples::total() const {
    constexpr auto limit = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max()
    );

    if (sumHigh_ != 0 || sumLow_ > limit) {
        return std::nullopt;
    }

    return std::chrono::nanoseconds(
        static_cast<std::int64_t>(sumLow_)
    );
}

std::size_t LatencySamples::nearestRankIndex(
    std::size_t count,
    unsigned percent
) {
    if (count == 0) {
        throw std::invalid_argument(
            "Percentile of an empty sample set is undefined"
        );
    }

    if (percent > 100) {
        throw std::invalid_argument("Percentile must be 0..100");
    }

    const std::size_t whole = count / 100;
    const std::size_t remainder = count % 100;

    std::size_t rank =
        whole * percent + (remainder * percent + 99) / 100;

    rank = std::clamp<std::size_t>(rank, 1, count);

    return rank - 1;
}

std::optional<std::chrono::nanoseconds> LatencySamples::percentile(
    unsigned percent
) const {
    if (samples_.empty()) {
        return std::nullopt;
    }

    std::vector<std::int64_t> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());

    return std::chrono::nanoseconds(
        sorted[nearestRankIndex(sorted.size(), percent)]
    );
}

std::optional<LatencySummary> LatencySamples::summarize() const {
    if (samples_.empty()) {
        return std::nullopt;
    }

    std::vector<std::int64_t> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());

    LatencySummary summary;
    summary.count = sorted.size();
    summary.minimum = std::chrono::nanoseconds(sorted.front());
    summary.maximum = std::chrono::nanoseconds(sorted.back());
    summary.averageNanoseconds = static_cast<double>(
        totalAsLongDouble() / static_cast<long double>(sorted.size())
    );
    summary.p50 = std::chrono::nanoseconds(
        sorted[nearestRankIndex(sorted.size(), 50)]
    );
    summary.p95 = std::chrono::nanoseconds(
        sorted[nearestRankIndex(sorted.size(), 95)]
    );
    summary.p99 = std::chrono::nanoseconds(
        sorted[nearestRankIndex(sorted.size(), 99)]
    );

    return summary;
}

}
