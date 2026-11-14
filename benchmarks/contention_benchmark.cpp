#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "BenchmarkCommon.h"
#include "ScalingAnalysis.h"
#include "StressRunner.h"
#include "metrics/ContentionMetrics.h"
#include "metrics/MeasuredLock.h"

namespace {

using namespace jobq_bench;

constexpr std::uint64_t kMaxJobs = 1'000'000;
constexpr std::uint64_t kMaxProducers = 64;
constexpr std::uint64_t kMaxWorkers = 256;
constexpr std::uint64_t kMaxRuns = 100;
constexpr std::uint64_t kMaxThresholdNs = 1'000'000'000;

enum class Mode { Disabled, Enabled, Compare };

enum class ContentionScenario {
    SingleProducer,
    Concurrent,
    Metrics,
    PriorityRetry
};

struct Config {
    std::uint64_t jobs{50000};
    std::uint64_t producers{4};
    std::uint64_t workers{4};
    ContentionScenario scenario{ContentionScenario::Concurrent};
    std::uint64_t runs{3};
    std::uint64_t warmupJobs{1000};
    Mode mode{Mode::Compare};
    std::uint64_t thresholdNs{jobq::ContentionMetrics::kDefaultThresholdNanoseconds};
    bool matrix{false};
    bool producersGiven{false};
    bool workersGiven{false};
};

struct ParseOutcome {
    bool ok{true};
    bool help{false};
    std::string error;
};

const char* scenarioLabel(ContentionScenario scenario) {
    switch (scenario) {
        case ContentionScenario::SingleProducer: return "single-producer";
        case ContentionScenario::Concurrent: return "concurrent";
        case ContentionScenario::Metrics: return "metrics";
        case ContentionScenario::PriorityRetry: return "priority-retry";
    }
    return "unknown";
}

const char* modeLabel(Mode mode) {
    switch (mode) {
        case Mode::Disabled: return "disabled";
        case Mode::Enabled: return "enabled";
        case Mode::Compare: return "compare";
    }
    return "unknown";
}

void printUsage(std::ostream& out) {
    out << "Usage: jobq_contention_benchmark [options]\n"
           "\n"
           "Measures mutex acquisition waiting and critical-section hold time\n"
           "inside the real JobQ engine and compares runs with the opt-in\n"
           "diagnostics enabled and disabled.\n"
           "\n"
           "Options:\n"
           "  --jobs N                  Total jobs per run, 1.."
        << kMaxJobs << " (default 50000)\n"
        << "  --producers N             Producer threads, 1.." << kMaxProducers
        << " (default 4)\n"
        << "  --workers N               Worker threads, 1.." << kMaxWorkers
        << " (default 4)\n"
        << "  --scenario NAME           single-producer | concurrent | metrics |\n"
           "                            priority-retry (default concurrent)\n"
           "  --runs N                  Measured runs per mode, 1.."
        << kMaxRuns << " (default 3)\n"
        << "  --diagnostics MODE        enabled | disabled | compare (default compare;\n"
           "                            compare runs both modes, alternating order)\n"
           "  --warmup-jobs N           Unmeasured warmup jobs per mode, 0.."
        << kMaxJobs << " (default 1000)\n"
        << "  --contention-threshold-ns N  A lock wait >= N ns counts as 'over\n"
           "                            threshold', 0.." << kMaxThresholdNs
        << " (default "
        << jobq::ContentionMetrics::kDefaultThresholdNanoseconds << ")\n"
        << "  --matrix                  Run the producer/worker pairs 1/1, 2/2, 4/4,\n"
           "                            8/8, 4/8 and 8/4 (cannot be combined with\n"
           "                            --producers or --workers)\n"
           "  --help                    Show this message\n"
           "\n"
           "Scenarios:\n"
           "  single-producer   one producer, --workers consumers, no-op handlers\n"
           "                    (scenarios A and C: queue retrieval with limited\n"
           "                    producer contention)\n"
           "  concurrent        --producers producers and --workers workers with\n"
           "                    lightweight CPU handlers (scenarios B and D)\n"
           "  metrics           concurrent with no-op handlers so that metrics and\n"
           "                    queue updates dominate (scenario E)\n"
           "  priority-retry    priorities plus deterministic retries (scenario F)\n"
           "\n"
           "Example:\n"
           "  jobq_contention_benchmark --jobs 100000 --producers 4 --workers 8 "
           "--scenario concurrent --runs 3 --diagnostics compare\n";
}

ParseOutcome parseArguments(int argc, char** argv, Config& config) {
    ParseOutcome outcome;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h") {
            outcome.help = true;
            return outcome;
        }

