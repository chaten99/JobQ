#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "BenchmarkCommon.h"
#include "ScalingAnalysis.h"
#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/ExecutionObserver.h"
#include "metrics/JobMetrics.h"
#include "metrics/LatencyStatistics.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace {

using namespace jobq_bench;

constexpr std::uint64_t kMaxJobs = 2'000'000;
constexpr std::uint64_t kMaxWorkers = 256;
constexpr std::uint64_t kMaxRuns = 100;
constexpr std::uint64_t kMaxPooledSamples = 5000000;

struct Config {
    std::uint64_t jobs{10000};
    std::uint64_t workers{4};
    Scenario scenario{Scenario::Noop};
    std::uint64_t runs{3};
    std::uint64_t warmupJobs{1000};
    bool latency{true};
    std::vector<std::uint64_t> workerCounts;
    bool workersGiven{false};
    bool scalingMode{false};
};

struct ParseOutcome {
    bool ok{true};
    bool help{false};
    std::string error;
};

void printUsage(std::ostream& out) {
    out << "Usage: jobq_throughput_benchmark [options]\n"
           "\n"
           "Measures end-to-end JobQ throughput (submit, queue, execute,\n"
           "complete, drain, join) using the real engine components.\n"
           "\n"
           "Options:\n"
           "  --jobs N           Jobs per measured run, 1.."
        << kMaxJobs << " (default 10000)\n"
        << "  --workers N        Worker threads, 1.." << kMaxWorkers
        << " (default 4)\n"
        << "  --worker-counts L  Worker-scaling mode: comma-separated list, e.g. 1,2,4,8\n"
           "                     (each 1.."  << kMaxWorkers << ", no duplicates, at most "
        << jobq_bench::kMaxWorkerCountEntries << " entries; cannot be\n"
           "                     combined with --workers)\n"
        << "  --scenario NAME    noop | lightweight | mixed (default noop)\n"
           "  --runs N           Measured runs, 1.."
        << kMaxRuns << " (default 3)\n"
        << "  --warmup-jobs N    Jobs in the unmeasured warmup run, 0.."
        << kMaxJobs << " (default 1000; 0 disables warmup)\n"
        << "  --no-latency       Disable latency collection (throughput only)\n"
        << "  --help             Show this message\n"
           "\n"
           "Options accept both '--name value' and '--name=value'.\n"
           "Example:\n"
           "  jobq_throughput_benchmark --jobs 100000 --workers 4 "
           "--scenario noop --runs 5 --warmup-jobs 1000\n"
           "  jobq_throughput_benchmark --jobs 100000 --worker-counts 1,2,4,8 "
           "--scenario lightweight --runs 5 --warmup-jobs 1000\n";
}

ParseOutcome parseArguments(int argc, char** argv, Config& config) {
    ParseOutcome outcome;

    for (int index = 1; index < argc; ++index) {
        std::string argument = argv[index];

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
            name == "--jobs" || name == "--workers" ||
            name == "--scenario" || name == "--runs" ||
            name == "--warmup-jobs" || name == "--worker-counts";

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

        if (name == "--worker-counts") {
            if (config.scalingMode) {
                outcome.ok = false;
                outcome.error = "Option --worker-counts was specified more than once";
                return outcome;
            }

            const auto list =
                jobq_bench::parseWorkerCounts(value, kMaxWorkers);

            if (!list.ok) {
                outcome.ok = false;
                outcome.error = list.error;
                return outcome;
            }

            config.workerCounts = list.counts;
            config.scalingMode = true;
            continue;
        }

        if (name == "--jobs") {
            parsed = parseUnsigned(
                value, 1, kMaxJobs, name, config.jobs, outcome.error
            );
        } else if (name == "--workers") {
            parsed = parseUnsigned(
                value, 1, kMaxWorkers, name, config.workers, outcome.error
            );
            config.workersGiven = true;
        } else if (name == "--runs") {
            parsed = parseUnsigned(
                value, 1, kMaxRuns, name, config.runs, outcome.error
            );
        } else if (name == "--warmup-jobs") {
            parsed = parseUnsigned(
                value, 0, kMaxJobs, name, config.warmupJobs, outcome.error
            );
        } else {
            if (value == "noop") {
                config.scenario = Scenario::Noop;
            } else if (value == "lightweight") {
                config.scenario = Scenario::Lightweight;
            } else if (value == "mixed") {
                config.scenario = Scenario::Mixed;
            } else {
                parsed = false;
                outcome.error =
                    "Unknown scenario '" + value +
                    "' (expected noop, lightweight or mixed)";
            }
        }

        if (!parsed) {
            outcome.ok = false;
            return outcome;
        }
    }

    if (config.scalingMode && config.workersGiven) {
        outcome.ok = false;
        outcome.error =
            "Options --workers and --worker-counts cannot be combined; "
            "use --workers N for a single configuration or "
            "--worker-counts LIST for a scaling comparison";
    }

    return outcome;
}

