#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/JobMetrics.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"
#include "storage/JobRecord.h"
#include "worker/WorkerPool.h"

#include "helpers/FaultInjectingJobStorage.h"
#include "helpers/TemporaryDirectory.h"
#include "helpers/TestSupport.h"

namespace {

using namespace jobq_test;
using jobq::JobPriority;
using jobq::JobRecord;
using jobq::JobStatus;
using jobq::RetryPolicy;

RetryPolicy retries(std::size_t count) {
    RetryPolicy policy;
    policy.maxRetries = count;
    policy.initialDelay = 1ms;
    policy.multiplier = 2.0;
    policy.maxDelay = 1ms;
    return policy;
}

struct Rig {
    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobMetrics metrics;
    std::shared_ptr<FaultInjectingJobStorage> storage =
        std::make_shared<FaultInjectingJobStorage>();
    std::unique_ptr<jobq::JobManager> manager;
    std::unique_ptr<jobq::WorkerPool> pool;

    Rig() {
        manager = std::make_unique<jobq::JobManager>(
            queue, storage, metrics
        );
    }

    void startPool(std::size_t workers) {
        pool = std::make_unique<jobq::WorkerPool>(
            queue, executor, workers, metrics
        );
        pool->start();
    }
};

bool stopsWithinTimeout(jobq::WorkerPool& pool) {
    auto stopped = std::async(std::launch::async, [&] { pool.stop(); });
    return stopped.wait_for(kTimeout) == std::future_status::ready;
}

std::string mutateField(
    const std::string& serialized,
    std::size_t index,
    const std::string& value
) {
    std::vector<std::string> fields;
    std::istringstream stream(serialized);
    std::string field;
    while (std::getline(stream, field, '|')) {
        fields.push_back(field);
    }
    fields.at(index) = value;
    std::string result;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        result += (i ? "|" : "") + fields[i];
    }
    return result;
}

JobRecord makeRecord(
    std::uint64_t id,
    JobStatus status,
    std::size_t attempts,
    RetryPolicy policy = {}
) {
    JobRecord record;
    record.id = id;
    record.type = "T";
    record.payload = "p" + std::to_string(id);
    record.status = status;
    record.retryPolicy = policy;
    record.attemptsMade = attempts;
    return record;
}

void testHandlerFailures() {
    std::cout << "\n--- A: Job Handler Failures ---\n";

    Rig rig;
    std::atomic<int> okRuns{0};
    std::atomic<int> flakyRuns{0};
    std::atomic<int> alwaysRuns{0};

    rig.executor.registerHandler("OK", [&](const jobq::Job&) { okRuns++; });
    rig.executor.registerHandler("STD", [](const jobq::Job&) {
        throw std::runtime_error("standard failure");
    });
    rig.executor.registerHandler("UNKNOWN", [](const jobq::Job&) {
        throw 7;
    });
    rig.executor.registerHandler("FLAKY", [&](const jobq::Job& job) {
        flakyRuns++;
        if (job.getAttemptsMade() == 1) {
            throw std::runtime_error("first attempt fails");
        }
    });
    rig.executor.registerHandler("ALWAYS", [&](const jobq::Job&) {
        alwaysRuns++;
        throw std::runtime_error("always fails");
    });

    rig.startPool(1);

    const auto ok1 = rig.manager->submit("OK", "p");
    const auto stdFail = rig.manager->submit("STD", "p");
    const auto unknown = rig.manager->submit("UNKNOWN", "p");
    const auto flaky = rig.manager->submit(
        "FLAKY", "p", JobPriority::Normal, retries(2)
    );
    const auto always = rig.manager->submit(
        "ALWAYS", "p", JobPriority::Normal, retries(2)
    );
    const auto ok2 = rig.manager->submit("OK", "p");

    rig.pool->stop();

    check(ok1->getStatus() == JobStatus::Completed &&
              ok2->getStatus() == JobStatus::Completed,
          "Valid jobs complete around failing jobs");
    check(okRuns.load() == 2, "Each valid job ran exactly once");
    check(stdFail->getStatus() == JobStatus::Failed &&
              stdFail->getAttemptsMade() == 1,
          "Standard exception fails the job after one attempt");
    check(unknown->getStatus() == JobStatus::Failed &&
              unknown->getAttemptsMade() == 1,
          "Non-standard exception fails the job after one attempt");
    check(flaky->getStatus() == JobStatus::Completed &&
              flaky->getAttemptsMade() == 2 && flakyRuns.load() == 2,
          "Fail-then-succeed job completes on attempt 2");
    check(always->getStatus() == JobStatus::Failed &&
              always->getAttemptsMade() == 3 && alwaysRuns.load() == 3,
          "Always-failing job stops at 1 + maxRetries attempts");

    const auto snap = rig.metrics.snapshot();
    check(snap.submittedJobs == 6 && snap.startedExecutions == 9 &&
              snap.completedExecutions == 3 && snap.failedExecutions == 6 &&
              snap.scheduledRetries == 3,
          "Metrics match the exact attempt, failure and retry counts");
    check(metricsConsistent(snap), "Metrics invariants hold");
    check(rig.pool->isStopped(), "Pool shut down cleanly after failures");
}

