#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "ScalingAnalysis.h"

namespace {

using namespace jobq_bench;

int testsRun = 0;
int testsFailed = 0;

void check(bool condition, const std::string& description) {
    ++testsRun;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

bool near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) <= tolerance;
}

bool rejected(const std::string& text, const std::string& fragment = "") {
    const auto result = parseWorkerCounts(text, 256);
    return !result.ok && result.counts.empty() &&
           result.error.find(fragment) != std::string::npos;
}

ScalingInput input(
    std::uint64_t workers,
    double median,
    double maximum = -1.0,
    bool ok = true
) {
    ScalingInput value;
    value.workers = workers;
    value.ok = ok;

    if (ok) {
        RunSummary summary;
        summary.count = 3;
        summary.median = median;
        summary.mean = median;
        summary.minimum = median;
        summary.maximum = maximum < 0.0 ? median : maximum;
        value.throughput = summary;
    }

    return value;
}

void testParsing() {
    std::cout << "\n--- Worker-Count Parsing ---\n";

    const auto basic = parseWorkerCounts("1,2,4,8,16", 256);
    check(basic.ok && basic.counts == std::vector<std::uint64_t>{1, 2, 4, 8, 16},
          "Valid list is parsed");

    const auto single = parseWorkerCounts("4", 256);
    check(single.ok && single.counts == std::vector<std::uint64_t>{4},
          "A single entry is a valid list");

    const auto unsorted = parseWorkerCounts("8,1,4", 256);
    check(unsorted.ok && unsorted.counts == std::vector<std::uint64_t>{1, 4, 8},
          "Entries are sorted ascending");

    check(rejected("", "non-empty"), "Empty list is rejected");
    check(rejected("0", "positive"), "Zero is rejected");
    check(rejected("1,0,2", "positive"), "Zero inside a list is rejected");
    check(rejected("-1", "not a valid"), "Negative value is rejected");
    check(rejected("1,,2", "empty"), "Doubled comma is rejected");
    check(rejected("1,2,", "empty"), "Trailing comma is rejected");
    check(rejected(",1", "empty"), "Leading comma is rejected");
    check(rejected("1, 2", "not a valid"), "Whitespace inside an entry is rejected");
    check(rejected("a", "not a valid"), "Non-numeric entry is rejected");
    check(rejected("2x", "not a valid"), "Trailing characters are rejected");
    check(rejected("2.5", "not a valid"), "Decimal entry is rejected");
    check(rejected("+2", "not a valid"), "Explicit plus sign is rejected");
    check(rejected("99999999999999999999", "out of range"),
          "uint64 overflow is rejected");
    check(rejected("257", "exceeds"), "Count above the maximum is rejected");
    check(parseWorkerCounts("256", 256).ok, "Count equal to the maximum is accepted");
    check(rejected("2,4,2", "duplicate"), "Duplicate entries are rejected");

    std::string many;
    for (int i = 1; i <= 33; ++i) {
        many += (i > 1 ? "," : "") + std::to_string(i);
    }
    check(rejected(many, "at most"), "More than 32 entries are rejected");
}

void testRunSummary() {
    std::cout << "\n--- Repeated-Run Summary ---\n";

    const auto odd = summarizeThroughputs({30.0, 10.0, 20.0});
    check(odd && odd->count == 3 && odd->minimum == 10.0 && odd->maximum == 30.0 &&
              odd->mean == 20.0 && odd->median == 20.0,
          "Odd count: min, max, mean, middle-element median");

    const auto even = summarizeThroughputs({4.0, 1.0, 3.0, 2.0});
    check(even && even->median == 2.5 && even->mean == 2.5,
          "Even count: median is the mean of the two middle values");

    const auto single = summarizeThroughputs({7.5});
    check(single && single->minimum == 7.5 && single->maximum == 7.5 &&
              single->mean == 7.5 && single->median == 7.5,
          "Single sample");

    check(!summarizeThroughputs({}).has_value(), "Empty sample set has no summary");
    check(!summarizeThroughputs({1.0, std::nan("")}).has_value(),
          "NaN sample is rejected");
    check(!summarizeThroughputs({1.0, std::numeric_limits<double>::infinity()})
               .has_value(),
          "Infinite sample is rejected");
    check(!summarizeThroughputs({1.0, -2.0}).has_value(),
          "Negative throughput is rejected");
    check(summarizeThroughputs({0.0, 0.0})->mean == 0.0,
          "All-zero throughputs summarize to zero");
}