struct RunResult {
    bool ok{false};
    std::string error;

    std::uint64_t requested{0};
    std::uint64_t accepted{0};
    std::uint64_t submitted{0};
    std::uint64_t completed{0};
    std::uint64_t failed{0};
    std::uint64_t started{0};
    std::uint64_t finished{0};

    double seconds{0.0};
    double throughput{0.0};

    double metricsTotalMs{0.0};
    double metricsAverageUs{0.0};
    double metricsThroughput{0.0};

    bool latencyCollected{false};
    jobq::LatencySamples queueWait;
    jobq::LatencySamples executionSuccess;
    jobq::LatencySamples executionFailure;
    jobq::LatencySamples endToEnd;
};

RunResult runOnce(
    Scenario scenario,
    std::uint64_t jobs,
    std::uint64_t workers,
    bool collectLatency
) {
    RunResult result;
    result.requested = jobs;

    StreamSilencer silencer;

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    jobq::JobExecutor executor;

    std::atomic<std::uint64_t> checksum{0};
    std::atomic<std::uint64_t> handlerCalls{0};

    const auto kinds = kindsFor(scenario);

    for (const auto& kind : kinds) {
        const Work work = kind.work;
        executor.registerHandler(
            kind.type,
            [&checksum, &handlerCalls, work](const jobq::Job& job) {
                checksum.fetch_add(
                    runWork(work, job.getId()),
                    std::memory_order_relaxed
                );
                handlerCalls.fetch_add(1, std::memory_order_relaxed);
            }
        );
    }

    std::vector<SteadyClock::time_point> submitStarts;
    std::unique_ptr<LatencyCollector> collector;

    if (collectLatency) {
        submitStarts.resize(static_cast<std::size_t>(jobs) + 1);
        collector = std::make_unique<LatencyCollector>(
            static_cast<std::size_t>(workers),
            static_cast<std::size_t>(jobs),
            &submitStarts
        );
    }

    std::unique_ptr<jobq::WorkerPool> poolOwner;

    if (collector) {
        poolOwner = std::make_unique<jobq::WorkerPool>(
            queue,
            executor,
            static_cast<std::size_t>(workers),
            metrics,
            *collector
        );
    } else {
        poolOwner = std::make_unique<jobq::WorkerPool>(
            queue, executor, static_cast<std::size_t>(workers), metrics
        );
    }

    jobq::WorkerPool& pool = *poolOwner;

    std::vector<std::shared_ptr<jobq::Job>> accepted;
    accepted.reserve(static_cast<std::size_t>(jobs));

    std::string submissionError;

    try {
        pool.start();
    } catch (const std::exception& exception) {
        result.error = std::string("Worker pool failed to start: ") +
                       exception.what();
        return result;
    }

    const auto startTime = SteadyClock::now();

    try {
        for (std::uint64_t i = 1; i <= jobs; ++i) {
            const auto& kind = kinds[kindIndexFor(scenario, i)];

            if (collectLatency) {
                submitStarts[i] = SteadyClock::now();
            }

            accepted.push_back(manager.submit(kind.type, "payload"));
        }
    } catch (const std::exception& exception) {
        submissionError = exception.what();
    }

    try {
        pool.stop();
    } catch (const std::exception& exception) {
        result.error = std::string("Worker pool failed to stop: ") +
                       exception.what();
        return result;
    }

    const auto endTime = SteadyClock::now();

    const auto snapshot = metrics.snapshot();

    result.accepted = accepted.size();
    result.submitted = snapshot.submittedJobs;
    result.completed = snapshot.completedExecutions;
    result.failed = snapshot.failedExecutions;
    result.started = snapshot.startedExecutions;
    result.finished = snapshot.finishedExecutions;
    result.seconds =
        std::chrono::duration<double>(endTime - startTime).count();
    result.metricsTotalMs =
        std::chrono::duration<double, std::milli>(
            snapshot.totalExecutionTime
        ).count();
    result.metricsAverageUs =
        std::chrono::duration<double, std::micro>(
            snapshot.averageExecutionTime
        ).count();
    result.metricsThroughput = snapshot.throughputJobsPerSecond;

    std::vector<std::string> problems;

    if (!submissionError.empty()) {
        problems.push_back("submission failed: " + submissionError);
    }
    if (result.accepted != jobs) {
        problems.push_back(
            "accepted " + std::to_string(result.accepted) + " of " +
            std::to_string(jobs) + " requested jobs"
        );
    }
    if (result.submitted != result.accepted) {
        problems.push_back("metrics submitted != accepted jobs");
    }
    if (result.started != result.submitted ||
        result.finished != result.started) {
        problems.push_back(
            "execution attempts differ (submitted/started/finished = " +
            std::to_string(result.submitted) + "/" +
            std::to_string(result.started) + "/" +
            std::to_string(result.finished) + ")"
        );
    }
    if (result.completed != result.accepted || result.failed != 0) {
        problems.push_back(
            std::to_string(result.failed) + " failed execution(s), " +
            std::to_string(result.completed) + " completed"
        );
    }
    if (handlerCalls.load() != result.accepted) {
        problems.push_back(
            "handler ran " + std::to_string(handlerCalls.load()) +
            " times for " + std::to_string(result.accepted) + " jobs"
        );
    }

    std::uint64_t notCompleted = 0;
    for (const auto& job : accepted) {
        if (job->getStatus() != jobq::JobStatus::Completed) {
            ++notCompleted;
        }
    }
    if (notCompleted != 0) {
        problems.push_back(
            std::to_string(notCompleted) +
            " accepted job(s) not in Completed state"
        );
    }

    if (!queue.empty()) {
        problems.push_back("queue not empty after shutdown");
    }
    if (!pool.isStopped()) {
        problems.push_back("worker pool not stopped");
    }

    std::uint64_t expectedChecksum = 0;
    for (std::uint64_t i = 1; i <= jobs; ++i) {
        expectedChecksum +=
            runWork(kinds[kindIndexFor(scenario, i)].work, i);
    }
    if (checksum.load() != expectedChecksum) {
        problems.push_back("workload checksum mismatch");
    }

    if (collectLatency) {
        result.latencyCollected = true;

        const std::uint64_t invalidSamples = collector->collect(
                result.queueWait,
                result.executionSuccess,
                result.executionFailure,
                result.endToEnd
            );

        if (invalidSamples != 0) {
            problems.push_back(
                std::to_string(invalidSamples) + " invalid latency sample(s)"
            );
        }
        if (result.queueWait.count() != result.started) {
            problems.push_back(
                "queue wait samples (" +
                std::to_string(result.queueWait.count()) +
                ") != started executions (" +
                std::to_string(result.started) + ")"
            );
        }
        if (result.executionSuccess.count() != result.completed ||
            result.executionFailure.count() != result.failed) {
            problems.push_back(
                "execution samples (" +
                std::to_string(result.executionSuccess.count()) + " ok / " +
                std::to_string(result.executionFailure.count()) +
                " failed) do not match completed/failed counts"
            );
        }
        if (result.endToEnd.count() != result.completed) {
            problems.push_back("end-to-end samples != completed jobs");
        }

        const auto okTotal = result.executionSuccess.total();
        const auto failTotal = result.executionFailure.total();

        if (!okTotal || !failTotal ||
            *okTotal + *failTotal != snapshot.totalExecutionTime) {
            problems.push_back(
                "execution samples do not sum to the JobMetrics total"
            );
        }
    }

    if (!problems.empty()) {
        for (std::size_t i = 0; i < problems.size(); ++i) {
            result.error += (i ? "; " : "") + problems[i];
        }
        return result;
    }

    if (result.seconds <= 0.0) {
        result.error = "non-positive benchmark duration";
        return result;
    }

    result.throughput =
        static_cast<double>(result.completed) / result.seconds;
    result.ok = true;
    return result;
}