        if (argument == "--matrix") {
            config.matrix = true;
            continue;
        }

        std::string name = argument;
        std::string value;
        bool hasValue = false;

        const auto equals = argument.find('=');
        if (argument.rfind("--", 0) == 0 && equals != std::string::npos) {
            name = argument.substr(0, equals);
            value = argument.substr(equals + 1);
            hasValue = true;
        }

        const bool known =
            name == "--jobs" || name == "--producers" || name == "--workers" ||
            name == "--scenario" || name == "--runs" ||
            name == "--diagnostics" || name == "--warmup-jobs" ||
            name == "--contention-threshold-ns";

        if (!known) {
            outcome.ok = false;
            outcome.error = "Unknown argument: " + argument;
            return outcome;
        }

        if (!hasValue) {
            if (index + 1 >= argc) {
                outcome.ok = false;
                outcome.error = "Option " + name + " requires a value";
                return outcome;
            }
            value = argv[++index];
        }

        bool parsed = true;

        if (name == "--jobs") {
            parsed = parseUnsigned(value, 1, kMaxJobs, name, config.jobs, outcome.error);
        } else if (name == "--producers") {
            parsed = parseUnsigned(
                value, 1, kMaxProducers, name, config.producers, outcome.error
            );
            config.producersGiven = true;
        } else if (name == "--workers") {
            parsed = parseUnsigned(
                value, 1, kMaxWorkers, name, config.workers, outcome.error
            );
            config.workersGiven = true;
        } else if (name == "--runs") {
            parsed = parseUnsigned(value, 1, kMaxRuns, name, config.runs, outcome.error);
        } else if (name == "--warmup-jobs") {
            parsed = parseUnsigned(
                value, 0, kMaxJobs, name, config.warmupJobs, outcome.error
            );
        } else if (name == "--contention-threshold-ns") {
            parsed = parseUnsigned(
                value, 0, kMaxThresholdNs, name, config.thresholdNs, outcome.error
            );
        } else if (name == "--diagnostics") {
            if (value == "enabled") {
                config.mode = Mode::Enabled;
            } else if (value == "disabled") {
                config.mode = Mode::Disabled;
            } else if (value == "compare") {
                config.mode = Mode::Compare;
            } else {
                parsed = false;
                outcome.error = "Unknown diagnostics mode '" + value +
                                "' (expected enabled, disabled or compare)";
            }
        } else {
            if (value == "single-producer") {
                config.scenario = ContentionScenario::SingleProducer;
            } else if (value == "concurrent") {
                config.scenario = ContentionScenario::Concurrent;
            } else if (value == "metrics") {
                config.scenario = ContentionScenario::Metrics;
            } else if (value == "priority-retry") {
                config.scenario = ContentionScenario::PriorityRetry;
            } else {
                parsed = false;
                outcome.error =
                    "Unknown scenario '" + value +
                    "' (expected single-producer, concurrent, metrics or "
                    "priority-retry)";
            }
        }

        if (!parsed) {
            outcome.ok = false;
            return outcome;
        }
    }

    if (config.matrix && (config.producersGiven || config.workersGiven)) {
        outcome.ok = false;
        outcome.error =
            "--matrix runs fixed producer/worker pairs and cannot be combined "
            "with --producers or --workers";
    }

    return outcome;
}

