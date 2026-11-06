#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "metrics/JobMetrics.h"
#include "queue/JobQueue.h"
#include "worker/Worker.h"
#include "worker/WorkerPool.h"

namespace {

using namespace std::chrono_literals;

constexpr auto kTimeout = 10s;

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

template <typename Exception, typename Fn>
bool throwsType(Fn&& fn) {
    try {
        fn();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

class ThreadRecorder {
public:
    void record() {
        std::lock_guard<std::mutex> lock(mutex_);
        ids_.insert(std::this_thread::get_id());
    }

    std::size_t distinct() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ids_.size();
    }

private:
    mutable std::mutex mutex_;
    std::set<std::thread::id> ids_;
};

class Rendezvous {
public:
    explicit Rendezvous(std::size_t expected) : expected_(expected) {}

    bool arriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (++arrived_ >= expected_) {
            cv_.notify_all();
        }
        return cv_.wait_for(
            lock, kTimeout, [&] { return arrived_ >= expected_; }
        );
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t expected_;
    std::size_t arrived_{0};
};

bool waitForStatus(
    const std::shared_ptr<jobq::Job>& job,
    jobq::JobStatus status
) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (job->getStatus() == status) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return job->getStatus() == status;
}

void testNormalLifecycle() {
    std::cout << "\n--- Test A: Normal Worker Lifecycle ---\n";

    constexpr std::size_t kWorkers = 3;
    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    ThreadRecorder threads;
    std::atomic<int> executed{0};

    executor.registerHandler("WORK", [&](const jobq::Job&) {
        threads.record();
        executed.fetch_add(1);
    });

    std::vector<std::shared_ptr<jobq::Job>> jobs;
    {
        jobq::WorkerPool pool(queue, executor, kWorkers, metrics);
        check(pool.size() == kWorkers, "Pool reports configured size");
        check(!pool.isStarted(), "Pool not started before start()");
        pool.start();
        check(pool.isStarted(), "Pool started");

        for (int i = 0; i < 20; ++i) {
            jobs.push_back(manager.submit("WORK", "p"));
        }

        pool.stop();
        check(pool.isStopped(), "Pool stopped (all workers joined)");
    }

    bool allCompleted = true;
    for (const auto& job : jobs) {
        allCompleted = allCompleted &&
            job->getStatus() == jobq::JobStatus::Completed;
    }
    check(allCompleted, "All submitted jobs Completed");
    check(executed.load() == 20, "Each job executed exactly once");
    check(threads.distinct() <= kWorkers,
          "No more threads than workers executed jobs");

    const auto snap = metrics.snapshot();
    check(snap.submittedJobs == 20 && snap.startedExecutions == 20 &&
              snap.completedExecutions == 20 && snap.failedExecutions == 0,
          "Metrics unchanged by lifecycle hardening");
}

void testRepeatedWorkerStart() {
    std::cout << "\n--- Test B: Repeated Worker Start ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobManager manager(queue);
    ThreadRecorder threads;
    std::atomic<int> executed{0};

    executor.registerHandler("WORK", [&](const jobq::Job&) {
        threads.record();
        executed.fetch_add(1);
    });

    jobq::Worker worker(1, queue, executor);
    worker.start();
    check(throwsType<std::logic_error>([&] { worker.start(); }),
          "Second Worker::start() throws std::logic_error");
    check(throwsType<std::logic_error>([&] { worker.start(); }),
          "Third Worker::start() throws std::logic_error");

    for (int i = 0; i < 10; ++i) {
        manager.submit("WORK", "p");
    }
    queue.close();
    worker.join();

    check(executed.load() == 10, "Jobs drained by the single worker");
    check(threads.distinct() == 1, "Only one execution thread exists");
    check(throwsType<std::logic_error>([&] { worker.start(); }),
          "Joined worker cannot be restarted");
}

void testRepeatedWorkerJoin() {
    std::cout << "\n--- Test C: Repeated Worker Join ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;

    jobq::Worker started(1, queue, executor);
    started.start();
    queue.close();
    started.join();
    bool noThrow = true;
    try {
        started.join();
        started.join();
    } catch (...) {
        noThrow = false;
    }
    check(noThrow, "Repeated join() on started worker is a safe no-op");

    jobq::Worker neverStarted(2, queue, executor);
    check(!throwsType<std::logic_error>([&] { neverStarted.join(); }),
          "join() on never-started worker does not throw");
    check(throwsType<std::logic_error>([&] { neverStarted.start(); }),
          "Worker joined before start cannot be started");

    jobq::JobQueue queue2;
    jobq::JobExecutor executor2;
    jobq::JobManager manager2(queue2);
    jobq::Worker selfWorker(3, queue2, executor2);
    std::promise<bool> selfJoinThrew;
    auto selfJoinFuture = selfJoinThrew.get_future();
    executor2.registerHandler("SELF", [&](const jobq::Job&) {
        selfJoinThrew.set_value(
            selfWorker.isCurrentThread() &&
            throwsType<std::logic_error>([&] { selfWorker.join(); })
        );
    });
    selfWorker.start();
    manager2.submit("SELF", "p");
    const bool ready =
        selfJoinFuture.wait_for(kTimeout) == std::future_status::ready;
    queue2.close();
    selfWorker.join();
    check(ready && selfJoinFuture.get(),
          "Worker::join() from its own thread throws instead of deadlocking");
    check(!selfWorker.isCurrentThread(),
          "isCurrentThread() is false on other threads");
}

void testRepeatedPoolStart() {
    std::cout << "\n--- Test D: Repeated WorkerPool Start ---\n";

    constexpr std::size_t kWorkers = 3;
    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobManager manager(queue);
    ThreadRecorder threads;
    Rendezvous rendezvous(kWorkers);
    std::atomic<bool> rendezvousFailed{false};
    std::atomic<bool> blocking{true};

    executor.registerHandler("WORK", [&](const jobq::Job&) {
        threads.record();
        if (blocking.load() && !rendezvous.arriveAndWait()) {
            rendezvousFailed.store(true);
        }
    });

    jobq::WorkerPool pool(queue, executor, kWorkers);
    pool.start();

    check(throwsType<std::logic_error>([&] { pool.start(); }),
          "Second pool start() throws std::logic_error");
    check(pool.isStarted() && !pool.isStopped(),
          "Pool state unchanged by rejected start");

    std::vector<std::shared_ptr<jobq::Job>> jobs;
    for (std::size_t i = 0; i < kWorkers; ++i) {
        jobs.push_back(manager.submit("WORK", "p"));
    }
    for (const auto& job : jobs) {
        waitForStatus(job, jobq::JobStatus::Completed);
    }
    blocking.store(false);

    check(!rendezvousFailed.load(), "All workers ran concurrently");
    check(threads.distinct() == kWorkers, "Exactly one thread per worker");

    for (int i = 0; i < 20; ++i) {
        manager.submit("WORK", "p");
    }
    pool.stop();
    check(threads.distinct() == kWorkers,
          "Rejected start created no extra threads");
}

void testStopBeforeStart() {
    std::cout << "\n--- Test E: Stop Before Start ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    {
        jobq::WorkerPool pool(queue, executor, 2);
        pool.stop();
        check(pool.isStopped() && !pool.isStarted(),
              "Stopped without ever starting");
        check(queue.isClosed(), "Queue closed by stop()");
    }
    check(true, "Destroying a stopped, never-started pool does not hang");