void printRun(
    std::ostream& out,
    std::size_t number,
    const Config& config,
    const RunResult& run
) {
    out << "Run " << number << '\n'
        << "  Scenario:             " << scenarioName(config.scenario) << '\n'
        << "  Workers:              " << config.workers << '\n'
        << "  Requested Jobs:       " << run.requested << '\n'
        << "  Accepted Jobs:        " << run.accepted << '\n'
        << "  Submitted Jobs:       " << run.submitted << '\n'
        << "  Started Executions:   " << run.started << '\n'
        << "  Finished Executions:  " << run.finished << '\n'
        << "  Completed Jobs:       " << run.completed << '\n'
        << "  Failed Jobs:          " << run.failed << '\n'
        << std::fixed << std::setprecision(3)
        << "  Duration:             " << run.seconds << " seconds\n"
        << std::setprecision(2);

    if (run.ok) {
        out << "  Throughput:           " << run.throughput
            << " jobs/sec\n";
    } else {
        out << "  Throughput:           n/a (run unsuccessful)\n";
    }

    out << std::setprecision(3)
        << "  Metrics Exec Total:   " << run.metricsTotalMs << " ms\n"
        << "  Metrics Exec Average: " << run.metricsAverageUs << " us\n"
        << std::setprecision(2)
        << "  Metrics Lifetime Rate:" << ' ' << run.metricsThroughput
        << " jobs/sec (JobMetrics, not the benchmark result)\n";

    if (run.latencyCollected) {
        printLatencyLine(out, "Queue Wait", run.queueWait);
        printLatencyLine(out, "Exec (ok)", run.executionSuccess);
        printLatencyLine(out, "Exec (failed)", run.executionFailure);
        printLatencyLine(out, "End-to-End", run.endToEnd);
    }

    if (!run.ok) {
        out << "  ERROR: " << run.error << '\n';
    }

    out << '\n';
}