StressConfig toStressConfig(
    const Config& config,
    std::uint64_t producers,
    std::uint64_t workers,
    jobq::ContentionMetrics* contention
) {
    StressConfig stress;
    stress.producers = producers;
    stress.workers = workers;
    stress.collectLatency = true;
    stress.contention = contention;

    switch (config.scenario) {
        case ContentionScenario::SingleProducer:
            stress.scenario = StressScenario::SingleProducer;
            stress.workload = Scenario::Noop;
            break;
        case ContentionScenario::Concurrent:
            stress.scenario = StressScenario::Concurrent;
            stress.workload = Scenario::Lightweight;
            break;
        case ContentionScenario::Metrics:
            stress.scenario = StressScenario::Concurrent;
            stress.workload = Scenario::Noop;
            break;
        case ContentionScenario::PriorityRetry:
            stress.scenario = StressScenario::PriorityRetry;
            break;
    }

    return stress;
}

std::uint64_t effectiveProducers(const Config& config, std::uint64_t producers) {
    return config.scenario == ContentionScenario::SingleProducer ? 1 : producers;
}

struct LockTotals {
    jobq::LockStatistics stats;
};

void pool(jobq::LockStatistics& into, const jobq::LockStatistics& add) {
    into.acquisitions += add.acquisitions;
    into.blockedAcquisitions += add.blockedAcquisitions;
    into.overThresholdAcquisitions += add.overThresholdAcquisitions;
    into.totalWaitNanoseconds += add.totalWaitNanoseconds;
    into.maxWaitNanoseconds = std::max(into.maxWaitNanoseconds, add.maxWaitNanoseconds);
    into.holdSamples += add.holdSamples;
    into.totalHoldNanoseconds += add.totalHoldNanoseconds;
    into.maxHoldNanoseconds = std::max(into.maxHoldNanoseconds, add.maxHoldNanoseconds);
    into.clampedSamples += add.clampedSamples;

    for (std::size_t i = 0; i < jobq::kContentionBuckets; ++i) {
        into.waitBuckets[i] += add.waitBuckets[i];
    }
}

struct ModeAggregate {
    Mode mode{Mode::Disabled};
    std::size_t runs{0};
    std::size_t failedRuns{0};
    std::string firstError;

    std::vector<double> throughputs;
    std::vector<double> seconds;
    std::vector<double> queueWaitAverageNs;
    std::vector<double> executionAverageNs;

    std::uint64_t accepted{0};
    std::uint64_t completed{0};
    std::uint64_t failedAttempts{0};
    std::uint64_t retries{0};
    std::uint64_t started{0};

    double threadSeconds{0.0};
    double totalJobsCompleted{0.0};

    std::array<jobq::LockStatistics, jobq::kLockCategoryCount> locks{};
};

void accumulate(
    ModeAggregate& aggregate,
    const StressResult& result,
    std::uint64_t threads,
    jobq::ContentionMetrics* contention
) {
    ++aggregate.runs;

    if (!result.ok) {
        ++aggregate.failedRuns;

        if (aggregate.firstError.empty()) {
            aggregate.firstError = result.error;
        }

        return;
    }

    aggregate.throughputs.push_back(result.throughput);
    aggregate.seconds.push_back(result.seconds);

    const auto wait = result.queueWait.summarize();
    const auto execution = result.executionSuccess.summarize();

    if (wait) {
        aggregate.queueWaitAverageNs.push_back(wait->averageNanoseconds);
    }

    if (execution) {
        aggregate.executionAverageNs.push_back(execution->averageNanoseconds);
    }

    aggregate.accepted = result.accepted;
    aggregate.completed = result.finalCompleted;
    aggregate.failedAttempts = result.failedAttempts;
    aggregate.retries = result.scheduledRetries;
    aggregate.started = result.started;
    aggregate.threadSeconds += result.seconds * static_cast<double>(threads);

    if (contention) {
        for (std::size_t i = 0; i < jobq::kLockCategoryCount; ++i) {
            pool(
                aggregate.locks[i],
                contention->snapshot(static_cast<jobq::LockCategory>(i))
            );
        }
    }
}

double meanOf(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    long double sum = 0.0L;
    for (const double value : values) {
        sum += value;
    }

    return static_cast<double>(sum / static_cast<long double>(values.size()));
}