    jobq::JobQueue queue2;
    {
        jobq::WorkerPool pool(queue2, executor, 2);
    }
    check(queue2.isClosed(), "Destroying an unstarted pool is safe");
}

void testRepeatedStop() {
    std::cout << "\n--- Test F: Repeated Stop ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobManager manager(queue);
    std::atomic<int> executed{0};
    executor.registerHandler("WORK", [&](const jobq::Job&) { executed++; });

    jobq::WorkerPool pool(queue, executor, 2);
    pool.start();
    for (int i = 0; i < 5; ++i) {
        manager.submit("WORK", "p");
    }

    bool noThrow = true;
    try {
        pool.stop();
        pool.stop();
        pool.stop();
    } catch (...) {
        noThrow = false;
    }
    check(noThrow, "Repeated stop() does not throw");
    check(pool.isStopped(), "Pool stopped");
    check(executed.load() == 5, "Queued jobs drained exactly once");

    jobq::JobQueue queue2;
    jobq::WorkerPool pool2(queue2, executor, 2);
    pool2.start();
    std::vector<std::thread> stoppers;
    std::atomic<int> returnedStopped{0};
    for (int i = 0; i < 4; ++i) {
        stoppers.emplace_back([&] {
            pool2.stop();
            if (pool2.isStopped()) {
                returnedStopped++;
            }
        });
    }
    for (auto& t : stoppers) {
        t.join();
    }
    check(returnedStopped.load() == 4,
          "Concurrent stop() calls all observe a stopped pool");
}

void testStartAfterStop() {
    std::cout << "\n--- Test G: Start After Stop ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;

    jobq::WorkerPool pool(queue, executor, 2);
    pool.start();
    pool.stop();
    check(throwsType<std::logic_error>([&] { pool.start(); }),
          "start() after stop() throws std::logic_error");
    check(pool.isStopped(), "Pool remains stopped");

    jobq::JobQueue queue2;
    jobq::WorkerPool pool2(queue2, executor, 2);
    pool2.stop();
    check(throwsType<std::logic_error>([&] { pool2.start(); }),
          "start() after stop-before-start throws std::logic_error");
}

