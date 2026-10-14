
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"

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

void testCreateAndLookup() {
    std::cout << "\n--- Job Creation and Lookup ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto firstJob = manager.createJob(
        "EMAIL",
        "first-test-job"
    );

    auto secondJob = manager.createJob(
        "REPORT",
        "second-test-job"
    );

    const auto foundJob = manager.getJob(firstJob->getId());
    const auto status = manager.getStatus(firstJob->getId());

    check(
        firstJob != nullptr && secondJob != nullptr,
        "JobManager creates jobs"
    );

    check(
        firstJob->getId() != secondJob->getId(),
        "Generated job IDs are unique"
    );

    check(
        foundJob == firstJob,
        "Registry returns the correct job"
    );

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Pending,
        "New job starts in PENDING status"
    );

    check(
        manager.totalJobs() == 2,
        "Registry tracks both created jobs"
    );

    check(
        queue.empty(),
        "createJob does not enqueue the job"
    );

    check(
        manager.getJob(0) == nullptr,
        "Unknown job ID returns nullptr"
    );

    check(
        !manager.getStatus(0).has_value(),
        "Unknown job ID has no status"
    );
}

void testSubmissionAndLifecycleTracking() {
    std::cout << "\n--- Submission and Lifecycle Tracking ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto submittedJob = manager.submit(
        "EMAIL",
        "lifecycle-test-job"
    );

    check(
        manager.totalJobs() == 1,
        "Submitted job is stored in the registry"
    );

    check(
        queue.size() == 1,
        "Submitted job is added to the queue"
    );

    const auto queuedJob = queue.tryPop();

    check(
        queuedJob == submittedJob,
        "Queue returns the submitted job"
    );

    auto status = manager.getStatus(submittedJob->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Pending,
        "Submitted job is initially PENDING"
    );

    submittedJob->markRunning();

    status = manager.getStatus(submittedJob->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Running,
        "Manager reflects the RUNNING status"
    );

    submittedJob->markCompleted();

    status = manager.getStatus(submittedJob->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Completed,
        "Manager reflects the COMPLETED status"
    );
}

void testFailedJobTracking() {
    std::cout << "\n--- Failed Job Tracking ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto job = manager.submit(
        "FAIL",
        "failed-job-test"
    );

    const auto queuedJob = queue.tryPop();

    check(
        queuedJob == job,
        "Failed-job test retrieves the submitted job"
    );

    job->markRunning();
    job->markFailed();

    const auto status = manager.getStatus(job->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Failed,
        "Manager reflects the FAILED status"
    );

    check(
        manager.getJob(job->getId()) == job,
        "Failed job remains available in the registry"
    );
}

void testCancelledJobTracking() {
    std::cout << "\n--- Cancelled Job Tracking ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto job = manager.createJob(
        "REPORT",
        "cancelled-job-test"
    );

    job->markCancelled();

    const auto status = manager.getStatus(job->getId());

    check(
        status.has_value() &&
            *status == jobq::JobStatus::Cancelled,
        "Manager reflects the CANCELLED status"
    );
}

void testClosedQueueRollback() {
    std::cout << "\n--- Closed Queue Submission Rollback ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto existingJob = manager.createJob(
        "EMAIL",
        "existing-registry-job"
    );

    const auto initialCount = manager.totalJobs();

    queue.close();

    bool submissionRejected = false;

    try {
        manager.submit(
            "EMAIL",
            "should-not-be-submitted"
        );
    } catch (const std::runtime_error&) {
        submissionRejected = true;
    }

    check(
        submissionRejected,
        "Submission to a closed queue throws an error"
    );

    check(
        manager.totalJobs() == initialCount,
        "Rejected submission is removed from the registry"
    );

    check(
        manager.getJob(existingJob->getId()) == existingJob,
        "Rollback preserves previously registered jobs"
    );

    check(
        queue.empty(),
        "Rejected job is not added to the closed queue"
    );
}

