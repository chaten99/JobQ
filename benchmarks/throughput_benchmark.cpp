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

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/ExecutionObserver.h"
#include "metrics/JobMetrics.h"
#include "metrics/LatencyStatistics.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::uint64_t kMaxJobs = 2'000'000;
constexpr std::uint64_t kMaxWorkers = 256;
constexpr std::uint64_t kMaxRuns = 100;
constexpr std::uint64_t kMaxPooledSamples = 5000000;

enum class Scenario { Noop, Lightweight, Mixed };

struct Config {
    std::uint64_t jobs{10000};
    std::uint64_t workers{4};
    Scenario scenario{Scenario::Noop};
    std::uint64_t runs{3};
    std::uint64_t warmupJobs{1000};
    bool latency{true};
};

struct ParseOutcome {
    bool ok{true};
    bool help{false};
    std::string error;
};

const char* scenarioName(Scenario scenario) {
    switch (scenario) {
        case Scenario::Noop: return "noop";
        case Scenario::Lightweight: return "lightweight";
        case Scenario::Mixed: return "mixed";
    }
    return "unknown";
}

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
           "--scenario noop --runs 5 --warmup-jobs 1000\n";
}

bool parseUnsigned(
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
            name == "--warmup-jobs";

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
            parsed = parseUnsigned(
                value, 1, kMaxJobs, name, config.jobs, outcome.error
            );
        } else if (name == "--workers") {
            parsed = parseUnsigned(
                value, 1, kMaxWorkers, name, config.workers, outcome.error
            );
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

    return outcome;
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

std::uint64_t spin(std::uint64_t seed, int iterations) {
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;

    for (int i = 0; i < iterations; ++i) {
        x ^= x >> 13;
        x *= 0xFF51AFD7ED558CCDULL;
        x ^= x << 7;
    }

    return x;
}

std::uint64_t fnv(std::uint64_t seed) {
    std::uint64_t hash = 0xCBF29CE484222325ULL ^ seed;

    for (int i = 0; i < 256; ++i) {
        hash ^= static_cast<std::uint64_t>((i * 31 + seed) & 0xFF);
        hash *= 0x100000001B3ULL;
    }

    return hash;
}

std::uint64_t runWork(Work work, std::uint64_t id) {
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

std::vector<Kind> kindsFor(Scenario scenario) {
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

std::size_t kindIndexFor(Scenario scenario, std::uint64_t id) {
    if (scenario != Scenario::Mixed) {
        return 0;
    }

    static constexpr std::size_t pattern[10] = {0, 0, 0, 0, 0, 1, 1, 1, 2, 3};
    return pattern[id % 10];
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

class LatencyCollector final : public jobq::ExecutionObserver {
public:
    LatencyCollector(
        std::size_t workers,
        std::size_t expectedJobs,
        const std::vector<SteadyClock::time_point>& submitStarts
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

            if (sample.jobId == 0 || sample.jobId >= submitStarts_.size()) {
                ++slot.invalid;
                return;
            }

            const auto total =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sample.finishedAt - submitStarts_[sample.jobId]
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

    std::uint64_t collect(RunResult& result) {
        std::uint64_t invalid = invalid_.load();

        for (auto& slot : slots_) {
            result.queueWait.merge(slot.queueWait);
            result.executionSuccess.merge(slot.executionSuccess);
            result.executionFailure.merge(slot.executionFailure);
            result.endToEnd.merge(slot.endToEnd);
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
    const std::vector<SteadyClock::time_point>& submitStarts_;
    std::atomic<std::uint64_t> invalid_{0};
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
            submitStarts
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

        const std::uint64_t invalidSamples = collector->collect(result);

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

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t count = values.size();

    if (count % 2 == 1) {
        return values[count / 2];
    }

    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

std::string formatNanoseconds(double nanoseconds) {
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

std::string formatDuration(std::chrono::nanoseconds duration) {
    return formatNanoseconds(static_cast<double>(duration.count()));
}

void printLatencyLine(
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

void printSummaryBlock(
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

    if (!throughputs.empty()) {
        const double minimum =
            *std::min_element(throughputs.begin(), throughputs.end());
        const double maximum =
            *std::max_element(throughputs.begin(), throughputs.end());
        const double mean =
            std::accumulate(throughputs.begin(), throughputs.end(), 0.0) /
            static_cast<double>(throughputs.size());

        std::cout << std::fixed << std::setprecision(2)
                  << "  Successful Runs:      " << throughputs.size()
                  << " of " << config.runs << '\n'
                  << "  Minimum Throughput:   " << minimum << " jobs/sec\n"
                  << "  Maximum Throughput:   " << maximum << " jobs/sec\n"
                  << "  Mean Throughput:      " << mean << " jobs/sec\n"
                  << "  Median Throughput:    " << median(throughputs)
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