void testStorageReadFailure() {
    std::cout << "\n--- B1: Storage Read Failure ---\n";

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    auto storage = std::make_shared<FaultInjectingJobStorage>();
    storage->seed(makeRecord(1, JobStatus::Pending, 0));
    storage->setFailLoad(true);

    check(throwsWith<std::runtime_error>(
              [&] { jobq::JobManager manager(queue, storage, metrics); },
              "injected load failure"),
          "Read failure is reported, not treated as empty storage");
    check(queue.empty() && metrics.snapshot().submittedJobs == 0,
          "Nothing queued or counted after a failed load");
}

void testStorageSaveFailureOnSubmit() {
    std::cout << "\n--- B2: Storage Save Failure During Submission ---\n";

    Rig rig;
    rig.executor.registerHandler("T", [](const jobq::Job&) {});
    rig.startPool(2);

    rig.storage->setSaveFault([](const JobRecord&) { return true; });

    check(throwsWith<std::runtime_error>(
              [&] { rig.manager->submit("T", "p"); },
              "injected save failure"),
          "Submission reports the persistence failure");
    check(rig.manager->totalJobs() == 0 && rig.manager->getJob(1) == nullptr,
          "Rolled back: no registry entry exposed");
    check(rig.queue.empty(), "Job not left queued");
    check(rig.metrics.snapshot().submittedJobs == 0,
          "Failed submission not counted");

    rig.storage->clearSaveFault();
    const auto job = rig.manager->submit("T", "p");
    rig.pool->stop();
    check(job->getStatus() == JobStatus::Completed &&
              job->getId() == 2,
          "Later submission succeeds with a fresh unique ID");
    check(rig.metrics.snapshot().submittedJobs == 1 &&
              metricsConsistent(rig.metrics.snapshot()),
          "Only the accepted submission is counted");
}

void testStorageFailureOnRunningTransition() {
    std::cout << "\n--- B3a: Save Failure On Pending -> Running ---\n";

    Rig rig;
    std::atomic<int> runs{0};
    rig.executor.registerHandler("T", [&](const jobq::Job&) { runs++; });
    rig.startPool(1);

    rig.storage->setSaveFault([](const JobRecord& record) {
        return record.id == 1 && record.status == JobStatus::Running;
    });

    const auto stranded = rig.manager->submit("T", "p");
    const auto healthy = rig.manager->submit("T", "p");
    rig.pool->stop();

    check(healthy->getStatus() == JobStatus::Completed,
          "Worker survives the failure and runs the next job");
    check(runs.load() == 1, "Handler never ran for the unsaved transition");
    check(stranded->getStatus() == JobStatus::Pending &&
              stranded->getAttemptsMade() == 0,
          "Job state not advanced when persistence failed");
    check(rig.queue.empty(),
          "Limitation: stranded job is not requeued within this process");
    check(rig.storage->latest(1)->status == JobStatus::Pending,
          "Durable state honestly stays Pending");

    const auto snap = rig.metrics.snapshot();
    check(snap.startedExecutions == 1 && metricsConsistent(snap),
          "No execution recorded for the failed transition");

    rig.storage->clearSaveFault();
    jobq::JobQueue queue2;
    jobq::JobMetrics metrics2;
    jobq::JobManager recovered(queue2, rig.storage, metrics2);
    check(recovered.getStatus(1) == JobStatus::Pending && queue2.size() == 1,
          "Restart recovers the stranded job from durable state");
    check(recovered.getStatus(2) == JobStatus::Completed,
          "Restart does not requeue the completed job");
}