void testConcurrentSubmissionAndTracking() {
    std::cout << "\n--- Concurrent Submission and Tracking ---\n";

    constexpr std::size_t producerCount = 4;
    constexpr std::size_t jobsPerProducer = 250;
    constexpr std::size_t expectedJobs =
        producerCount * jobsPerProducer;

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    std::atomic<std::size_t> successfulSubmissions{0};
    std::atomic<std::size_t> submissionFailures{0};
    std::atomic<std::size_t> concurrentLookupErrors{0};

    std::atomic<bool> readerReady{false};
    std::atomic<bool> submissionsFinished{false};

    std::thread reader([&]() {
        std::uint64_t probeId = 1;

        readerReady.store(true, std::memory_order_release);

        while (!submissionsFinished.load(std::memory_order_acquire)) {
            const auto currentCount = manager.totalJobs();

            if (currentCount > expectedJobs) {
                concurrentLookupErrors.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }

            auto observedJob = manager.getJob(probeId);

            if (observedJob) {
                const auto status = manager.getStatus(probeId);

                if (!status.has_value() ||
                    *status != jobq::JobStatus::Pending) {
                    concurrentLookupErrors.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                }
            }

            probeId = (probeId % expectedJobs) + 1;

            std::this_thread::yield();
        }
    });

    while (!readerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (std::size_t producer = 0;
         producer < producerCount;
         ++producer) {

        producers.emplace_back([&, producer]() {
            for (std::size_t index = 0;
                 index < jobsPerProducer;
                 ++index) {

                try {
                    const std::string payload =
                        "producer-" + std::to_string(producer) +
                        "-job-" + std::to_string(index);

                    manager.submit("EMAIL", payload);

                    successfulSubmissions.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                } catch (const std::exception&) {
                    submissionFailures.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                } catch (...) {
                    submissionFailures.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                }

                if (index % 16 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    submissionsFinished.store(true, std::memory_order_release);
    reader.join();

    const auto submittedCount =
        successfulSubmissions.load(std::memory_order_relaxed);

    const auto failureCount =
        submissionFailures.load(std::memory_order_relaxed);

    const auto lookupErrorCount =
        concurrentLookupErrors.load(std::memory_order_relaxed);

    check(
        submittedCount == expectedJobs,
        "Concurrent producers submit all 1000 jobs"
    );

    check(
        failureCount == 0,
        "Concurrent submissions have no failures"
    );

    check(
        manager.totalJobs() == expectedJobs,
        "Registry contains all concurrently submitted jobs"
    );

    check(
        queue.size() == expectedJobs,
        "Queue contains all concurrently submitted jobs"
    );

    check(
        lookupErrorCount == 0,
        "Concurrent registry and status lookups remain consistent"
    );

    std::unordered_set<std::uint64_t> observedIds;
    observedIds.reserve(expectedJobs);

    bool uniqueIds = true;
    bool allJobsMatchRegistry = true;
    std::size_t drainedJobs = 0;

    while (auto job = queue.tryPop()) {
        ++drainedJobs;

        const auto inserted = observedIds.insert(job->getId()).second;

        if (!inserted) {
            uniqueIds = false;
        }

        if (manager.getJob(job->getId()) != job) {
            allJobsMatchRegistry = false;
        }

        const auto status = manager.getStatus(job->getId());

        if (!status.has_value() ||
            *status != jobq::JobStatus::Pending) {
            allJobsMatchRegistry = false;
        }
    }

    check(
        uniqueIds && observedIds.size() == expectedJobs,
        "All concurrently submitted job IDs are unique"
    );

    check(
        allJobsMatchRegistry,
        "Every queued job matches its registry entry and status"
    );

    check(
        drainedJobs == expectedJobs,
        "All 1000 jobs can be retrieved from the queue"
    );

    check(
        queue.empty(),
        "Queue is empty after every submitted job is drained"
    );

    check(
        manager.totalJobs() == expectedJobs,
        "Registry retains all jobs after the queue is drained"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "       JobManager Automated Tests       \n";
    std::cout << "========================================\n";

    try {
        testCreateAndLookup();
        testSubmissionAndLifecycleTracking();
        testFailedJobTracking();
        testCancelledJobTracking();
        testClosedQueueRollback();
        testConcurrentSubmissionAndTracking();
    } catch (const std::exception& exception) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected exception: "
            << exception.what()
            << '\n';
    } catch (...) {
        ++testsFailed;

        std::cerr << "[FAIL] Unexpected non-standard exception\n";
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