std::string percent(double value) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << value << "%";
    return text.str();
}

std::string nsText(double nanoseconds) {
    return formatNanoseconds(nanoseconds);
}

struct ConfigOutcome {
    std::uint64_t producers{0};
    std::uint64_t workers{0};
    ModeAggregate disabled;
    ModeAggregate enabled;
    bool hasDisabled{false};
    bool hasEnabled{false};
    std::uint64_t threads{0};
};

StressResult runOne(
    const Config& config,
    std::uint64_t producers,
    std::uint64_t workers,
    std::uint64_t jobs,
    jobq::ContentionMetrics* contention
) {
    return runStress(toStressConfig(config, producers, workers, contention), jobs);
}

ConfigOutcome runConfiguration(
    const Config& config,
    std::uint64_t producers,
    std::uint64_t workers
) {
    ConfigOutcome outcome;
    outcome.producers = effectiveProducers(config, producers);
    outcome.workers = workers;
    outcome.threads = outcome.producers + workers;
    outcome.disabled.mode = Mode::Disabled;
    outcome.enabled.mode = Mode::Enabled;
    outcome.hasDisabled = config.mode != Mode::Enabled;
    outcome.hasEnabled = config.mode != Mode::Disabled;

    if (config.warmupJobs > 0) {
        if (outcome.hasDisabled) {
            const auto warm = runOne(config, producers, workers, config.warmupJobs, nullptr);
            if (!warm.ok) {
                ++outcome.disabled.failedRuns;
                outcome.disabled.firstError = "warmup failed: " + warm.error;
            }
        }

        if (outcome.hasEnabled) {
            jobq::ContentionMetrics contention(config.thresholdNs);
            const auto warm =
                runOne(config, producers, workers, config.warmupJobs, &contention);
            if (!warm.ok) {
                ++outcome.enabled.failedRuns;
                outcome.enabled.firstError = "warmup failed: " + warm.error;
            }
        }
    }

    const auto runDisabled = [&] {
        const auto result = runOne(config, producers, workers, config.jobs, nullptr);
        accumulate(outcome.disabled, result, outcome.threads, nullptr);
    };

    const auto runEnabled = [&] {
        jobq::ContentionMetrics contention(config.thresholdNs);
        const auto result =
            runOne(config, producers, workers, config.jobs, &contention);
        accumulate(outcome.enabled, result, outcome.threads, &contention);
    };

    for (std::uint64_t run = 1; run <= config.runs; ++run) {
        const bool enabledFirst = run % 2 == 0;

        if (config.mode == Mode::Compare) {
            if (enabledFirst) {
                runEnabled();
                runDisabled();
            } else {
                runDisabled();
                runEnabled();
            }
        } else if (config.mode == Mode::Enabled) {
            runEnabled();
        } else {
            runDisabled();
        }
    }

    return outcome;
}

bool modeFailed(const ModeAggregate& aggregate) {
    return aggregate.failedRuns != 0;
}

void printModeResults(
    const Config& config,
    const ModeAggregate& aggregate,
    const std::string& label
) {
    std::cout << "  Diagnostics " << label << "\n"
              << "    Runs:               " << aggregate.runs << " (failed "
              << aggregate.failedRuns << ")\n"
              << "    Accepted Jobs:      " << aggregate.accepted << '\n'
              << "    Completed Jobs:     " << aggregate.completed << '\n'
              << "    Failed Attempts:    " << aggregate.failedAttempts
              << "   Scheduled Retries: " << aggregate.retries << '\n';

    if (aggregate.failedRuns != 0) {
        std::cout << "    ERROR: " << aggregate.firstError << '\n';
    }

    const auto summary = summarizeThroughputs(aggregate.throughputs);

    if (!summary) {
        std::cout << "    No successful runs; no throughput reported.\n\n";
        return;
    }

    std::cout << std::fixed << std::setprecision(2)
              << "    Throughput (jobs/s): median " << summary->median << ", mean "
              << summary->mean << ", min " << summary->minimum << ", max "
              << summary->maximum << '\n'
              << "    Avg Duration:       " << std::setprecision(3)
              << meanOf(aggregate.seconds) << " s\n"
              << "    Avg Queue Wait:     " << nsText(meanOf(aggregate.queueWaitAverageNs)) << '\n'
              << "    Avg Execution:      " << nsText(meanOf(aggregate.executionAverageNs)) << "\n\n";

    (void)config;
}

