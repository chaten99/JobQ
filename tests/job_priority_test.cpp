
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

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

void testPriorityMetadata() {
    std::cout << "\n--- Priority Metadata ---\n";

    jobq::Job defaultJob(
        1,
        "EMAIL",
        "default-priority"
    );

    jobq::Job lowJob(
        2,
        "REPORT",
        "low-priority",
        jobq::JobPriority::Low
    );

    jobq::Job highJob(
        3,
        "CALCULATE",
        "high-priority",
        jobq::JobPriority::High
    );

    check(
        defaultJob.getPriority() == jobq::JobPriority::Normal,
        "Jobs default to NORMAL priority"
    );

    check(
        lowJob.getPriority() == jobq::JobPriority::Low,
        "LOW priority can be assigned"
    );

    check(
        highJob.getPriority() == jobq::JobPriority::High,
        "HIGH priority can be assigned"
    );

    check(
        jobq::toString(jobq::JobPriority::High) == "HIGH",
        "Priority string conversion works"
    );
}

void testPriorityOrdering() {
    std::cout << "\n--- Priority Queue Ordering ---\n";

    jobq::JobQueue queue;

    auto lowFirst = std::make_shared<jobq::Job>(
        1, "EMAIL", "low-first", jobq::JobPriority::Low
    );

    auto normalFirst = std::make_shared<jobq::Job>(
        2, "EMAIL", "normal-first", jobq::JobPriority::Normal
    );

    auto highFirst = std::make_shared<jobq::Job>(
        3, "EMAIL", "high-first", jobq::JobPriority::High
    );

    auto normalSecond = std::make_shared<jobq::Job>(
        4, "EMAIL", "normal-second", jobq::JobPriority::Normal
    );

    auto lowSecond = std::make_shared<jobq::Job>(
        5, "EMAIL", "low-second", jobq::JobPriority::Low
    );

    const bool allAccepted =
        queue.push(lowFirst) &&
        queue.push(normalFirst) &&
        queue.push(highFirst) &&
        queue.push(normalSecond) &&
        queue.push(lowSecond);

    check(
        allAccepted,
        "Queue accepts jobs of different priorities"
    );

    check(
        queue.size() == 5,
        "Queue tracks all five submitted jobs"
    );

    check(
        queue.tryPop() == highFirst,
        "HIGH priority job is dequeued first"
    );

    check(
        queue.tryPop() == normalFirst,
        "First NORMAL job preserves FIFO order"
    );

    check(
        queue.tryPop() == normalSecond,
        "Second NORMAL job preserves FIFO order"
    );

    check(
        queue.tryPop() == lowFirst,
        "First LOW job is dequeued before later LOW jobs"
    );

    check(
        queue.tryPop() == lowSecond,
        "Second LOW job preserves FIFO order"
    );

    check(
        queue.empty(),
        "Queue is empty after every job is removed"
    );

    check(
        queue.tryPop() == nullptr,
        "Empty queue returns nullptr"
    );
}

void testManagerPrioritySubmission() {
    std::cout << "\n--- JobManager Priority Submission ---\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);

    auto lowJob = manager.submit(
        "EMAIL",
        "manager-low",
        jobq::JobPriority::Low
    );

    auto highJob = manager.submit(
        "EMAIL",
        "manager-high",
        jobq::JobPriority::High
    );

    auto normalJob = manager.submit(
        "EMAIL",
        "manager-normal"
    );

    check(
        lowJob->getPriority() == jobq::JobPriority::Low &&
        highJob->getPriority() == jobq::JobPriority::High &&
        normalJob->getPriority() == jobq::JobPriority::Normal,
        "JobManager stores submitted priorities"
    );

    check(
        manager.totalJobs() == 3,
        "All priority jobs remain registered"
    );

    check(
        queue.tryPop() == highJob,
        "JobManager HIGH job is dequeued first"
    );

    check(
        queue.tryPop() == normalJob,
        "JobManager NORMAL job follows HIGH"
    );

    check(
        queue.tryPop() == lowJob,
        "JobManager LOW job is dequeued last"
    );

    check(
        manager.getStatus(highJob->getId()).has_value() &&
        manager.getStatus(highJob->getId()).value() ==
            jobq::JobStatus::Pending,
        "Priority scheduling does not alter job lifecycle status"
    );
}

void testClosedQueueDrainsPriorities() {
    std::cout << "\n--- Closed Queue Draining ---\n";

    jobq::JobQueue queue;

    auto lowJob = std::make_shared<jobq::Job>(
        1, "EMAIL", "low", jobq::JobPriority::Low
    );

    auto highJob = std::make_shared<jobq::Job>(
        2, "EMAIL", "high", jobq::JobPriority::High
    );

    const bool accepted =
        queue.push(lowJob) &&
        queue.push(highJob);

    queue.close();

    check(
        accepted,
        "Queue accepts jobs before shutdown"
    );

    check(
        queue.isClosed(),
        "Queue reports closed state"
    );

    check(
        queue.tryPop() == highJob,
        "Closed queue still returns highest priority job first"
    );

    check(
        queue.tryPop() == lowJob,
        "Closed queue drains remaining jobs"
    );

    check(
        queue.tryPop() == nullptr,
        "Closed and drained queue returns nullptr"
    );

    check(
        !queue.push(std::make_shared<jobq::Job>(
            3, "EMAIL", "rejected"
        )),
        "Closed queue rejects new jobs"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "        Job Priority Automated Tests    \n";
    std::cout << "========================================\n";

    try {
        testPriorityMetadata();
        testPriorityOrdering();
        testManagerPrioritySubmission();
        testClosedQueueDrainsPriorities();
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
