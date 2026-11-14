#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace jobq {

enum class LockCategory : std::size_t {
    JobQueue = 0,
    JobRegistry = 1,
    JobMetrics = 2,
    JobExecutor = 3
};

constexpr std::size_t kLockCategoryCount = 4;
constexpr std::size_t kContentionBuckets = 40;

const char* lockCategoryName(LockCategory category) noexcept;

struct LockStatistics {
    std::uint64_t acquisitions{0};
    std::uint64_t blockedAcquisitions{0};
    std::uint64_t overThresholdAcquisitions{0};

    std::uint64_t totalWaitNanoseconds{0};
    std::uint64_t maxWaitNanoseconds{0};

    std::uint64_t holdSamples{0};
    std::uint64_t totalHoldNanoseconds{0};
    std::uint64_t maxHoldNanoseconds{0};

    std::uint64_t clampedSamples{0};

    std::array<std::uint64_t, kContentionBuckets> waitBuckets{};

    double averageWaitNanoseconds() const noexcept;

    double averageHoldNanoseconds() const noexcept;

    std::uint64_t estimatedWaitPercentileNanoseconds(unsigned percent) const;
};

class ContentionMetrics {
public:
    static constexpr std::uint64_t kDefaultThresholdNanoseconds = 1000;
    static constexpr std::uint64_t kMaxSampleNanoseconds =
        std::uint64_t{1} << 44;
    static constexpr std::size_t kShards = 32;

    explicit ContentionMetrics(
        std::uint64_t thresholdNanoseconds = kDefaultThresholdNanoseconds
    );

    ~ContentionMetrics();

    ContentionMetrics(const ContentionMetrics&) = delete;
    ContentionMetrics& operator=(const ContentionMetrics&) = delete;

    void recordAcquisition(
        LockCategory category,
        std::uint64_t waitNanoseconds,
        bool blocked
    ) noexcept;

    void recordHold(
        LockCategory category,
        std::uint64_t holdNanoseconds
    ) noexcept;

    LockStatistics snapshot(LockCategory category) const;

    std::uint64_t thresholdNanoseconds() const noexcept;

    void reset() noexcept;

private:
    struct Counters;

    Counters& countersFor(LockCategory category) noexcept;

    std::uint64_t threshold_;
    std::unique_ptr<Counters[]> counters_;
};

}
