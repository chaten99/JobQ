#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace jobq_bench {

constexpr std::size_t kMaxWorkerCountEntries = 32;

struct WorkerCountList {
    bool ok{true};
    std::string error;
    std::vector<std::uint64_t> counts;
};

inline WorkerCountList parseWorkerCounts(
    std::string_view text,
    std::uint64_t maxWorkers
) {
    WorkerCountList result;

    const auto fail = [&result](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        result.counts.clear();
        return result;
    };

    if (text.empty()) {
        return fail("--worker-counts requires a non-empty list such as 1,2,4");
    }

    std::size_t start = 0;
    std::size_t position = 1;

    while (true) {
        const auto comma = text.find(',', start);
        const auto token = text.substr(
            start,
            comma == std::string_view::npos ? std::string_view::npos
                                            : comma - start
        );

        if (token.empty()) {
            return fail(
                "--worker-counts entry " + std::to_string(position) +
                " is empty (check for doubled or trailing commas)"
            );
        }

        std::uint64_t value = 0;
        const char* begin = token.data();
        const char* end = begin + token.size();
        const auto [stop, code] = std::from_chars(begin, end, value);

        if (code == std::errc::result_out_of_range) {
            return fail(
                "--worker-counts entry '" + std::string(token) +
                "' is out of range"
            );
        }

        if (code != std::errc{} || stop != end) {
            return fail(
                "--worker-counts entry '" + std::string(token) +
                "' is not a valid positive integer"
            );
        }

        if (value == 0) {
            return fail("--worker-counts entries must be positive (got 0)");
        }

        if (value > maxWorkers) {
            return fail(
                "--worker-counts entry " + std::to_string(value) +
                " exceeds the maximum of " + std::to_string(maxWorkers)
            );
        }

        if (std::find(result.counts.begin(), result.counts.end(), value) !=
            result.counts.end()) {
            return fail(
                "--worker-counts contains duplicate entry " +
                std::to_string(value)
            );
        }

        if (result.counts.size() >= kMaxWorkerCountEntries) {
            return fail(
                "--worker-counts accepts at most " +
                std::to_string(kMaxWorkerCountEntries) + " entries"
            );
        }

        result.counts.push_back(value);

        if (comma == std::string_view::npos) {
            break;
        }

        start = comma + 1;
        ++position;
    }

    std::sort(result.counts.begin(), result.counts.end());
    return result;
}

struct RunSummary {
    std::size_t count{0};
    double minimum{0.0};
    double maximum{0.0};
    double mean{0.0};
    double median{0.0};
};

inline double medianOf(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t count = values.size();

    if (count % 2 == 1) {
        return values[count / 2];
    }

    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

inline std::optional<RunSummary> summarizeThroughputs(
    const std::vector<double>& values
) {
    if (values.empty()) {
        return std::nullopt;
    }

    long double sum = 0.0L;

    for (const double value : values) {
        if (!std::isfinite(value) || value < 0.0) {
            return std::nullopt;
        }
        sum += value;
    }

    RunSummary summary;
    summary.count = values.size();
    summary.minimum = *std::min_element(values.begin(), values.end());
    summary.maximum = *std::max_element(values.begin(), values.end());
    summary.mean = static_cast<double>(
        sum / static_cast<long double>(values.size())
    );
    summary.median = medianOf(values);
    return summary;
}

inline std::optional<double> speedup(double baseline, double value) {
    if (!std::isfinite(baseline) || !std::isfinite(value) ||
        baseline <= 0.0 || value < 0.0) {
        return std::nullopt;
    }

    return value / baseline;
}

inline std::optional<double> scalingEfficiencyPercent(
    std::optional<double> speedupValue,
    std::uint64_t workers
) {
    if (!speedupValue || workers == 0) {
        return std::nullopt;
    }

    return *speedupValue / static_cast<double>(workers) * 100.0;
}

struct ThroughputChange {
    double absolute{0.0};
    std::optional<double> percent;
};

inline std::optional<ThroughputChange> throughputChange(
    double baseline,
    double value
) {
    if (!std::isfinite(baseline) || !std::isfinite(value)) {
        return std::nullopt;
    }

    ThroughputChange change;
    change.absolute = value - baseline;

    if (baseline > 0.0) {
        change.percent = change.absolute / baseline * 100.0;
    }

    return change;
}

struct ScalingInput {
    std::uint64_t workers{0};
    bool ok{false};
    std::optional<RunSummary> throughput;
};

struct ScalingRow {
    std::uint64_t workers{0};
    bool valid{false};
    std::optional<double> speedup;
    std::optional<double> efficiencyPercent;
    std::optional<ThroughputChange> change;
};

struct ScalingAnalysis {
    bool baselineAvailable{false};
    std::string baselineNote;

    std::vector<ScalingRow> rows;

    std::optional<std::size_t> bestMaximumIndex;
    std::optional<std::size_t> bestMedianIndex;

    std::vector<std::size_t> decreasedAt;
    std::vector<std::size_t> efficiencyDeclinedAt;

    bool additionalWorkersImproved{false};
};

inline ScalingAnalysis analyzeScaling(
    const std::vector<ScalingInput>& inputs
) {
    ScalingAnalysis analysis;
    analysis.rows.resize(inputs.size());

    std::optional<double> baseline;

    for (const auto& input : inputs) {
        if (input.workers == 1 && input.ok && input.throughput &&
            input.throughput->median > 0.0) {
            baseline = input.throughput->median;
        }
    }

    analysis.baselineAvailable = baseline.has_value();

    if (!baseline) {
        analysis.baselineNote =
            "No successful 1-worker configuration with positive throughput; "
            "speedup and efficiency are unavailable";
    }

    std::optional<std::size_t> previous;

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& input = inputs[i];
        auto& row = analysis.rows[i];

        row.workers = input.workers;
        row.valid = input.ok && input.throughput.has_value();

        if (!row.valid) {
            continue;
        }

        const double median = input.throughput->median;

        if (baseline) {
            row.speedup = speedup(*baseline, median);
            row.efficiencyPercent =
                scalingEfficiencyPercent(row.speedup, input.workers);
            row.change = throughputChange(*baseline, median);

            if (input.workers > 1 && median > *baseline) {
                analysis.additionalWorkersImproved = true;
            }
        }

        if (!analysis.bestMaximumIndex ||
            input.throughput->maximum >
                inputs[*analysis.bestMaximumIndex].throughput->maximum) {
            analysis.bestMaximumIndex = i;
        }

        if (!analysis.bestMedianIndex ||
            median > inputs[*analysis.bestMedianIndex].throughput->median) {
            analysis.bestMedianIndex = i;
        }

        if (previous) {
            const auto& before = analysis.rows[*previous];

            if (median < inputs[*previous].throughput->median) {
                analysis.decreasedAt.push_back(i);
            }

            if (row.efficiencyPercent && before.efficiencyPercent &&
                *row.efficiencyPercent < *before.efficiencyPercent) {
                analysis.efficiencyDeclinedAt.push_back(i);
            }
        }

        previous = i;
    }

    return analysis;
}

}