void printLockTable(const ModeAggregate& aggregate, std::uint64_t thresholdNs) {
    std::cout << "Contention Statistics (diagnostics enabled, pooled over "
              << (aggregate.runs - aggregate.failedRuns)
              << " successful run(s); counts, totals and maxima are directly "
                 "measured, percentiles are histogram estimates)\n\n";

    for (std::size_t i = 0; i < jobq::kLockCategoryCount; ++i) {
        const auto& stats = aggregate.locks[i];
        const auto category = static_cast<jobq::LockCategory>(i);

        std::cout << jobq::lockCategoryName(category) << "\n";

        if (stats.acquisitions == 0) {
            std::cout << "  No acquisitions recorded.\n\n";
            continue;
        }

        const double blockedRatio = 100.0 * static_cast<double>(stats.blockedAcquisitions) /
                                    static_cast<double>(stats.acquisitions);
        const double overRatio =
            100.0 * static_cast<double>(stats.overThresholdAcquisitions) /
            static_cast<double>(stats.acquisitions);
        const double waitShare =
            aggregate.threadSeconds > 0.0
                ? 100.0 * (static_cast<double>(stats.totalWaitNanoseconds) / 1e9) /
                      aggregate.threadSeconds
                : 0.0;

        std::cout << "  Acquisitions:         " << stats.acquisitions << '\n'
                  << "  Blocked Acquisitions: " << stats.blockedAcquisitions << " ("
                  << percent(blockedRatio) << ", try_lock failed)\n"
                  << "  Over " << thresholdNs << " ns:" << std::string(
                         thresholdNs < 10 ? 11 : (thresholdNs < 1000 ? 9 : 6), ' ')
                  << stats.overThresholdAcquisitions << " (" << percent(overRatio) << ")\n"
                  << "  Total Wait:           "
                  << nsText(static_cast<double>(stats.totalWaitNanoseconds)) << '\n'
                  << "  Average Wait:         " << nsText(stats.averageWaitNanoseconds())
                  << '\n'
                  << "  Maximum Wait:         "
                  << nsText(static_cast<double>(stats.maxWaitNanoseconds)) << '\n'
                  << "  Est. p50 / p99 Wait:  "
                  << nsText(static_cast<double>(stats.estimatedWaitPercentileNanoseconds(50)))
                  << " / "
                  << nsText(static_cast<double>(stats.estimatedWaitPercentileNanoseconds(99)))
                  << " (upper bound of histogram bucket)\n"
                  << "  Hold Samples:         " << stats.holdSamples << '\n'
                  << "  Total Hold Time:      "
                  << nsText(static_cast<double>(stats.totalHoldNanoseconds)) << '\n'
                  << "  Average Hold Time:    " << nsText(stats.averageHoldNanoseconds())
                  << '\n'
                  << "  Maximum Hold Time:    "
                  << nsText(static_cast<double>(stats.maxHoldNanoseconds)) << '\n'
                  << "  Wait Share Of Thread Time: " << percent(waitShare) << '\n';

        if (stats.clampedSamples != 0) {
            std::cout << "  Clamped Samples:      " << stats.clampedSamples << '\n';
        }

        std::cout << '\n';
    }
}

