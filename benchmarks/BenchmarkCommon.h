#pragma once

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "metrics/ExecutionObserver.h"
#include "metrics/LatencyStatistics.h"

namespace jobq_bench {

using SteadyClock = std::chrono::steady_clock;

enum class Scenario { Noop, Lightweight, Mixed };

inline const char* scenarioName(Scenario scenario) {
    switch (scenario) {
        case Scenario::Noop: return "noop";
        case Scenario::Lightweight: return "lightweight";
        case Scenario::Mixed: return "mixed";
    }
    return "unknown";
}

inline bool parseUnsigned(
    std::string_view text,
    std::uint64_t minimum,
    std::uint64_t maximum,
    const std::string& option,
    std::uint64_t& value,
    std::string& error
) {
    if (text.empty()) {
        error = "Option " + option + " requires a value";
        return false;
    }

    std::uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, code] = std::from_chars(begin, end, parsed);

    if (code == std::errc::result_out_of_range) {
        error = "Value for " + option + " is out of range: " +
                std::string(text);
        return false;
    }

    if (code != std::errc{} || position != end) {
        error = "Value for " + option + " is not a valid non-negative "
                "integer: '" + std::string(text) + "'";
        return false;
    }

    if (parsed < minimum || parsed > maximum) {
        error = "Value for " + option + " must be between " +
                std::to_string(minimum) + " and " +
                std::to_string(maximum) + " (got " +
                std::to_string(parsed) + ")";
        return false;
    }

    value = parsed;
    return true;
}

class NullBuffer final : public std::streambuf {
protected:
    int_type overflow(int_type character) override {
        return traits_type::not_eof(character);
    }

    std::streamsize xsputn(const char*, std::streamsize count) override {
        return count;
    }
};

class StreamSilencer {
public:
    StreamSilencer()
        : coutBuffer_(std::cout.rdbuf(&null_)),
          cerrBuffer_(std::cerr.rdbuf(&null_)) {}

    ~StreamSilencer() {
        std::cout.rdbuf(coutBuffer_);
        std::cerr.rdbuf(cerrBuffer_);
    }

    StreamSilencer(const StreamSilencer&) = delete;
    StreamSilencer& operator=(const StreamSilencer&) = delete;

private:
    NullBuffer null_;
    std::streambuf* coutBuffer_;
    std::streambuf* cerrBuffer_;
};

enum class Work { None, Spin50, Spin200, Spin400, Spin1500, Hash };

struct Kind {
    const char* type;
    Work work;
};

inline std::uint64_t spin(std::uint64_t seed, int iterations) {
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;

    for (int i = 0; i < iterations; ++i) {
        x ^= x >> 13;
        x *= 0xFF51AFD7ED558CCDULL;
        x ^= x << 7;
    }

    return x;
}

inline std::uint64_t fnv(std::uint64_t seed) {
    std::uint64_t hash = 0xCBF29CE484222325ULL ^ seed;

    for (int i = 0; i < 256; ++i) {
        hash ^= static_cast<std::uint64_t>((i * 31 + seed) & 0xFF);
        hash *= 0x100000001B3ULL;
    }

    return hash;
}

inline std::uint64_t runWork(Work work, std::uint64_t id) {
    switch (work) {
        case Work::None: return 0;
        case Work::Spin50: return spin(id, 50);
        case Work::Spin200: return spin(id, 200);
        case Work::Spin400: return spin(id, 400);
        case Work::Spin1500: return spin(id, 1500);
        case Work::Hash: return fnv(id);
    }
    return 0;
}

inline std::vector<Kind> kindsFor(Scenario scenario) {
    switch (scenario) {
        case Scenario::Noop:
            return {{"NOOP", Work::None}};
        case Scenario::Lightweight:
            return {{"LIGHT", Work::Spin200}};
        case Scenario::Mixed:
            return {
                {"MIX_SMALL", Work::Spin50},
                {"MIX_MEDIUM", Work::Spin400},
                {"MIX_LARGE", Work::Spin1500},
                {"MIX_HASH", Work::Hash},
            };
    }
    return {};
}

inline std::size_t kindIndexFor(Scenario scenario, std::uint64_t id) {
    if (scenario != Scenario::Mixed) {
        return 0;
    }

    static constexpr std::size_t pattern[10] = {0, 0, 0, 0, 0, 1, 1, 1, 2, 3};
    return pattern[id % 10];
}

