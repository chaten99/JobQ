#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "BenchmarkCommon.h"
#include "ScalingAnalysis.h"
#include "StressRunner.h"

namespace {

using namespace jobq_bench;

constexpr std::uint64_t kMaxJobs = 1'000'000;
constexpr std::uint64_t kMaxProducers = 64;
constexpr std::uint64_t kMaxWorkers = 256;
constexpr std::uint64_t kMaxRuns = 100;

struct Config {
    std::uint64_t jobs{40000};
    std::uint64_t producers{4};
    std::uint64_t workers{4};
    StressScenario scenario{StressScenario::Concurrent};
    std::uint64_t runs{3};
    std::uint64_t warmupJobs{1000};
    Scenario workload{Scenario::Noop};
    bool latency{true};
    bool producersGiven{false};
    bool workloadGiven{false};
};

struct ParseOutcome {
    bool ok{true};
    bool help{false};
    std::string error;
};

void printUsage(std::ostream& out) {
    out << "Usage: jobq_queue_stress_benchmark [options]\n"
           "\n"
           "Stress-tests the real JobQ engine with concurrent producers and\n"
           "workers and verifies exact job accounting after every run.\n"
           "\n"
           "Options:\n"
           "  --jobs N           Total jobs per run, 1.."
        << kMaxJobs << " (default 40000)\n"
        << "  --producers N      Producer threads, 1.." << kMaxProducers
        << " (default 4)\n"
        << "  --workers N        Worker threads, 1.." << kMaxWorkers
        << " (default 4)\n"
        << "  --scenario NAME    single-producer | concurrent | sustained |\n"
           "                     priority | variable | failure | priority-retry | large\n"
           "                     (default concurrent)\n"
           "  --runs N           Measured runs, 1.."
        << kMaxRuns << " (default 3)\n"
        << "  --warmup-jobs N    Jobs in the unmeasured warmup run, 0.."
        << kMaxJobs << " (default 1000; 0 disables)\n"
        << "  --workload NAME    noop | lightweight | mixed handler cost for\n"
           "                     the scenarios that do not fix it (default noop)\n"
           "  --no-latency       Disable latency collection\n"
           "  --help             Show this message\n"
           "\n"
           "Scenarios:\n"
           "  single-producer    exactly one producer (--producers is ignored)\n"
           "  concurrent         --producers threads submit while workers run\n"
           "  sustained          concurrent + sampled queue depth monitor\n"
           "  priority           20% High / 50% Normal / 30% Low jobs\n"
           "  variable           mixed-cost handlers (implies --workload mixed)\n"
           "  failure            deterministic failures and retries\n"
           "  priority-retry     priorities combined with deterministic retries\n"
           "  large              concurrent over 10000, 50000, 100000, 250000\n"
           "                     (sizes up to --jobs; --jobs itself is added)\n"
           "\n"
           "--jobs is the total across all producers. Options accept both\n"
           "'--name value' and '--name=value'.\n"
           "Example:\n"
           "  jobq_queue_stress_benchmark --jobs 100000 --producers 4 "
           "--workers 8 --scenario concurrent --runs 3 --warmup-jobs 1000\n";
}

ParseOutcome parseArguments(int argc, char** argv, Config& config) {
    ParseOutcome outcome;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h") {
            outcome.help = true;
            return outcome;
        }

