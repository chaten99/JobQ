#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

namespace {

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

void testWorkerRetriesAndExhaustion() {
    std::cout << "\n--- Worker Retry Integration ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);
    jobq::JobExecutor executor;

    executor.registerHandler(
        "FLAKY",
        [](const jobq::Job& job) {
            if (job.getAttemptsMade() < 3) {
                throw std::runtime_error(
                    "Temporary failure"
                );
            }
        }
    );

    executor.registerHandler(
        "ALWAYS_FAIL",
        [](const jobq::Job&) {
            throw std::runtime_error(
                "Permanent failure"
            );
        }
    );

    executor.registerHandler(
        "NO_RETRY",
        [](const jobq::Job&) {
            throw std::runtime_error(
                "Failure without retries"
            );
        }
    );

    jobq::WorkerPool pool(queue, executor, 1);

    pool.start();

    auto flakyJob = manager.submit(
        "FLAKY",
        "fails-twice-then-succeeds",
        jobq::JobPriority::High,
        jobq::RetryPolicy{2}
    );

    auto permanentJob = manager.submit(
        "ALWAYS_FAIL",
        "fails-until-retries-exhausted",
        jobq::JobPriority::Normal,
        jobq::RetryPolicy{1}
    );

    auto noRetryJob = manager.submit(
        "NO_RETRY",
        "only-one-attempt",
        jobq::JobPriority::Low,
        jobq::RetryPolicy{0}
    );
    pool.stop();

    const auto flakyStatus =
        manager.getStatus(flakyJob->getId());

    const auto permanentStatus =
        manager.getStatus(permanentJob->getId());

    const auto noRetryStatus =
        manager.getStatus(noRetryJob->getId());

    check(
        flakyStatus.has_value() &&
            *flakyStatus == jobq::JobStatus::Completed,
        "Transiently failing job eventually completes"
    );

    check(
        flakyJob->getAttemptsMade() == 3,
        "Successful job completes after two retries"
    );

    check(
        permanentStatus.has_value() &&
            *permanentStatus == jobq::JobStatus::Failed,
        "Permanently failing job remains FAILED"
    );

    check(
        permanentJob->getAttemptsMade() == 2,
        "One configured retry permits two total attempts"
    );

    check(
        noRetryStatus.has_value() &&
            *noRetryStatus == jobq::JobStatus::Failed,
        "Job with zero retries remains FAILED"
    );

    check(
        noRetryJob->getAttemptsMade() == 1,
        "Zero-retry job executes only once"
    );

    check(
        flakyJob->getPriority() == jobq::JobPriority::High,
        "Retry preserves the original job priority"
    );

    check(
        manager.totalJobs() == 3,
        "All submitted jobs remain tracked"
    );

    check(
        queue.isClosed() && queue.empty(),
        "Shutdown drains accepted work and its retries"
    );

    check(
        flakyJob->getStatus() != jobq::JobStatus::Pending &&
            permanentJob->getStatus() != jobq::JobStatus::Pending &&
            noRetryJob->getStatus() != jobq::JobStatus::Pending,
        "No job is left PENDING after worker shutdown"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "       Job Retry Execution Tests        \n";
    std::cout << "========================================\n";

    try {
        testWorkerRetriesAndExhaustion();
    } catch (const std::exception& exception) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected exception: "
            << exception.what()
            << '\n';
    } catch (...) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected non-standard exception\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "Tests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';

    if (testsFailed == 0) {
        std::cout << "Result: ALL TESTS PASSED\n";
    } else {
        std::cout << "Result: TESTS FAILED\n";
    }

    std::cout << "========================================\n";

    return testsFailed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}