struct ConfigResult {
    std::uint64_t workers{0};
    bool ok{false};
    std::string error;

    std::size_t runsRequested{0};
    std::size_t runsSucceeded{0};

    std::vector<double> throughputs;
    std::optional<jobq_bench::RunSummary> summary;

    std::uint64_t accepted{0};
    std::uint64_t completed{0};
    std::uint64_t failed{0};
    std::uint64_t started{0};
    std::uint64_t finished{0};

    double metricsAverageUs{0.0};

    bool latencyPooled{false};
    std::size_t queueWaitSamples{0};
    std::size_t executionSamples{0};
    std::optional<jobq::LatencySummary> queueWait;
    std::optional<jobq::LatencySummary> execution;
};

std::string formatRatio(const std::optional<double>& value, const char* suffix) {
    if (!value) {
        return "n/a";
    }

    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << *value << suffix;
    return text.str();
}

std::string formatChange(
    const std::optional<jobq_bench::ThroughputChange>& change
) {
    if (!change) {
        return "n/a";
    }

    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << std::showpos
         << change->absolute << " jobs/sec";

    if (change->percent) {
        text << " (" << *change->percent << "%)";
    } else {
        text << " (percentage n/a)";
    }

    return text.str();
}

std::vector<jobq_bench::ScalingInput> toScalingInputs(
    const std::vector<ConfigResult>& results
) {
    std::vector<jobq_bench::ScalingInput> inputs;

    for (const auto& result : results) {
        jobq_bench::ScalingInput input;
        input.workers = result.workers;
        input.ok = result.ok;
        input.throughput = result.summary;
        inputs.push_back(input);
    }

    return inputs;
}