        if (argument == "--no-latency") {
            config.latency = false;
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
            name == "--jobs" || name == "--producers" ||
            name == "--workers" || name == "--scenario" ||
            name == "--runs" || name == "--warmup-jobs" ||
            name == "--workload";

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
        } else if (name == "--runs") {
            parsed = parseUnsigned(value, 1, kMaxRuns, name, config.runs, outcome.error);
        } else if (name == "--warmup-jobs") {
            parsed = parseUnsigned(
                value, 0, kMaxJobs, name, config.warmupJobs, outcome.error
            );
        } else if (name == "--scenario") {
            const auto scenario = parseStressScenario(value);

            if (scenario) {
                config.scenario = *scenario;
            } else {
                parsed = false;
                outcome.error =
                    "Unknown scenario '" + value +
                    "' (expected single-producer, concurrent, sustained, "
                    "priority, variable, failure, priority-retry or large)";
            }
        } else {
            config.workloadGiven = true;

            if (value == "noop") {
                config.workload = Scenario::Noop;
            } else if (value == "lightweight") {
                config.workload = Scenario::Lightweight;
            } else if (value == "mixed") {
                config.workload = Scenario::Mixed;
            } else {
                parsed = false;
                outcome.error =
                    "Unknown workload '" + value +
                    "' (expected noop, lightweight or mixed)";
            }
        }

        if (!parsed) {
            outcome.ok = false;
            return outcome;
        }
    }

    if (config.scenario == StressScenario::Variable && config.workloadGiven &&
        config.workload != Scenario::Mixed) {
        outcome.ok = false;
        outcome.error =
            "Scenario 'variable' always uses the mixed workload; remove "
            "--workload or use --workload mixed";
    }

    if ((config.scenario == StressScenario::Failure ||
         config.scenario == StressScenario::PriorityRetry) &&
        config.workloadGiven) {
        outcome.ok = false;
        outcome.error =
            "Scenarios failure and priority-retry use their own deterministic handlers; "
            "--workload cannot be combined with them";
    }

    return outcome;
}

StressConfig toStressConfig(const Config& config) {
    StressConfig stress;
    stress.scenario = config.scenario;
    stress.workload = config.workload;
    stress.producers = config.producers;
    stress.workers = config.workers;
    stress.collectLatency = config.latency;
    return stress;
}

std::uint64_t effectiveProducers(const Config& config) {
    return config.scenario == StressScenario::SingleProducer ? 1
                                                             : config.producers;
}

const char* yesNo(bool value) {
    return value ? "Yes" : "No";
}

void printRunLatency(std::ostream& out, const StressResult& run) {
    if (!run.latencyCollected) {
        return;
    }

    printLatencyLine(out, "Queue Wait", run.queueWait);
    printLatencyLine(out, "Exec (ok)", run.executionSuccess);
    printLatencyLine(out, "Exec (failed)", run.executionFailure);

    if (run.priorityCollected) {
        printLatencyLine(out, "Wait: High", run.priorityWait[2]);
        printLatencyLine(out, "Wait: Normal", run.priorityWait[1]);
        printLatencyLine(out, "Wait: Low", run.priorityWait[0]);
    }
}

void printRunBlock(
    std::ostream& out,
    const std::string& title,
    const Config& config,
    const StressResult& run
) {
    out << title << "\n"
        << "  Scenario:             " << stressScenarioName(config.scenario) << '\n'
        << "  Requested Jobs:       " << run.requested << '\n'
        << "  Producer Threads:     " << effectiveProducers(config) << '\n'
        << "  Worker Threads:       " << config.workers << "\n\n"
        << "Submission Statistics\n"
        << "  Attempted:            " << run.attempted << '\n'
        << "  Accepted:             " << run.accepted << '\n'
        << "  Rejected:             " << run.rejected << '\n'
        << "  Unique Job IDs:       " << run.uniqueIds << '\n'
        << "  Registry Entries:     " << run.registrySize << "\n\n"
        << "Execution Statistics\n"
        << "  Completed Jobs:       " << run.finalCompleted << '\n'
        << "  Permanently Failed:   " << run.finalFailed << '\n'
        << "  Started Executions:   " << run.started << '\n'
        << "  Finished Executions:  " << run.finished << '\n'
        << "  Completed Attempts:   " << run.completedAttempts << '\n'
        << "  Failed Attempts:      " << run.failedAttempts << '\n'
        << "  Scheduled Retries:    " << run.scheduledRetries << "\n\n"
        << "Queue Statistics\n"
        << "  Final Queue Size:     " << run.finalQueueSize << '\n'
        << "  Queue Empty:          " << yesNo(run.queueEmpty) << '\n';

    if (run.depthSampled) {
        out << "  Sampled Depth Max:    " << run.depthMax << " (" << run.depthSamples
            << " samples; sampled, not an exact peak)\n";
    }

    out << "\nPerformance\n"
        << std::fixed << std::setprecision(3)
        << "  Total Duration:       " << run.seconds
        << " seconds (first submission to workers joined)\n"
        << "  Producer Phase:       " << run.producerSeconds << " seconds\n"
        << std::setprecision(2);

    if (run.ok) {
        out << "  Overall Throughput:   " << run.throughput
            << " jobs/sec (completed jobs / total duration)\n"
            << "  Submission Rate:      " << run.submissionRate
            << " jobs/sec (accepted / producer phase)\n";
    } else {
        out << "  Overall Throughput:   n/a (run unsuccessful)\n";
    }

    out << "  Metrics Avg Exec:     "
        << formatNanoseconds(run.metricsAverageUs * 1000.0) << '\n';

    printRunLatency(out, run);

    out << "\n  Run Status: " << (run.ok ? "PASS" : "FAIL") << '\n';

    if (!run.ok) {
        out << "  ERROR: " << run.error << '\n';
    }

    out << '\n';
}