void testStorageFailureOnCompletion() {
    std::cout << "\n--- B3b/c/d: Save Failure On Completion ---\n";

    {
        Rig rig;
        std::atomic<int> runs{0};
        rig.executor.registerHandler("T", [&](const jobq::Job&) { runs++; });
        rig.startPool(1);
        rig.storage->setSaveFault(
            [left = 1](const JobRecord& record) mutable {
                return record.status == JobStatus::Completed && left-- > 0;
            }
        );

        const auto job =
            rig.manager->submit("T", "p", JobPriority::Normal, retries(1));
        rig.pool->stop();

        check(runs.load() == 2 && job->getStatus() == JobStatus::Completed &&
                  job->getAttemptsMade() == 2,
              "Transient completion-save failure causes a retry "
              "(at-least-once execution)");
        const auto snap = rig.metrics.snapshot();
        check(snap.startedExecutions == 2 && snap.completedExecutions == 1 &&
                  snap.failedExecutions == 1 && snap.scheduledRetries == 1,
              "Metrics reflect the extra attempt and its retry");
        check(metricsConsistent(snap), "Metrics invariants hold");
        check(rig.storage->latest(1)->status == JobStatus::Completed,
              "Durable state ends Completed");
    }

    {
        Rig rig;
        std::atomic<int> runs{0};
        rig.executor.registerHandler("T", [&](const jobq::Job&) { runs++; });
        rig.startPool(1);
        rig.storage->setSaveFault([](const JobRecord& record) {
            return record.status == JobStatus::Completed;
        });

        const auto job =
            rig.manager->submit("T", "p", JobPriority::Normal, retries(1));
        rig.pool->stop();

        check(runs.load() == 2 && job->getStatus() == JobStatus::Failed &&
                  job->getAttemptsMade() == 2,
              "Persistent completion-save failure exhausts retries as Failed");
        check(rig.storage->latest(1)->status == JobStatus::Failed,
              "Durable state matches the in-memory Failed state");
        check(metricsConsistent(rig.metrics.snapshot()) &&
                  rig.metrics.snapshot().completedExecutions == 0,
              "No completion is recorded when it was never persisted");
    }

    {
        Rig rig;
        std::atomic<int> runs{0};
        rig.executor.registerHandler("T", [&](const jobq::Job&) { runs++; });
        rig.startPool(1);
        rig.storage->setSaveFault([](const JobRecord& record) {
            return record.status == JobStatus::Completed ||
                   record.status == JobStatus::Failed;
        });

        const auto job = rig.manager->submit("T", "p");
        const auto next = rig.manager->submit("T", "p");
        rig.pool->stop();

        check(runs.load() == 2 && rig.pool->isStopped(),
              "Workers keep processing and shut down when no outcome can "
              "be saved");
        check(job->getStatus() == JobStatus::Running,
              "Limitation: job stays Running in memory when its outcome "
              "cannot be persisted");
        check(rig.storage->latest(1)->status == JobStatus::Running,
              "Durable state is not falsely marked Completed");
        check(metricsConsistent(rig.metrics.snapshot()),
              "Metrics invariants hold with unsaved outcomes");
        (void)next;

        rig.storage->clearSaveFault();
        jobq::JobQueue queue2;
        jobq::JobManager recovered(queue2, rig.storage);
        check(recovered.getStatus(1) == JobStatus::Failed &&
                  recovered.getJob(1)->getAttemptsMade() == 1,
              "Restart treats the unsaved job as interrupted");
        check(queue2.empty(), "Exhausted interrupted jobs are not requeued");
    }
}

void testRetrySchedulingException() {
    std::cout << "\n--- D: Retry Scheduling Exception ---\n";

    Rig rig;
    std::atomic<int> runs{0};
    rig.executor.registerHandler("T", [&](const jobq::Job&) {
        runs++;
        throw std::runtime_error("fails");
    });
    rig.startPool(1);
    rig.storage->setSaveFault([](const JobRecord& record) {
        return record.status == JobStatus::Pending &&
               record.attemptsMade > 0;
    });

    const auto job =
        rig.manager->submit("T", "p", JobPriority::Normal, retries(2));
    rig.pool->stop();

    check(runs.load() == 1, "No second attempt when the retry could not be saved");
    check(job->getStatus() == JobStatus::Failed &&
              job->getAttemptsMade() == 1,
          "Job stays in a valid Failed state");
    const auto snap = rig.metrics.snapshot();
    check(snap.scheduledRetries == 0 && snap.failedExecutions == 1,
          "Unscheduled retry is not counted");
    check(metricsConsistent(snap), "Metrics invariants hold");
    check(rig.storage->latest(1)->status == JobStatus::Failed,
          "Durable state is Failed");

    rig.storage->clearSaveFault();
    jobq::JobQueue queue2;
    jobq::JobMetrics metrics2;
    jobq::JobManager recovered(queue2, rig.storage, metrics2);
    check(recovered.getStatus(1) == JobStatus::Pending && queue2.size() == 1 &&
              metrics2.snapshot().scheduledRetries == 1,
          "Restart schedules the retry that could not be saved earlier");
}