void testFormulas() {
    std::cout << "\n--- Speedup, Efficiency, Change ---\n";

    check(near(*speedup(10000.0, 30000.0), 3.0), "Speedup = 30000 / 10000 = 3.0");
    check(near(*speedup(10000.0, 10000.0), 1.0), "Baseline speedup is 1.0");
    check(near(*speedup(10000.0, 5000.0), 0.5), "Speedup below 1 is reported as is");
    check(!speedup(0.0, 100.0).has_value(), "Zero baseline: speedup unavailable");
    check(!speedup(-1.0, 100.0).has_value(), "Negative baseline: speedup unavailable");
    check(!speedup(std::nan(""), 100.0).has_value(), "NaN baseline: speedup unavailable");
    check(!speedup(100.0, std::numeric_limits<double>::infinity()).has_value(),
          "Infinite throughput: speedup unavailable");
    check(near(*speedup(100.0, 0.0), 0.0), "Zero throughput gives speedup 0");

    check(near(*scalingEfficiencyPercent(3.0, 4), 75.0),
          "Efficiency = 3.0 / 4 * 100 = 75%");
    check(near(*scalingEfficiencyPercent(1.0, 1), 100.0),
          "One-worker baseline efficiency is 100%");
    check(near(*scalingEfficiencyPercent(2.5, 2), 125.0),
          "Efficiency above 100% is not clamped");
    check(!scalingEfficiencyPercent(std::nullopt, 4).has_value(),
          "Unavailable speedup propagates to efficiency");
    check(!scalingEfficiencyPercent(1.0, 0).has_value(), "Zero workers: unavailable");

    const auto change = throughputChange(10000.0, 25000.0);
    check(change && near(change->absolute, 15000.0) && near(*change->percent, 150.0),
          "Change: +15000 jobs/sec, +150%");
    const auto drop = throughputChange(10000.0, 8000.0);
    check(drop && near(drop->absolute, -2000.0) && near(*drop->percent, -20.0),
          "Decrease is reported as negative");
    const auto zeroBase = throughputChange(0.0, 500.0);
    check(zeroBase && near(zeroBase->absolute, 500.0) && !zeroBase->percent.has_value(),
          "Zero baseline: absolute change only, no percentage");
    check(!throughputChange(std::nan(""), 1.0).has_value(),
          "Non-finite input: change unavailable");
}

void testAnalysis() {
    std::cout << "\n--- Scaling Analysis ---\n";

    const auto normal = analyzeScaling({
        input(1, 10000.0, 11000.0),
        input(2, 18000.0, 19000.0),
        input(4, 30000.0, 36000.0),
        input(8, 28000.0, 29000.0),
    });

    check(normal.baselineAvailable, "Baseline found at 1 worker");
    check(near(*normal.rows[0].speedup, 1.0) &&
              near(*normal.rows[0].efficiencyPercent, 100.0),
          "Baseline row: 1.0x, 100%");
    check(near(*normal.rows[1].speedup, 1.8) &&
              near(*normal.rows[1].efficiencyPercent, 90.0),
          "2 workers: 1.8x, 90%");
    check(near(*normal.rows[2].speedup, 3.0) &&
              near(*normal.rows[2].efficiencyPercent, 75.0),
          "4 workers: 3.0x, 75%");
    check(near(*normal.rows[3].speedup, 2.8) &&
              near(*normal.rows[3].efficiencyPercent, 35.0),
          "8 workers: 2.8x, 35%");
    check(near(normal.rows[2].change->absolute, 20000.0) &&
              near(*normal.rows[2].change->percent, 200.0),
          "Throughput difference relative to the baseline");
    check(normal.bestMedianIndex == 2, "Best median is 4 workers, not the largest count");
    check(normal.bestMaximumIndex == 2, "Best single-run maximum is 4 workers");
    check((normal.decreasedAt == std::vector<std::size_t>{3}),
          "Throughput decrease detected at 8 workers");
    check((normal.efficiencyDeclinedAt == std::vector<std::size_t>{1, 2, 3}),
          "Efficiency declines at each step");
    check(normal.additionalWorkersImproved, "Additional workers improved throughput");

    const auto noBaseline = analyzeScaling({input(2, 100.0), input(4, 150.0)});
    check(!noBaseline.baselineAvailable && !noBaseline.baselineNote.empty(),
          "Missing 1-worker baseline is reported");
    check(!noBaseline.rows[0].speedup && !noBaseline.rows[1].efficiencyPercent &&
              !noBaseline.rows[1].change,
          "No relative values are fabricated without a baseline");
    check(noBaseline.bestMedianIndex == 1,
          "Absolute best configuration is still identified");
    check(!noBaseline.additionalWorkersImproved,
          "Improvement is not claimed without a baseline");

    const auto failedBaseline =
        analyzeScaling({input(1, 0.0, -1.0, false), input(2, 100.0)});
    check(!failedBaseline.baselineAvailable && !failedBaseline.rows[0].valid,
          "A failed 1-worker run does not provide a baseline");
    check(failedBaseline.bestMedianIndex == 1,
          "Failed configurations are excluded from best-configuration selection");

    const auto zeroBaseline = analyzeScaling({input(1, 0.0), input(2, 100.0)});
    check(!zeroBaseline.baselineAvailable && !zeroBaseline.rows[1].speedup,
          "Zero baseline throughput makes speedup unavailable");

    const auto failedMiddle = analyzeScaling({
        input(1, 100.0),
        input(2, 0.0, -1.0, false),
        input(4, 300.0),
    });
    check(!failedMiddle.rows[1].valid && failedMiddle.rows[2].valid &&
              near(*failedMiddle.rows[2].speedup, 3.0),
          "A failed configuration is marked invalid and skipped, not averaged in");
    check(failedMiddle.decreasedAt.empty(),
          "Comparison skips the failed configuration");

    const auto superLinear = analyzeScaling({input(1, 100.0), input(2, 250.0)});
    check(near(*superLinear.rows[1].efficiencyPercent, 125.0),
          "Efficiency above 100% is reported as measured");

    const auto allSame = analyzeScaling({input(1, 100.0), input(2, 100.0)});
    check(!allSame.additionalWorkersImproved && allSame.decreasedAt.empty(),
          "Equal throughput is neither an improvement nor a decrease");
}

}

int main() {
    std::cout << "========================================\n";
    std::cout << "   JobQ Worker Scaling Statistics Tests\n";
    std::cout << "========================================\n";

    testParsing();
    testRunSummary();
    testFormulas();
    testAnalysis();

    std::cout << "\nTests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0 ? "Result: ALL TESTS PASSED\n"
                                   : "Result: SOME TESTS FAILED\n");

    return testsFailed == 0 ? 0 : 1;
}
