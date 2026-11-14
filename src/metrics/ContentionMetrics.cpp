#include "metrics/ContentionMetrics.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>

namespace jobq {

namespace {

std::atomic<std::size_t> nextShardIndex{0};

std::size_t currentShard() noexcept {
    thread_local const std::size_t shard =
        nextShardIndex.fetch_add(1, std::memory_order_relaxed) %
        ContentionMetrics::kShards;

    return shard;
}

void updateMaximum(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value
) noexcept {
    std::uint64_t current = target.load(std::memory_order_relaxed);

    while (value > current &&
           !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed
           )) {
    }
}

std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) noexcept {
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    return b > limit - a ? limit : a + b;
}

std::size_t bucketFor(std::uint64_t nanoseconds) noexcept {
    if (nanoseconds == 0) {
        return 0;
    }

    return std::min<std::size_t>(
        static_cast<std::size_t>(std::bit_width(nanoseconds)),
        kContentionBuckets - 1
    );
}

}

const char* lockCategoryName(LockCategory category) noexcept {
    switch (category) {
        case LockCategory::JobQueue: return "JobQueue mutex";
        case LockCategory::JobRegistry: return "JobManager registry mutex";
        case LockCategory::JobMetrics: return "JobMetrics mutex";
        case LockCategory::JobExecutor: return "JobExecutor handler mutex";
    }

    return "unknown";
}

double LockStatistics::averageWaitNanoseconds() const noexcept {
    if (acquisitions == 0) {
        return 0.0;
    }

    return static_cast<double>(totalWaitNanoseconds) /
           static_cast<double>(acquisitions);
}

double LockStatistics::averageHoldNanoseconds() const noexcept {
    if (holdSamples == 0) {
        return 0.0;
    }

    return static_cast<double>(totalHoldNanoseconds) /
           static_cast<double>(holdSamples);
}

std::uint64_t LockStatistics::estimatedWaitPercentileNanoseconds(
    unsigned percent
) const {
    if (acquisitions == 0 || percent > 100) {
        return 0;
    }

    const std::uint64_t whole = acquisitions / 100;
    const std::uint64_t remainder = acquisitions % 100;

    std::uint64_t rank = whole * percent + (remainder * percent + 99) / 100;
    rank = std::clamp<std::uint64_t>(rank, 1, acquisitions);

    std::uint64_t cumulative = 0;

    for (std::size_t bucket = 0; bucket < kContentionBuckets; ++bucket) {
        cumulative += waitBuckets[bucket];

        if (cumulative >= rank) {
            if (bucket == 0) {
                return 0;
            }

            if (bucket == kContentionBuckets - 1) {
                return maxWaitNanoseconds;
            }

            const std::uint64_t upper = (std::uint64_t{1} << bucket) - 1;
            return std::min(upper, maxWaitNanoseconds);
        }
    }

    return maxWaitNanoseconds;
}

struct alignas(64) ContentionMetrics::Counters {
    std::atomic<std::uint64_t> acquisitions{0};
    std::atomic<std::uint64_t> blocked{0};
    std::atomic<std::uint64_t> overThreshold{0};
    std::atomic<std::uint64_t> totalWait{0};
    std::atomic<std::uint64_t> maxWait{0};
    std::atomic<std::uint64_t> holdSamples{0};
    std::atomic<std::uint64_t> totalHold{0};
    std::atomic<std::uint64_t> maxHold{0};
    std::atomic<std::uint64_t> clamped{0};
    std::array<std::atomic<std::uint64_t>, kContentionBuckets> buckets{};
};

ContentionMetrics::ContentionMetrics(std::uint64_t thresholdNanoseconds)
    : threshold_(thresholdNanoseconds),
      counters_(new Counters[kShards * kLockCategoryCount]) {}

ContentionMetrics::~ContentionMetrics() = default;

ContentionMetrics::Counters& ContentionMetrics::countersFor(
    LockCategory category
) noexcept {
    return counters_[currentShard() * kLockCategoryCount +
                     static_cast<std::size_t>(category)];
}