void testStorageFailureDuringRecovery() {
    std::cout << "\n--- B4: Storage Failure During Recovery ---\n";

    auto storage = std::make_shared<FaultInjectingJobStorage>();
    storage->seed(makeRecord(1, JobStatus::Running, 1, retries(2)));
    storage->setSaveFault([](const JobRecord&) { return true; });

    jobq::JobQueue queue;
    jobq::JobMetrics metrics;
    check(throwsWith<std::runtime_error>(
              [&] { jobq::JobManager manager(queue, storage, metrics); },
              "injected save failure"),
          "Failure while persisting the interrupted-job transition is reported");
    check(queue.empty() && metrics.snapshot().scheduledRetries == 0,
          "Nothing queued or counted by the failed recovery");
    check(storage->latest(1)->status == JobStatus::Running,
          "Durable record untouched by the failed recovery");

    storage->clearSaveFault();
    jobq::JobQueue queue2;
    jobq::JobManager manager(queue2, storage);
    check(manager.getStatus(1) == JobStatus::Pending && queue2.size() == 1,
          "Recovery succeeds once storage works again");

    auto closedStorage = std::make_shared<FaultInjectingJobStorage>();
    closedStorage->seed(makeRecord(1, JobStatus::Pending, 0));
    jobq::JobQueue closed;
    closed.close();
    check(throwsWith<std::runtime_error>(
              [&] { jobq::JobManager m(closed, closedStorage); },
              "Failed to enqueue recovered job 1"),
          "Queue rejection during recovery is reported with the job ID");
    check(closed.empty() &&
              closedStorage->latest(1)->status == JobStatus::Pending,
          "Rejected recovered job is neither queued nor lost durably");
}

void testSubmitToClosedQueue() {
    std::cout << "\n--- C1: Submission To A Closed Queue ---\n";

    Rig rig;
    rig.queue.close();

    check(throwsWith<std::runtime_error>(
              [&] { rig.manager->submit("T", "p"); }, "queue is closed"),
          "Closed queue rejects the submission");
    check(rig.manager->totalJobs() == 0 && rig.queue.empty(),
          "No registry entry or queue entry for a rejected job");
    check(rig.metrics.snapshot().submittedJobs == 0,
          "Rejected submission is not counted");
    check(rig.storage->latest(1)->status == JobStatus::Cancelled,
          "Durable record is Cancelled, not Pending");

    jobq::JobQueue queue2;
    jobq::JobManager recovered(queue2, rig.storage);
    check(recovered.getStatus(1) == JobStatus::Cancelled && queue2.empty(),
          "Restart does not resurrect the rejected job");
}

void testCloseWhileProducersSubmit() {
    std::cout << "\n--- C2: Queue Closure While Producers Submit ---\n";

    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 100;
    constexpr std::size_t kTotal = kProducers * kPerProducer;

    for (int round = 0; round < 5; ++round) {
        Rig rig;
        std::atomic<int> runs{0};
        rig.executor.registerHandler("T", [&](const jobq::Job&) { runs++; });

        Counter progress;
        std::atomic<std::size_t> accepted{0};
        std::atomic<std::size_t> rejected{0};
        std::vector<std::thread> producers;

        for (std::size_t p = 0; p < kProducers; ++p) {
            producers.emplace_back([&] {
                for (std::size_t i = 0; i < kPerProducer; ++i) {
                    try {
                        rig.manager->submit("T", "p");
                        accepted++;
                    } catch (const std::runtime_error&) {
                        rejected++;
                    }
                    progress.add();
                }
            });
        }

        std::thread closer([&] {
            progress.waitAtLeast(40);
            rig.queue.close();
        });

        for (auto& t : producers) {
            t.join();
        }
        closer.join();

        std::size_t durablePending = 0;
        std::size_t durableCancelled = 0;
        for (std::uint64_t id = 1; id <= kTotal; ++id) {
            const auto record = rig.storage->latest(id);
            if (record && record->status == JobStatus::Pending) {
                ++durablePending;
            } else if (record && record->status == JobStatus::Cancelled) {
                ++durableCancelled;
            }
        }

        const bool accounted =
            accepted.load() + rejected.load() == kTotal &&
            rig.manager->totalJobs() == accepted.load() &&
            rig.queue.size() == accepted.load() &&
            rig.metrics.snapshot().submittedJobs == accepted.load() &&
            durablePending == accepted.load() &&
            durableCancelled == rejected.load();

        rig.startPool(3);
        rig.pool->stop();

        const bool drained =
            static_cast<std::size_t>(runs.load()) == accepted.load() &&
            rig.queue.empty() &&
            metricsConsistent(rig.metrics.snapshot());

        if (!accounted || !drained) {
            check(false, "Producer/closure round was inconsistent");
            return;
        }
    }

    check(true,
          "5 rounds: accepted + rejected = attempted; registry, queue, "
          "metrics and durable state agree; accepted jobs all executed once");
}