void printCompactRun(
    std::ostream& out,
    std::uint64_t number,
    const StressResult& run
) {
    out << "  Run " << number << ": accepted " << run.accepted << "/"
        << run.requested << ", completed " << run.finalCompleted
        << ", rejected " << run.rejected << std::fixed << std::setprecision(3)
        << ", duration " << run.seconds << " s" << std::setprecision(2);

    if (run.ok) {
        out << ", throughput " << run.throughput << " jobs/sec, submission "
            << run.submissionRate << " jobs/sec, PASS\n";
    } else {
        out << ", FAIL - " << run.error << '\n';
    }
}

struct SizeOutcome {
    std::uint64_t jobs{0};
    std::size_t failedRuns{0};
    std::size_t runs{0};
    std::optional<RunSummary> summary;
};

SizeOutcome runSize(
    const Config& config,
    std::uint64_t jobs,
    bool verbose
) {
    SizeOutcome outcome;
    outcome.jobs = jobs;
    outcome.runs = static_cast<std::size_t>(config.runs);

    const StressConfig stress = toStressConfig(config);
    std::vector<double> throughputs;

    for (std::uint64_t run = 1; run <= config.runs; ++run) {
        const auto result = runStress(stress, jobs);

        if (verbose) {
            printRunBlock(std::cout, "Run " + std::to_string(run), config, result);
        } else {
            printCompactRun(std::cout, run, result);
        }

        if (result.ok) {
            throughputs.push_back(result.throughput);
        } else {
            ++outcome.failedRuns;
        }
    }

    outcome.summary = summarizeThroughputs(throughputs);
    return outcome;
}

void printSummary(
    std::ostream& out,
    const std::string& heading,
    const SizeOutcome& outcome
) {
    out << heading << "\n\n" << std::fixed << std::setprecision(2)
        << "  Runs:                 " << outcome.runs << '\n'
        << "  Failed Runs:          " << outcome.failedRuns << '\n';

    if (outcome.summary) {
        out << "  Successful Runs:      " << outcome.summary->count << '\n'
            << "  Minimum Throughput:   " << outcome.summary->minimum
            << " jobs/sec\n"
            << "  Maximum Throughput:   " << outcome.summary->maximum
            << " jobs/sec\n"
            << "  Mean Throughput:      " << outcome.summary->mean
            << " jobs/sec\n"
            << "  Median Throughput:    " << outcome.summary->median
            << " jobs/sec\n";
    } else {
        out << "  No successful runs; no throughput reported.\n";
    }

    out << '\n';
}