ConfigResult runWorkerCount(
    const Config& config,
    std::uint64_t workers
) {
    ConfigResult result;
    result.workers = workers;
    result.runsRequested = static_cast<std::size_t>(config.runs);

    std::cout << "Workers: " << workers << "\n";

    if (config.warmupJobs > 0) {
        const auto warmup =
            runOnce(config.scenario, config.warmupJobs, workers, config.latency);

        if (!warmup.ok) {
            result.error = "warmup failed: " + warmup.error;
            std::cout << "  Warmup FAILED: " << warmup.error << "\n\n";
            return result;
        }
    }

    const bool pool =
        config.latency && config.jobs * config.runs <= kMaxPooledSamples;
    jobq::LatencySamples pooledWait;
    jobq::LatencySamples pooledExecution;
    std::vector<double> metricsAverages;

    for (std::uint64_t run = 1; run <= config.runs; ++run) {
        const auto outcome =
            runOnce(config.scenario, config.jobs, workers, config.latency);

        if (!outcome.ok) {
            result.error = "run " + std::to_string(run) + " failed: " +
                           outcome.error;
            std::cout << "  Run " << run << ": FAILED - " << outcome.error
                      << "\n\n";
            return result;
        }

        std::cout << "  Run " << run << ": accepted " << outcome.accepted
                  << ", completed " << outcome.completed << ", failed "
                  << outcome.failed << ", started " << outcome.started
                  << ", finished " << outcome.finished << std::fixed
                  << std::setprecision(3) << ", duration " << outcome.seconds
                  << " s" << std::setprecision(2) << ", throughput "
                  << outcome.throughput << " jobs/sec\n";

        result.throughputs.push_back(outcome.throughput);
        metricsAverages.push_back(outcome.metricsAverageUs);
        ++result.runsSucceeded;

        result.accepted = outcome.accepted;
        result.completed = outcome.completed;
        result.failed = outcome.failed;
        result.started = outcome.started;
        result.finished = outcome.finished;

        if (pool) {
            pooledWait.merge(outcome.queueWait);
            pooledExecution.merge(outcome.executionSuccess);
            pooledExecution.merge(outcome.executionFailure);
        }
    }

    result.summary = jobq_bench::summarizeThroughputs(result.throughputs);

    if (!result.summary) {
        result.error = "throughput samples are not valid";
        return result;
    }

    result.metricsAverageUs =
        std::accumulate(metricsAverages.begin(), metricsAverages.end(), 0.0) /
        static_cast<double>(metricsAverages.size());

    if (pool) {
        result.latencyPooled = true;
        result.queueWaitSamples = pooledWait.count();
        result.executionSamples = pooledExecution.count();
        result.queueWait = pooledWait.summarize();
        result.execution = pooledExecution.summarize();
    }

    result.ok = true;
    return result;
}