void testClosureWakesIdleWorkers() {
    std::cout << "\n--- C3: Closure While Consumers Wait ---\n";

    Rig rig;
    rig.startPool(4);
    check(stopsWithinTimeout(*rig.pool),
          "Idle workers wake and exit when the pool stops");
    check(rig.pool->isStopped() && rig.queue.isClosed(),
          "Pool stopped and queue closed");
}

void testShutdownWithRetryAndLateSubmit() {
    std::cout << "\n--- C4: Retry And Submission During Shutdown ---\n";

    Rig rig;
    Gate release;
    Counter entered;
    std::atomic<int> runs{0};

    rig.executor.registerHandler("T", [&](const jobq::Job& job) {
        runs++;
        if (job.getAttemptsMade() == 1) {
            entered.add();
            release.wait();
            throw std::runtime_error("fails during shutdown");
        }
    });
    rig.startPool(1);

    const auto job =
        rig.manager->submit("T", "p", JobPriority::Normal, retries(1));
    check(entered.waitAtLeast(1), "Handler is active");

    auto stopper = std::async(std::launch::async, [&] { rig.pool->stop(); });
    check(waitUntil([&] { return rig.queue.isClosed(); }),
          "Shutdown has closed the queue");

    check(throwsWith<std::runtime_error>(
              [&] { rig.manager->submit("T", "late"); }, "queue is closed"),
          "Submission during shutdown is rejected");

    release.open();
    check(stopper.wait_for(kTimeout) == std::future_status::ready,
          "Shutdown completes after the active handler finishes");
    stopper.get();

    check(job->getStatus() == JobStatus::Completed &&
              job->getAttemptsMade() == 2 && runs.load() == 2,
          "Retry requested during shutdown is accepted and executed once");
    const auto snap = rig.metrics.snapshot();
    check(snap.submittedJobs == 1 && snap.scheduledRetries == 1 &&
              snap.failedExecutions == 1 && snap.completedExecutions == 1,
          "Rejected late submission not counted; retry counted once");
    check(metricsConsistent(snap), "Metrics invariants hold");
    check(rig.queue.empty(), "Queue fully drained");
}

void testConcurrentRetries() {
    std::cout << "\n--- D5: Concurrent Failures Requesting Retries ---\n";

    Rig rig;
    std::atomic<int> runs{0};
    rig.executor.registerHandler("T", [&](const jobq::Job& job) {
        runs++;
        if (job.getAttemptsMade() < 3) {
            throw std::runtime_error("transient");
        }
    });
    rig.startPool(6);

    std::vector<std::shared_ptr<jobq::Job>> jobs;
    for (int i = 0; i < 60; ++i) {
        jobs.push_back(
            rig.manager->submit("T", "p", JobPriority::Normal, retries(2))
        );
    }
    rig.pool->stop();

    bool allDone = true;
    for (const auto& job : jobs) {
        allDone = allDone && job->getStatus() == JobStatus::Completed &&
                  job->getAttemptsMade() == 3;
    }
    const auto snap = rig.metrics.snapshot();
    check(allDone && runs.load() == 180,
          "Every job completed on attempt 3, no duplicate executions");
    check(snap.scheduledRetries == 120 && snap.failedExecutions == 120 &&
              snap.completedExecutions == 60 && snap.submittedJobs == 60,
          "Retry and execution counters match exactly");
    check(metricsConsistent(snap), "Metrics invariants hold");
}