void printOverhead(const ConfigOutcome& outcome) {
    const auto off = summarizeThroughputs(outcome.disabled.throughputs);
    const auto on = summarizeThroughputs(outcome.enabled.throughputs);

    std::cout << "Instrumentation Overhead (median of successful runs; diagnostics-"
                 "enabled numbers are not unmodified production performance)\n\n";

    if (!off || !on) {
        std::cout << "  Unavailable: both modes need at least one successful run.\n\n";
        return;
    }

    const auto relative = [](double base, double value) {
        return base > 0.0 ? 100.0 * (value - base) / base : 0.0;
    };

    const double execOff = meanOf(outcome.disabled.executionAverageNs);
    const double execOn = meanOf(outcome.enabled.executionAverageNs);
    const double secOff = meanOf(outcome.disabled.seconds);
    const double secOn = meanOf(outcome.enabled.seconds);
    const double waitOff = meanOf(outcome.disabled.queueWaitAverageNs);
    const double waitOn = meanOf(outcome.enabled.queueWaitAverageNs);

    std::cout << std::fixed << std::setprecision(2)
              << "  Throughput:           disabled " << off->median << "  enabled "
              << on->median << " jobs/s  (" << std::showpos
              << relative(off->median, on->median) << std::noshowpos << "%)\n"
              << "  Avg Execution:        disabled " << nsText(execOff) << "  enabled "
              << nsText(execOn) << "  (" << std::showpos << relative(execOff, execOn)
              << std::noshowpos << "%)\n"
              << "  Avg Queue Wait:       disabled " << nsText(waitOff) << "  enabled "
              << nsText(waitOn) << "  (" << std::showpos << relative(waitOff, waitOn)
              << std::noshowpos << "%)\n"
              << std::setprecision(3)
              << "  Workload Duration:    disabled " << secOff << " s  enabled " << secOn
              << " s  (" << std::showpos << std::setprecision(2)
              << relative(secOff, secOn) << std::noshowpos << "%)\n\n";
}

struct Ranked {
    std::size_t index{0};
    double value{0.0};
};

void printAnalysis(const ConfigOutcome& outcome) {
    const auto& locks = outcome.enabled.locks;
    const double threadSeconds = outcome.enabled.threadSeconds;

    std::cout << "Analysis (descriptive; correlation is not causation)\n\n";

    bool any = false;
    std::vector<Ranked> byTotal;
    std::vector<Ranked> byAverage;

    for (std::size_t i = 0; i < jobq::kLockCategoryCount; ++i) {
        if (locks[i].acquisitions == 0) {
            continue;
        }
        any = true;
        byTotal.push_back({i, static_cast<double>(locks[i].totalWaitNanoseconds)});
        byAverage.push_back({i, locks[i].averageWaitNanoseconds()});
    }

    if (!any) {
        std::cout << "  No lock activity was recorded.\n\n";
        return;
    }

    const auto best = [](const std::vector<Ranked>& ranked) {
        return *std::max_element(
            ranked.begin(), ranked.end(),
            [](const Ranked& a, const Ranked& b) { return a.value < b.value; }
        );
    };

    const auto topTotal = best(byTotal);
    const auto topAverage = best(byAverage);

    const auto name = [](std::size_t i) {
        return jobq::lockCategoryName(static_cast<jobq::LockCategory>(i));
    };

    const double topShare =
        threadSeconds > 0.0 ? 100.0 * (topTotal.value / 1e9) / threadSeconds : 0.0;

    std::cout << "  Highest total wait:   " << name(topTotal.index) << " ("
              << nsText(topTotal.value) << ", " << percent(topShare)
              << " of aggregate thread time)\n"
              << "  Highest average wait: " << name(topAverage.index) << " ("
              << nsText(topAverage.value) << ")\n";

    double maxBlocked = 0.0;
    std::size_t maxBlockedIndex = topTotal.index;

    for (std::size_t i = 0; i < jobq::kLockCategoryCount; ++i) {
        if (locks[i].acquisitions == 0) {
            continue;
        }

        const double ratio = 100.0 * static_cast<double>(locks[i].blockedAcquisitions) /
                             static_cast<double>(locks[i].acquisitions);

        if (ratio > maxBlocked) {
            maxBlocked = ratio;
            maxBlockedIndex = i;
        }
    }

    std::cout << "  Highest blocking rate: " << name(maxBlockedIndex) << " ("
              << percent(maxBlocked) << " of acquisitions had to block)\n";

    if (maxBlocked < 1.0) {
        std::cout << "  Pattern: no lock blocked in more than 1% of acquisitions in "
                     "this configuration.\n";
    } else if (topShare >= 5.0) {
        std::cout << "  Pattern: " << name(topTotal.index)
                  << " accounts for a noticeable share of aggregate thread time; "
                     "compare with the throughput table before drawing "
                     "conclusions.\n";
    } else {
        std::cout << "  Pattern: blocking is present but each lock's total wait is a "
                     "small share of aggregate thread time (highest "
                  << percent(topShare) << ").\n";
    }

    std::cout << '\n';
}