void ContentionMetrics::recordAcquisition(
    LockCategory category,
    std::uint64_t waitNanoseconds,
    bool blocked
) noexcept {
    Counters& counters = countersFor(category);

    if (waitNanoseconds > kMaxSampleNanoseconds) {
        waitNanoseconds = kMaxSampleNanoseconds;
        counters.clamped.fetch_add(1, std::memory_order_relaxed);
    }

    counters.acquisitions.fetch_add(1, std::memory_order_relaxed);
    counters.totalWait.fetch_add(waitNanoseconds, std::memory_order_relaxed);
    counters.buckets[bucketFor(waitNanoseconds)].fetch_add(
        1, std::memory_order_relaxed
    );

    if (blocked) {
        counters.blocked.fetch_add(1, std::memory_order_relaxed);
    }

    if (waitNanoseconds >= threshold_) {
        counters.overThreshold.fetch_add(1, std::memory_order_relaxed);
    }

    updateMaximum(counters.maxWait, waitNanoseconds);
}

void ContentionMetrics::recordHold(
    LockCategory category,
    std::uint64_t holdNanoseconds
) noexcept {
    Counters& counters = countersFor(category);

    if (holdNanoseconds > kMaxSampleNanoseconds) {
        holdNanoseconds = kMaxSampleNanoseconds;
        counters.clamped.fetch_add(1, std::memory_order_relaxed);
    }

    counters.holdSamples.fetch_add(1, std::memory_order_relaxed);
    counters.totalHold.fetch_add(holdNanoseconds, std::memory_order_relaxed);
    updateMaximum(counters.maxHold, holdNanoseconds);
}

LockStatistics ContentionMetrics::snapshot(LockCategory category) const {
    LockStatistics result;

    for (std::size_t shard = 0; shard < kShards; ++shard) {
        const Counters& counters =
            counters_[shard * kLockCategoryCount +
                      static_cast<std::size_t>(category)];

        result.acquisitions = saturatingAdd(
            result.acquisitions,
            counters.acquisitions.load(std::memory_order_relaxed)
        );
        result.blockedAcquisitions = saturatingAdd(
            result.blockedAcquisitions,
            counters.blocked.load(std::memory_order_relaxed)
        );
        result.overThresholdAcquisitions = saturatingAdd(
            result.overThresholdAcquisitions,
            counters.overThreshold.load(std::memory_order_relaxed)
        );
        result.totalWaitNanoseconds = saturatingAdd(
            result.totalWaitNanoseconds,
            counters.totalWait.load(std::memory_order_relaxed)
        );
        result.maxWaitNanoseconds = std::max(
            result.maxWaitNanoseconds,
            counters.maxWait.load(std::memory_order_relaxed)
        );
        result.holdSamples = saturatingAdd(
            result.holdSamples,
            counters.holdSamples.load(std::memory_order_relaxed)
        );
        result.totalHoldNanoseconds = saturatingAdd(
            result.totalHoldNanoseconds,
            counters.totalHold.load(std::memory_order_relaxed)
        );
        result.maxHoldNanoseconds = std::max(
            result.maxHoldNanoseconds,
            counters.maxHold.load(std::memory_order_relaxed)
        );
        result.clampedSamples = saturatingAdd(
            result.clampedSamples,
            counters.clamped.load(std::memory_order_relaxed)
        );

        for (std::size_t bucket = 0; bucket < kContentionBuckets; ++bucket) {
            result.waitBuckets[bucket] = saturatingAdd(
                result.waitBuckets[bucket],
                counters.buckets[bucket].load(std::memory_order_relaxed)
            );
        }
    }

    return result;
}

std::uint64_t ContentionMetrics::thresholdNanoseconds() const noexcept {
    return threshold_;
}

void ContentionMetrics::reset() noexcept {
    for (std::size_t i = 0; i < kShards * kLockCategoryCount; ++i) {
        Counters& counters = counters_[i];

        counters.acquisitions.store(0, std::memory_order_relaxed);
        counters.blocked.store(0, std::memory_order_relaxed);
        counters.overThreshold.store(0, std::memory_order_relaxed);
        counters.totalWait.store(0, std::memory_order_relaxed);
        counters.maxWait.store(0, std::memory_order_relaxed);
        counters.holdSamples.store(0, std::memory_order_relaxed);
        counters.totalHold.store(0, std::memory_order_relaxed);
        counters.maxHold.store(0, std::memory_order_relaxed);
        counters.clamped.store(0, std::memory_order_relaxed);

        for (auto& bucket : counters.buckets) {
            bucket.store(0, std::memory_order_relaxed);
        }
    }
}

}