void testCorruptedJournals() {
    std::cout << "\n--- E: Persistence Corruption ---\n";

    const std::string valid = jobq::serializeJobRecord(
        makeRecord(1, JobStatus::Pending, 0, retries(2))
    );
    const std::string second = jobq::serializeJobRecord(
        makeRecord(2, JobStatus::Pending, 0)
    );

    struct Case {
        std::string name;
        std::string badLine;
    };

    const std::vector<Case> cases = {
        {"malformed record", "this is not a record"},
        {"truncated record with newline", second.substr(0, second.size() / 2)},
        {"empty line", ""},
        {"invalid job ID", mutateField(second, 1, "0")},
        {"invalid status", mutateField(second, 2, "EXPLODED")},
        {"invalid priority", mutateField(second, 3, "URGENT")},
        {"invalid retry configuration", mutateField(second, 7, "0.25")},
        {"attempts beyond retry policy", mutateField(second, 4, "5")},
    };

    for (const auto& testCase : cases) {
        TemporaryDirectory dir("corrupt");
        {
            std::ofstream out(dir.journal(), std::ios::binary);
            out << valid << '\n' << testCase.badLine << '\n';
        }
        std::ifstream before(dir.journal(), std::ios::binary);
        const std::string original(
            (std::istreambuf_iterator<char>(before)),
            std::istreambuf_iterator<char>()
        );
        before.close();

        jobq::JobQueue queue;
        const bool rejected = throwsWith<std::runtime_error>(
            [&] {
                jobq::JobManager manager(
                    queue,
                    std::make_shared<jobq::FileJobStorage>(dir.journal())
                );
            },
            "line 2"
        );

        std::ifstream after(dir.journal(), std::ios::binary);
        const std::string current(
            (std::istreambuf_iterator<char>(after)),
            std::istreambuf_iterator<char>()
        );

        check(rejected, testCase.name + ": recovery fails and names the line");
        check(queue.empty(),
              testCase.name + ": earlier valid record is not scheduled");
        check(current == original,
              testCase.name + ": failed recovery leaves the journal intact");
    }

    {
        TemporaryDirectory dir("empty");
        std::ofstream(dir.journal(), std::ios::binary).close();
        jobq::JobQueue queue;
        jobq::JobManager manager(
            queue, std::make_shared<jobq::FileJobStorage>(dir.journal())
        );
        check(manager.totalJobs() == 0 && queue.empty(),
              "Empty journal is a valid empty store");
    }

    {
        TemporaryDirectory dir("eof");
        {
            std::ofstream out(dir.journal(), std::ios::binary);
            out << valid << '\n' << second.substr(0, second.size() - 5);
        }
        jobq::JobQueue queue;
        jobq::JobManager manager(
            queue, std::make_shared<jobq::FileJobStorage>(dir.journal())
        );
        check(manager.totalJobs() == 1 && queue.size() == 1,
              "Unexpected end of file drops only the unterminated record");
    }

    {
        TemporaryDirectory dir("dupes");
        {
            std::ofstream out(dir.journal(), std::ios::binary);
            out << valid << '\n' << valid << '\n';
        }
        jobq::JobQueue queue;
        jobq::JobExecutor executor;
        std::atomic<int> runs{0};
        executor.registerHandler("T", [&](const jobq::Job&) { runs++; });
        {
            jobq::JobManager manager(
                queue, std::make_shared<jobq::FileJobStorage>(dir.journal())
            );
            check(manager.totalJobs() == 1 && queue.size() == 1,
                  "Duplicate journal entries collapse to one job");
            jobq::WorkerPool pool(queue, executor, 2);
            pool.start();
            pool.stop();
        }
        check(runs.load() == 1, "Duplicated record executes exactly once");
    }
}