class LatencyCollector final : public jobq::ExecutionObserver {
public:
    LatencyCollector(
        std::size_t workers,
        std::size_t expectedJobs,
        const std::vector<SteadyClock::time_point>* submitStarts
    )
        : slots_(workers), submitStarts_(submitStarts) {
        const std::size_t reserve =
            expectedJobs / workers + expectedJobs / 8 + 16;

        for (auto& slot : slots_) {
            slot.queueWait.reserve(reserve);
            slot.executionSuccess.reserve(reserve);
            slot.endToEnd.reserve(reserve);
        }
    }

    void onExecutionFinished(
        const jobq::ExecutionSample& sample
    ) noexcept override {
        if (sample.workerId == 0 || sample.workerId > slots_.size()) {
            invalid_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        Slot& slot = slots_[sample.workerId - 1];

        try {
            const auto wait =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sample.queueWait
                );
            const auto execution =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sample.executionDuration
                );

            if (wait.count() < 0 || execution.count() < 0) {
                ++slot.invalid;
                return;
            }

            slot.queueWait.add(wait);

            if (!sample.succeeded) {
                slot.executionFailure.add(execution);
                return;
            }

            slot.executionSuccess.add(execution);

            if (!submitStarts_) {
                return;
            }

            if (sample.jobId == 0 || sample.jobId >= submitStarts_->size()) {
                ++slot.invalid;
                return;
            }

            const auto total =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sample.finishedAt - (*submitStarts_)[sample.jobId]
                );

            if (total.count() < 0) {
                ++slot.invalid;
                return;
            }

            slot.endToEnd.add(total);
        } catch (...) {
            ++slot.invalid;
        }
    }

    std::uint64_t collect(
        jobq::LatencySamples& queueWait,
        jobq::LatencySamples& executionSuccess,
        jobq::LatencySamples& executionFailure,
        jobq::LatencySamples& endToEnd
    ) {
        std::uint64_t invalid = invalid_.load();

        for (auto& slot : slots_) {
            queueWait.merge(slot.queueWait);
            executionSuccess.merge(slot.executionSuccess);
            executionFailure.merge(slot.executionFailure);
            endToEnd.merge(slot.endToEnd);
            invalid += slot.invalid;
        }

        return invalid;
    }

private:
    struct alignas(64) Slot {
        jobq::LatencySamples queueWait;
        jobq::LatencySamples executionSuccess;
        jobq::LatencySamples executionFailure;
        jobq::LatencySamples endToEnd;
        std::uint64_t invalid{0};
    };

    std::vector<Slot> slots_;
    const std::vector<SteadyClock::time_point>* submitStarts_;
    std::atomic<std::uint64_t> invalid_{0};
};

inline std::string formatNanoseconds(double nanoseconds) {
    std::ostringstream text;
    text << std::fixed;

    if (nanoseconds < 1000.0) {
        text << std::setprecision(0) << nanoseconds << " ns";
    } else if (nanoseconds < 1'000'000.0) {
        text << std::setprecision(2) << nanoseconds / 1000.0 << " us";
    } else if (nanoseconds < 1'000'000'000.0) {
        text << std::setprecision(3) << nanoseconds / 1'000'000.0 << " ms";
    } else {
        text << std::setprecision(3) << nanoseconds / 1'000'000'000.0 << " s";
    }

    return text.str();
}

inline std::string formatDuration(std::chrono::nanoseconds duration) {
    return formatNanoseconds(static_cast<double>(duration.count()));
}


inline void printLatencyLine(
    std::ostream& out,
    const std::string& label,
    const jobq::LatencySamples& samples
) {
    const auto summary = samples.summarize();

    out << "  " << std::left << std::setw(14) << label << std::right
        << " n=" << samples.count();

    if (!summary) {
        out << "  (no samples)\n";
        return;
    }

    out << "  avg " << formatNanoseconds(summary->averageNanoseconds)
        << "  p50 " << formatDuration(summary->p50)
        << "  p95 " << formatDuration(summary->p95)
        << "  p99 " << formatDuration(summary->p99) << '\n';
}

inline void printSummaryBlock(
    std::ostream& out,
    const std::string& title,
    const jobq::LatencySamples& samples
) {
    out << title << "\n\n"
        << "  Samples:              " << samples.count() << '\n';

    const auto summary = samples.summarize();

    if (!summary) {
        out << "  No samples collected (statistics not available).\n\n";
        return;
    }

    out << "  Minimum:              " << formatDuration(summary->minimum) << '\n'
        << "  Maximum:              " << formatDuration(summary->maximum) << '\n'
        << "  Average:              "
        << formatNanoseconds(summary->averageNanoseconds) << '\n'
        << "  P50:                  " << formatDuration(summary->p50) << '\n'
        << "  P95:                  " << formatDuration(summary->p95) << '\n'
        << "  P99:                  " << formatDuration(summary->p99) << "\n\n";
}

}
