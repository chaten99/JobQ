#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace jobq {

struct LatencySummary {
    std::size_t count{0};

    std::chrono::nanoseconds minimum{0};
    std::chrono::nanoseconds maximum{0};

    double averageNanoseconds{0.0};

    std::chrono::nanoseconds p50{0};
    std::chrono::nanoseconds p95{0};
    std::chrono::nanoseconds p99{0};
};

class LatencySamples {
public:
    void reserve(std::size_t capacity);

    void add(std::chrono::nanoseconds duration);

    void merge(const LatencySamples& other);

    std::size_t count() const noexcept;

    bool empty() const noexcept;

    std::optional<std::chrono::nanoseconds> total() const;

    std::optional<std::chrono::nanoseconds> percentile(
        unsigned percent
    ) const;

    std::optional<LatencySummary> summarize() const;

    static std::size_t nearestRankIndex(
        std::size_t count,
        unsigned percent
    );

private:
    void accumulate(std::uint64_t value) noexcept;

    long double totalAsLongDouble() const noexcept;

    std::vector<std::int64_t> samples_;
    std::uint64_t sumLow_{0};
    std::uint64_t sumHigh_{0};
};

}