void testWorkerLifecycleFailures() {
    std::cout << "\n--- F: Worker Lifecycle Failures ---\n";

    {
        Rig rig;
        rig.executor.registerHandler("OK", [](const jobq::Job&) {});
        rig.executor.registerHandler("BAD", [](const jobq::Job&) {
            throw std::runtime_error("bad");
        });
        rig.startPool(4);

        std::vector<std::shared_ptr<jobq::Job>> jobs;
        for (int i = 0; i < 60; ++i) {
            jobs.push_back(rig.manager->submit(i % 2 ? "BAD" : "OK", "p"));
        }

        rig.pool->stop();
        rig.pool->stop();
        rig.pool->stop();

        bool valid = true;
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            const auto expected =
                i % 2 ? JobStatus::Failed : JobStatus::Completed;
            valid = valid && jobs[i]->getStatus() == expected;
        }
        check(valid, "Failures on some workers do not disturb the others");
        check(rig.pool->isStopped(), "Repeated stop after failures is safe");
        check(metricsConsistent(rig.metrics.snapshot()),
              "Metrics consistent after mixed failures");
    }

    {
        Rig rig;
        Gate release;
        Counter entered;
        rig.executor.registerHandler("T", [&](const jobq::Job&) {
            entered.add();
            release.wait();
            throw std::runtime_error("fails while shutting down");
        });
        rig.startPool(3);

        std::vector<std::shared_ptr<jobq::Job>> jobs;
        for (int i = 0; i < 3; ++i) {
            jobs.push_back(rig.manager->submit("T", "p"));
        }
        check(entered.waitAtLeast(3), "All three handlers are active");

        auto stopper =
            std::async(std::launch::async, [&] { rig.pool->stop(); });
        check(waitUntil([&] { return rig.queue.isClosed(); }),
              "Shutdown requested while handlers are active");
        check(!rig.pool->isStopped(),
              "Pool is not stopped while handlers are still running");

        release.open();
        check(stopper.wait_for(kTimeout) == std::future_status::ready,
              "Shutdown completes after active handlers fail");
        stopper.get();

        bool failed = true;
        for (const auto& job : jobs) {
            failed = failed && job->getStatus() == JobStatus::Failed &&
                     job->getAttemptsMade() == 1;
        }
        check(failed, "Jobs failing during shutdown end in a valid Failed state");
    }

    {
        Rig rig;
        Counter thrown;
        rig.executor.registerHandler("T", [&](const jobq::Job&) {
            thrown.add();
            throw std::runtime_error("fails right before shutdown");
        });
        rig.startPool(2);

        const auto job = rig.manager->submit("T", "p");
        thrown.waitAtLeast(1);
        check(stopsWithinTimeout(*rig.pool),
              "Shutdown right after a failure does not deadlock");
        check(job->getStatus() == JobStatus::Failed,
              "Job that failed just before shutdown is Failed");
    }

    {
        Rig rig;
        rig.executor.registerHandler("T", [](const jobq::Job&) {
            throw std::runtime_error("bad");
        });
        rig.startPool(3);
        rig.manager->submit("T", "p");
        rig.manager->submit("T", "p");
        rig.pool.reset();
        check(rig.queue.isClosed() && rig.queue.empty(),
              "Destroying the pool after failures joins every worker");
    }
}

std::uint64_t jobIndex(const jobq::Job& job) {
    return std::stoull(job.getPayload());
}