void printConfigSummary(
    const ConfigResult& result,
    const jobq_bench::ScalingRow& row,
    bool baselineAvailable,
    const Config& config
) {
    std::cout << std::fixed << std::setprecision(2);

    if (!result.ok) {
        std::cout << "  Configuration FAILED: " << result.error << "\n\n";
        return;
    }

    std::cout << "  Runs Succeeded:       " << result.runsSucceeded << " of "
              << result.runsRequested << '\n'
              << "  Completed Jobs:       " << result.completed
              << " per run (failed " << result.failed << ")\n"
              << "  Min Throughput:       " << result.summary->minimum
              << " jobs/sec\n"
              << "  Max Throughput:       " << result.summary->maximum
              << " jobs/sec\n"
              << "  Mean Throughput:      " << result.summary->mean
              << " jobs/sec\n"
              << "  Median Throughput:    " << result.summary->median
              << " jobs/sec\n"
              << "  Avg Exec (JobMetrics):" << ' '
              << formatNanoseconds(result.metricsAverageUs * 1000.0) << '\n';

    if (config.latency && result.latencyPooled) {
        const auto line = [](const char* label,
                             const std::optional<jobq::LatencySummary>& value,
                             std::size_t samples) {
            std::cout << "  " << std::left << std::setw(22) << label
                      << std::right << "n=" << samples;

            if (!value) {
                std::cout << "  (no samples)\n";
                return;
            }

            std::cout << "  avg " << formatNanoseconds(value->averageNanoseconds)
                      << "  p50 " << formatDuration(value->p50)
                      << "  p95 " << formatDuration(value->p95)
                      << "  p99 " << formatDuration(value->p99) << '\n';
        };

        line("Queue Wait:", result.queueWait, result.queueWaitSamples);
        line("Execution Duration:", result.execution, result.executionSamples);
    }

    if (baselineAvailable) {
        std::cout << "  Speedup:              " << formatRatio(row.speedup, "x")
                  << '\n'
                  << "  Scaling Efficiency:   "
                  << formatRatio(row.efficiencyPercent, "%") << '\n'
                  << "  Throughput Change:    " << formatChange(row.change)
                  << '\n';
    } else {
        std::cout << "  Speedup:              n/a (no 1-worker baseline)\n"
                  << "  Scaling Efficiency:   n/a (no 1-worker baseline)\n";
    }

    std::cout << '\n';
}