void testHandlerExceptionSurvival() {
    std::cout << "\n--- Test H: Handler Exception Survival ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::JobMetrics metrics;
    jobq::JobManager manager(queue, metrics);
    std::atomic<int> goodRuns{0};
    std::atomic<int> badRuns{0};

    executor.registerHandler("BAD", [&](const jobq::Job&) {
        badRuns++;
        throw std::runtime_error("boom");
    });
    executor.registerHandler("WEIRD", [&](const jobq::Job&) { throw 42; });
    executor.registerHandler("GOOD", [&](const jobq::Job&) { goodRuns++; });

    jobq::WorkerPool pool(queue, executor, 1, metrics);
    pool.start();

    auto bad = manager.submit("BAD", "p");
    auto weird = manager.submit("WEIRD", "p");
    jobq::RetryPolicy policy;
    policy.maxRetries = 2;
    policy.initialDelay = 1ms;
    policy.maxDelay = 1ms;
    auto retried =
        manager.submit("BAD", "p", jobq::JobPriority::Normal, policy);
    auto good = manager.submit("GOOD", "p");

    check(waitForStatus(good, jobq::JobStatus::Completed),
          "Valid job after throwing handlers still executes");
    check(waitForStatus(bad, jobq::JobStatus::Failed),
          "Throwing job without retries is Failed");
    check(waitForStatus(weird, jobq::JobStatus::Failed),
          "Non-std exception is handled as a failure");
    check(waitForStatus(retried, jobq::JobStatus::Failed),
          "Throwing job with retries ends Failed after exhausting policy");

    auto after = manager.submit("GOOD", "p");
    check(waitForStatus(after, jobq::JobStatus::Completed),
          "Worker still operational after failures");

    pool.stop();
    check(pool.isStopped(), "Shutdown completed after handler exceptions");
    check(badRuns.load() == 4, "Retry policy followed (1 + 3 attempts)");
    check(goodRuns.load() == 2, "Good jobs executed once each");

    const auto snap = metrics.snapshot();
    check(snap.failedExecutions == 5 && snap.scheduledRetries == 2 &&
              snap.completedExecutions == 2,
          "Failure/retry metrics follow existing semantics");
}

void testResourceCleanup() {
    std::cout << "\n--- Test I: Resource Cleanup ---\n";

    jobq::JobExecutor executor;
    std::atomic<int> executed{0};
    executor.registerHandler("WORK", [&](const jobq::Job&) { executed++; });

    for (int round = 0; round < 25; ++round) {
        jobq::JobQueue q;
        jobq::JobManager m(q);
        jobq::WorkerPool pool(q, executor, 4);
        pool.start();
        m.submit("WORK", "p");
    }
    check(executed.load() == 25,
          "Destruction after start drained work, no terminate");

    jobq::JobQueue q2;
    jobq::JobManager m2(q2);
    {
        auto worker = std::make_unique<jobq::Worker>(1, q2, executor);
        worker->start();
        m2.submit("WORK", "p");
        q2.close();
        worker.reset();
    }
    check(executed.load() == 26,
          "Worker destructor joins a still-running thread (no terminate)");

    jobq::JobQueue q3;
    jobq::JobExecutor executor3;
    jobq::JobManager m3(q3);
    jobq::WorkerPool pool3(q3, executor3, 2);
    std::promise<bool> result;
    auto resultFuture = result.get_future();
    executor3.registerHandler("STOP", [&](const jobq::Job&) {
        result.set_value(throwsType<std::logic_error>([&] { pool3.stop(); }));
    });
    pool3.start();
    m3.submit("STOP", "p");
    const bool ready =
        resultFuture.wait_for(kTimeout) == std::future_status::ready;
    check(ready && resultFuture.get(),
          "pool.stop() from a worker thread throws instead of deadlocking");
    pool3.stop();
    check(pool3.isStopped(), "Pool stops normally after rejected self-stop");
}

void testStartupFailureSafety() {
    std::cout << "\n--- Test J: Startup Failure Safety (state machine) ---\n";

    jobq::JobQueue queue;
    jobq::JobExecutor executor;
    jobq::Worker w1(1, queue, executor);
    jobq::Worker w2(2, queue, executor);

    w1.start();
    queue.close();
    bool ok = true;
    try {
        w1.join();
        w2.join();
    } catch (...) {
        ok = false;
    }
    check(ok, "Rollback join of started + never-started workers is safe");
    check(throwsType<std::logic_error>([&] { w2.start(); }),
          "Rolled-back worker cannot be started later");
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "     JobQ Worker Lifecycle Tests\n";
    std::cout << "========================================\n";

    testNormalLifecycle();
    testRepeatedWorkerStart();
    testRepeatedWorkerJoin();
    testRepeatedPoolStart();
    testStopBeforeStart();
    testRepeatedStop();
    testStartAfterStop();
    testHandlerExceptionSurvival();
    testResourceCleanup();
    testStartupFailureSafety();

    std::cout << "\nTests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0 ? "Result: ALL TESTS PASSED\n"
                                   : "Result: SOME TESTS FAILED\n");

    return testsFailed == 0 ? 0 : 1;
}