void testConcurrentFailureStress() {
    std::cout << "\n--- G: Concurrent Failure Stress ---\n";

    constexpr std::size_t kWorkers = 6;
    constexpr int kRounds = 5;

    auto expectedAttempts = [](std::size_t index) -> std::size_t {
        switch (index % 8) {
            case 3: return 1;
            case 4: return 2;
            case 5: return 3;
            case 6: return 3;
            default: return 1;
        }
    };
    auto expectedStatus = [](std::size_t index) {
        const auto kind = index % 8;
        return (kind == 3 || kind == 6) ? JobStatus::Failed
                                        : JobStatus::Completed;
    };
    auto policyFor = [](std::size_t index) {
        const auto kind = index % 8;
        return kind >= 4 ? retries(2) : retries(0);
    };

    for (int round = 0; round < kRounds; ++round) {
        const std::size_t kJobs = round == 0 ? 40 : 400;
        const std::uint64_t groups = kJobs / 8;
        TemporaryDirectory dir("stress");
        jobq::JobQueue queue;
        jobq::JobExecutor executor;
        jobq::JobMetrics metrics;

        auto fileStorage =
            std::make_shared<jobq::FileJobStorage>(dir.journal());
        std::shared_ptr<jobq::JobStorage> storage =
            round == 0
                ? std::shared_ptr<jobq::JobStorage>(fileStorage)
                : std::shared_ptr<jobq::JobStorage>(
                      std::make_shared<FaultInjectingJobStorage>()
                  );

        std::vector<std::atomic<int>> attempts(kJobs + 1);

        executor.registerHandler("T", [&](const jobq::Job& job) {
            const auto index = jobIndex(job);
            attempts[index]++;
            const auto kind = index % 8;
            const auto attempt = job.getAttemptsMade();

            if (kind == 7) {
                std::this_thread::sleep_for(200us);
            }

            if (kind == 3 || kind == 6) {
                if (kind == 3) {
                    throw 3;
                }
                throw std::runtime_error("always");
            }
            if (kind == 4 && attempt == 1) {
                throw std::runtime_error("flaky");
            }
            if (kind == 5 && attempt <= 2) {
                throw std::runtime_error("flakier");
            }
        });

        std::vector<std::shared_ptr<jobq::Job>> jobs;
        {
            jobq::JobManager manager(queue, storage, metrics);
            jobq::WorkerPool pool(queue, executor, kWorkers, metrics);
            pool.start();

            for (std::size_t i = 1; i <= kJobs; ++i) {
                jobs.push_back(manager.submit(
                    "T",
                    std::to_string(i),
                    JobPriority::Normal,
                    policyFor(i)
                ));
            }

            pool.stop();
        }

        bool statusesOk = true;
        bool attemptsOk = true;
        for (std::size_t i = 1; i <= kJobs; ++i) {
            const auto& job = jobs[i - 1];
            statusesOk = statusesOk && job->getStatus() == expectedStatus(i);
            attemptsOk = attemptsOk &&
                         attempts[i].load() ==
                             static_cast<int>(expectedAttempts(i)) &&
                         job->getAttemptsMade() == expectedAttempts(i);
        }

        const auto snap = metrics.snapshot();
        const bool countersOk =
            snap.submittedJobs == kJobs &&
            snap.startedExecutions == 13 * groups &&
            snap.completedExecutions == 6 * groups &&
            snap.failedExecutions == 7 * groups &&
            snap.scheduledRetries == 5 * groups;

        bool recoveryOk = true;
        if (round == 0) {
            jobq::JobQueue queue2;
            jobq::JobMetrics metrics2;
            jobq::JobManager recovered(queue2, storage, metrics2);
            recoveryOk = recovered.totalJobs() == kJobs && queue2.empty() &&
                         metrics2.snapshot().submittedJobs == 0;
            for (std::size_t i = 1; i <= kJobs && recoveryOk; ++i) {
                recoveryOk = recovered.getStatus(i) == expectedStatus(i);
            }
        }

        if (!statusesOk || !attemptsOk || !countersOk || !recoveryOk ||
            !metricsConsistent(snap) || !queue.empty()) {
            check(statusesOk, "Stress: final job statuses");
            check(attemptsOk, "Stress: per-job attempt counts");
            check(countersOk, "Stress: metric counters");
            check(recoveryOk, "Stress: recovery from journal");
            check(metricsConsistent(snap), "Stress: metrics invariants");
            check(queue.empty(), "Stress: queue drained");
            return;
        }
    }

    check(true,
          "Round 0 (file journal, 40 jobs) and 4 rounds of 400 jobs on 6 workers: statuses, attempts, retries, "
          "metrics and queue are exactly as expected");
    check(true, "Journal recovery after the stress run restores every final state");
}

}

int main() {
    std::cout << "========================================\n";
    std::cout << "     JobQ Fault Injection Tests\n";
    std::cout << "========================================\n";

    try {
        testHandlerFailures();
        testStorageReadFailure();
        testStorageSaveFailureOnSubmit();
        testStorageFailureOnRunningTransition();
        testStorageFailureOnCompletion();
        testRetrySchedulingException();
        testStorageFailureDuringRecovery();
        testSubmitToClosedQueue();
        testCloseWhileProducersSubmit();
        testClosureWakesIdleWorkers();
        testShutdownWithRetryAndLateSubmit();
        testConcurrentRetries();
        testCorruptedJournals();
        testWorkerLifecycleFailures();
        testConcurrentFailureStress();
    } catch (const std::exception& error) {
        check(false, std::string("Unexpected exception: ") + error.what());
    }

    std::cout << "\nTests run: " << testsRun() << '\n';
    std::cout << "Tests failed: " << testsFailed() << '\n';
    std::cout << (testsFailed() == 0 ? "Result: ALL TESTS PASSED\n"
                                     : "Result: SOME TESTS FAILED\n");

    return testsFailed() == 0 ? 0 : 1;
}