void printConfigurationReport(
    const Config& config,
    const ConfigOutcome& outcome,
    bool detailed
) {
    std::cout << "Configuration\n"
              << "  Scenario:             " << scenarioLabel(config.scenario) << '\n'
              << "  Jobs:                 " << config.jobs << '\n'
              << "  Producers:            " << outcome.producers << '\n'
              << "  Workers:              " << outcome.workers << '\n'
              << "  Runs Per Mode:        " << config.runs << '\n'
              << "  Diagnostics:          " << modeLabel(config.mode) << "\n\n"
              << "Workload Results\n";

    if (outcome.hasDisabled) {
        printModeResults(config, outcome.disabled, "disabled");
    }

    if (outcome.hasEnabled) {
        printModeResults(config, outcome.enabled, "enabled");
    }

    if (outcome.hasEnabled && outcome.enabled.runs > outcome.enabled.failedRuns) {
        if (detailed) {
            printLockTable(outcome.enabled, config.thresholdNs);
        }
    }

    if (config.mode == Mode::Compare) {
        printOverhead(outcome);
    }

    if (outcome.hasEnabled && outcome.enabled.runs > outcome.enabled.failedRuns) {
        printAnalysis(outcome);
    }
}

struct Calibration {
    double clockNs{0.0};
    double plainLockNs{0.0};
    double disabledLockNs{0.0};
    double enabledLockNs{0.0};
    double recordNs{0.0};
};

Calibration calibrate() {
    constexpr int kIterations = 1'000'000;
    using Clock = std::chrono::steady_clock;

    Calibration result;
    volatile std::uint64_t sink = 0;
    std::mutex mutex;
    jobq::ContentionMetrics metrics;

    const auto perOp = [](Clock::time_point start, Clock::time_point end) {
        return std::chrono::duration<double, std::nano>(end - start).count() /
               static_cast<double>(kIterations);
    };

    {
        const auto start = Clock::now();
        for (int i = 0; i < kIterations; ++i) {
            sink = sink + static_cast<std::uint64_t>(
                              Clock::now().time_since_epoch().count() & 1);
        }
        result.clockNs = perOp(start, Clock::now());
    }

    {
        const auto start = Clock::now();
        for (int i = 0; i < kIterations; ++i) {
            std::lock_guard<std::mutex> lock(mutex);
            sink = sink + 1;
        }
        result.plainLockNs = perOp(start, Clock::now());
    }

    {
        const auto start = Clock::now();
        for (int i = 0; i < kIterations; ++i) {
            jobq::MeasuredMutexLock lock(mutex, nullptr, jobq::LockCategory::JobQueue);
            sink = sink + 1;
        }
        result.disabledLockNs = perOp(start, Clock::now());
    }

    {
        const auto start = Clock::now();
        for (int i = 0; i < kIterations; ++i) {
            jobq::MeasuredMutexLock lock(mutex, &metrics, jobq::LockCategory::JobQueue);
            sink = sink + 1;
        }
        result.enabledLockNs = perOp(start, Clock::now());
    }

    {
        const auto start = Clock::now();
        for (int i = 0; i < kIterations; ++i) {
            metrics.recordAcquisition(jobq::LockCategory::JobMetrics, 0, false);
            metrics.recordHold(jobq::LockCategory::JobMetrics, 10);
        }
        result.recordNs = perOp(start, Clock::now());
    }

    (void)sink;
    return result;
}