std::vector<std::uint64_t> largeSizes(std::uint64_t maxJobs) {
    std::vector<std::uint64_t> sizes;

    for (const std::uint64_t size : {10000ULL, 50000ULL, 100000ULL, 250000ULL}) {
        if (size <= maxJobs) {
            sizes.push_back(size);
        }
    }

    if (sizes.empty() || sizes.back() != maxJobs) {
        sizes.push_back(maxJobs);
    }

    return sizes;
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

    const bool large = config.scenario == StressScenario::Large;
    const auto workload =
        config.scenario == StressScenario::Variable ? Scenario::Mixed
                                                     : config.workload;

    std::cout << "========== JobQ Queue Stress Benchmark ==========\n\n"
              << "Configuration\n"
              << "  Scenario:             " << stressScenarioName(config.scenario)
              << '\n'
              << "  Requested Jobs:       " << config.jobs << " (total across producers)\n"
              << "  Producer Threads:     " << effectiveProducers(config);

    if (config.scenario == StressScenario::SingleProducer && config.producersGiven &&
        config.producers != 1) {
        std::cout << " (scenario single-producer ignores --producers "
                  << config.producers << ")";
    }

    std::cout << '\n'
              << "  Worker Threads:       " << config.workers << '\n'
              << "  Repetitions:          " << config.runs << '\n'
              << "  Warmup Jobs:          " << config.warmupJobs << '\n';

    if (config.scenario == StressScenario::Failure ||
        config.scenario == StressScenario::PriorityRetry) {
        std::cout << "  Handlers:             deterministic failure/retry mix\n";
    } else {
        std::cout << "  Workload:             " << scenarioName(workload) << '\n';
    }

    std::cout << "  Latency Collection:   "
              << (config.latency ? "enabled" : "disabled (--no-latency)") << '\n'
              << "  Per-job logging:      suppressed during runs\n\n"
              << std::flush;

    bool allOk = true;

    if (config.warmupJobs > 0) {
        const auto warmup = runStress(toStressConfig(config), config.warmupJobs);

        if (!warmup.ok) {
            std::cout << "Warmup run FAILED: " << warmup.error << "\n\n";
            allOk = false;
        } else {
            std::cout << "Warmup completed (" << warmup.finalCompleted
                      << " jobs, not included in results)\n\n";
        }
    }

    std::cout << "Run Results\n\n";

    if (large) {
        std::vector<SizeOutcome> outcomes;

        for (const auto size : largeSizes(config.jobs)) {
            std::cout << "Job Count: " << size << "\n";
            outcomes.push_back(runSize(config, size, false));
            std::cout << '\n';
        }

        std::cout << "Large-Volume Summary\n\n"
                  << std::left << std::setw(10) << "Jobs" << std::right
                  << std::setw(8) << "Failed" << std::setw(16) << "Median jobs/s"
                  << std::setw(16) << "Mean jobs/s" << std::setw(16)
                  << "Min jobs/s" << std::setw(16) << "Max jobs/s" << '\n'
                  << std::fixed << std::setprecision(2);

        for (const auto& item : outcomes) {
            allOk = allOk && item.failedRuns == 0;
            std::cout << std::left << std::setw(10) << item.jobs << std::right
                      << std::setw(8) << item.failedRuns;

            if (item.summary) {
                std::cout << std::setw(16) << item.summary->median << std::setw(16)
                          << item.summary->mean << std::setw(16)
                          << item.summary->minimum << std::setw(16)
                          << item.summary->maximum;
            } else {
                std::cout << "  no successful runs";
            }

            std::cout << '\n';
        }

        std::cout << '\n';
    } else {
        const auto result = runSize(config, config.jobs, true);
        allOk = allOk && result.failedRuns == 0;
        printSummary(std::cout, "Performance Summary", result);
    }

    std::cout << "  Benchmark Status: " << (allOk ? "PASS" : "FAIL")
              << "\n\n=================================================\n";

    return allOk ? 0 : 1;
}