bool runScalingBenchmark(const Config& config) {
    std::cout << "========== JobQ Worker Scaling Benchmark ==========\n\n"
              << "Workload Configuration\n"
              << "  Scenario:             " << scenarioName(config.scenario)
              << '\n'
              << "  Requested Jobs:       " << config.jobs << '\n'
              << "  Worker Counts:        ";

    for (std::size_t i = 0; i < config.workerCounts.size(); ++i) {
        std::cout << (i ? "," : "") << config.workerCounts[i];
    }

    std::cout << '\n'
              << "  Runs Per Count:       " << config.runs << '\n'
              << "  Warmup Jobs:          " << config.warmupJobs << '\n'
              << "  Latency Collection:   "
              << (config.latency ? "enabled" : "disabled (--no-latency)")
              << '\n'
              << "  Per-job logging:      suppressed during runs\n"
              << "  Execution Order:      sequential, ascending worker count\n\n"
              << "Worker Scaling Results\n\n"
              << std::flush;

    std::vector<ConfigResult> results;

    for (const auto workers : config.workerCounts) {
        auto result = runWorkerCount(config, workers);
        results.push_back(std::move(result));

        const auto analysis =
            jobq_bench::analyzeScaling(toScalingInputs(results));

        printConfigSummary(
            results.back(),
            analysis.rows.back(),
            analysis.baselineAvailable,
            config
        );
    }

    const auto inputs = toScalingInputs(results);
    const auto analysis = jobq_bench::analyzeScaling(inputs);

    bool allOk = true;
    for (const auto& result : results) {
        allOk = allOk && result.ok;
    }

    std::cout << "Scaling Comparison (speedup uses median throughput; "
                 "baseline = 1 worker)\n\n"
              << std::left << std::setw(9) << "Workers" << std::right
              << std::setw(15) << "Median jobs/s" << std::setw(15)
              << "Mean jobs/s" << std::setw(10) << "Speedup"
              << std::setw(13) << "Efficiency" << "  Change vs 1 worker\n";

    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        const auto& row = analysis.rows[i];

        std::cout << std::left << std::setw(9) << result.workers << std::right;

        if (!result.ok) {
            std::cout << "  FAILED (" << result.error << ")\n";
            continue;
        }

        std::cout << std::fixed << std::setprecision(2) << std::setw(15)
                  << result.summary->median << std::setw(15)
                  << result.summary->mean << std::setw(10)
                  << formatRatio(row.speedup, "x") << std::setw(13)
                  << formatRatio(row.efficiencyPercent, "%") << "  "
                  << (analysis.baselineAvailable ? formatChange(row.change)
                                                 : std::string("n/a"))
                  << '\n';
    }

    std::cout << '\n';

    if (config.latency) {
        std::cout << "Latency Comparison (pooled over successful runs; "
                     "nearest-rank percentiles)\n\n"
                  << std::left << std::setw(9) << "Workers" << std::right
                  << std::setw(40) << "Queue wait avg / p50 / p95 / p99"
                  << std::setw(44) << "Execution avg / p50 / p95 / p99"
                  << '\n';

        const auto cell = [](const std::optional<jobq::LatencySummary>& v) {
            if (!v) {
                return std::string("n/a");
            }
            return formatNanoseconds(v->averageNanoseconds) + " / " +
                   formatDuration(v->p50) + " / " + formatDuration(v->p95) +
                   " / " + formatDuration(v->p99);
        };

        for (const auto& result : results) {
            std::cout << std::left << std::setw(9) << result.workers
                      << std::right;

            if (!result.ok || !result.latencyPooled) {
                std::cout << "  n/a\n";
                continue;
            }

            std::cout << std::setw(40) << cell(result.queueWait)
                      << std::setw(44) << cell(result.execution) << '\n';
        }

        std::cout << '\n';
    }

    std::cout << "Summary\n\n";

    if (!analysis.baselineAvailable) {
        std::cout << "  Baseline: " << analysis.baselineNote << ".\n";
    }

    if (analysis.bestMedianIndex) {
        std::cout << "  Best median throughput:      "
                  << results[*analysis.bestMedianIndex].workers
                  << " worker(s) at " << std::fixed << std::setprecision(2)
                  << results[*analysis.bestMedianIndex].summary->median
                  << " jobs/sec\n";
    }

    if (analysis.bestMaximumIndex) {
        std::cout << "  Best single-run throughput:  "
                  << results[*analysis.bestMaximumIndex].workers
                  << " worker(s) at " << std::fixed << std::setprecision(2)
                  << results[*analysis.bestMaximumIndex].summary->maximum
                  << " jobs/sec\n";
    }

    if (analysis.baselineAvailable) {
        std::cout << "  Additional workers improved median throughput over 1 "
                     "worker: "
                  << (analysis.additionalWorkersImproved ? "yes" : "no")
                  << '\n';
    }

    if (analysis.decreasedAt.empty()) {
        std::cout << "  Throughput decreases between successive counts: none\n";
    } else {
        for (const auto index : analysis.decreasedAt) {
            std::cout << "  Throughput decreased at " << results[index].workers
                      << " worker(s) compared with the previous successful "
                         "configuration\n";
        }
    }

    if (analysis.baselineAvailable) {
        if (analysis.efficiencyDeclinedAt.empty()) {
            std::cout << "  Scaling efficiency did not decline between "
                         "successive counts\n";
        } else {
            std::cout << "  Scaling efficiency declined (diminishing returns) "
                         "at:";
            for (const auto index : analysis.efficiencyDeclinedAt) {
                std::cout << ' ' << results[index].workers;
            }
            std::cout << " worker(s)\n";
        }
    }

    for (const auto& result : results) {
        if (!result.ok) {
            std::cout << "  FAILED configuration: " << result.workers
                      << " worker(s) - " << result.error << '\n';
        }
    }

    std::cout << "\n  Benchmark Status: " << (allOk ? "PASS" : "FAIL")
              << "\n\n====================================================\n";

    return allOk;
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

    if (config.scalingMode) {
        return runScalingBenchmark(config) ? 0 : 1;
    }

    std::cout << "========== JobQ Throughput Benchmark ==========\n\n"
              << "Configuration\n"
              << "  Scenario:             " << scenarioName(config.scenario)
              << '\n'
              << "  Requested Jobs:       " << config.jobs << '\n'
              << "  Workers:              " << config.workers << '\n'
              << "  Repetitions:          " << config.runs << '\n'
              << "  Warmup Jobs:          " << config.warmupJobs << '\n'
              << "  Latency Collection:   "
              << (config.latency ? "enabled" : "disabled (--no-latency)")
              << '\n'
              << "  Per-job logging:      suppressed during runs\n\n"
              << std::flush;

    bool allOk = true;

    if (config.warmupJobs > 0) {
        const auto warmup =
            runOnce(
                config.scenario,
                config.warmupJobs,
                config.workers,
                config.latency
            );

        if (!warmup.ok) {
            std::cout << "Warmup run FAILED: " << warmup.error << "\n\n";
            allOk = false;
        } else {
            std::cout << "Warmup completed (" << warmup.completed
                      << " jobs, not included in results)\n\n";
        }
    }

    std::vector<double> throughputs;

    const bool poolLatency =
        config.latency && config.jobs * config.runs <= kMaxPooledSamples;
    jobq::LatencySamples pooledWait;
    jobq::LatencySamples pooledOk;
    jobq::LatencySamples pooledFailed;
    jobq::LatencySamples pooledEndToEnd;
    std::size_t pooledRuns = 0;

    std::cout << "Run Results\n\n";

    for (std::uint64_t run = 1; run <= config.runs && allOk; ++run) {
        const auto result =
            runOnce(
                config.scenario,
                config.jobs,
                config.workers,
                config.latency
            );

        printRun(std::cout, run, config, result);

        if (result.ok) {
            throughputs.push_back(result.throughput);

            if (poolLatency) {
                pooledWait.merge(result.queueWait);
                pooledOk.merge(result.executionSuccess);
                pooledFailed.merge(result.executionFailure);
                pooledEndToEnd.merge(result.endToEnd);
                ++pooledRuns;
            }
        } else {
            allOk = false;
        }
    }

    std::cout << "Performance Summary\n\n";

    const auto throughputSummary =
        jobq_bench::summarizeThroughputs(throughputs);

    if (throughputSummary) {
        std::cout << std::fixed << std::setprecision(2)
                  << "  Successful Runs:      " << throughputSummary->count
                  << " of " << config.runs << '\n'
                  << "  Minimum Throughput:   " << throughputSummary->minimum
                  << " jobs/sec\n"
                  << "  Maximum Throughput:   " << throughputSummary->maximum
                  << " jobs/sec\n"
                  << "  Mean Throughput:      " << throughputSummary->mean
                  << " jobs/sec\n"
                  << "  Median Throughput:    " << throughputSummary->median
                  << " jobs/sec\n\n";
    } else {
        std::cout << "  No successful runs; no throughput reported.\n\n";
    }

    if (config.latency && pooledRuns > 0) {
        std::cout << "========== Latency Statistics ==========\n\n"
                  << "Pooled over " << pooledRuns << " successful measured run(s); "
                  << "per-attempt samples, nearest-rank percentiles.\n\n";
        printSummaryBlock(std::cout, "Queue Waiting Time", pooledWait);
        printSummaryBlock(
            std::cout, "Execution Duration (successful attempts)", pooledOk
        );
        printSummaryBlock(
            std::cout, "Execution Duration (failed attempts)", pooledFailed
        );
        printSummaryBlock(
            std::cout, "End-to-End Processing Time (per job)", pooledEndToEnd
        );
        std::cout << "========================================\n\n";
    } else if (config.latency) {
        std::cout << "Latency Statistics: pooled section omitted (more than "
                  << kMaxPooledSamples
                  << " samples would be retained); see per-run lines above.\n\n";
    }

    std::cout << "  Benchmark Status: " << (allOk ? "PASS" : "FAIL")
              << "\n\n================================================\n";

    return allOk ? 0 : 1;
}