void printCalibration(const Calibration& c) {
    std::cout << "Instrumentation Cost Calibration (single thread, uncontended, "
                 "per operation)\n"
              << std::fixed << std::setprecision(1)
              << "  steady_clock::now():             " << c.clockNs << " ns\n"
              << "  lock_guard<std::mutex>:          " << c.plainLockNs << " ns\n"
              << "  MeasuredLock, diagnostics off:   " << c.disabledLockNs << " ns\n"
              << "  MeasuredLock, diagnostics on:    " << c.enabledLockNs << " ns\n"
              << "  recordAcquisition + recordHold:  " << c.recordNs << " ns\n\n";
}

}

int main(int argc, char** argv) {
    Config config;
    const auto outcome = parseArguments(argc, argv, config);

    if (outcome.help) {
        printUsage(std::cout);
        return 0;
    }

    if (!outcome.ok) {
        std::cerr << "Error: " << outcome.error << "\n\n";
        printUsage(std::cerr);
        return 2;
    }

    std::cout << "========== JobQ Concurrency & Contention Analysis ==========\n\n";

    printCalibration(calibrate());

    bool allOk = true;

    if (!config.matrix) {
        const auto result = runConfiguration(config, config.producers, config.workers);
        printConfigurationReport(config, result, true);

        allOk = !modeFailed(result.disabled) && !modeFailed(result.enabled);
    } else {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs = {
            {1, 1}, {2, 2}, {4, 4}, {8, 8}, {4, 8}, {8, 4}};

        std::vector<ConfigOutcome> outcomes;

        for (const auto& [producers, workers] : pairs) {
            auto result = runConfiguration(config, producers, workers);
            std::cout << "---- " << producers << " producer(s), " << workers
                      << " worker(s) ----\n\n";
            printConfigurationReport(config, result, true);
            allOk = allOk && !modeFailed(result.disabled) && !modeFailed(result.enabled);
            outcomes.push_back(std::move(result));
        }

        std::cout << "Matrix Comparison (" << scenarioLabel(config.scenario) << ", "
                  << config.jobs << " jobs, medians of successful runs)\n\n"
                  << std::left << std::setw(6) << "Prod" << std::setw(6) << "Work"
                  << std::right << std::setw(14) << "Off jobs/s" << std::setw(14)
                  << "On jobs/s" << std::setw(10) << "Overhead" << std::setw(12)
                  << "QueueWait%" << std::setw(12) << "Registry%" << std::setw(12)
                  << "Metrics%" << std::setw(12) << "Executor%" << '\n';

        for (const auto& item : outcomes) {
            const auto off = summarizeThroughputs(item.disabled.throughputs);
            const auto on = summarizeThroughputs(item.enabled.throughputs);

            std::cout << std::left << std::setw(6) << item.producers << std::setw(6)
                      << item.workers << std::right << std::fixed << std::setprecision(0)
                      << std::setw(14);

            if (off) {
                std::cout << off->median;
            } else {
                std::cout << "n/a";
            }

            std::cout << std::setw(14);

            if (on) {
                std::cout << on->median;
            } else {
                std::cout << "n/a";
            }

            std::cout << std::setw(10);

            if (off && on && off->median > 0.0) {
                std::cout << percent(100.0 * (on->median - off->median) / off->median);
            } else {
                std::cout << "n/a";
            }

            for (const auto category : {
                     jobq::LockCategory::JobQueue,
                     jobq::LockCategory::JobRegistry,
                     jobq::LockCategory::JobMetrics,
                     jobq::LockCategory::JobExecutor}) {
                const auto& stats = item.enabled.locks[static_cast<std::size_t>(category)];
                std::cout << std::setw(12);

                if (stats.acquisitions == 0) {
                    std::cout << "n/a";
                } else {
                    std::cout << percent(
                        100.0 * static_cast<double>(stats.blockedAcquisitions) /
                        static_cast<double>(stats.acquisitions)
                    );
                }
            }

            std::cout << '\n';
        }

        std::cout << "\n(Columns QueueWait%..Executor% = share of acquisitions that had "
                     "to block, diagnostics enabled.)\n\n";
    }

    std::cout << "  Benchmark Status: " << (allOk ? "PASS" : "FAIL")
              << "\n\n============================================================\n";

    return allOk ? 0 : 1;
}
