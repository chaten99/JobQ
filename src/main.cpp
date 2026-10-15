#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <syncstream>
#include <thread>
#include <vector>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

int main() {
    std::cout << "========== Job Lifecycle Test ==========\n";

    auto lifecycleTest = std::make_shared<jobq::Job>(
        9999,
        "TEST",
        "lifecycle-validation"
    );

    std::cout
        << "Initial status: "
        << jobq::toString(lifecycleTest->getStatus())
        << '\n';

    lifecycleTest->markRunning();

    std::cout
        << "After starting: "
        << jobq::toString(lifecycleTest->getStatus())
        << '\n';

    lifecycleTest->markCompleted();

    std::cout
        << "After completion: "
        << jobq::toString(lifecycleTest->getStatus())
        << '\n';

    bool invalidTransitionPassed = false;

    try {
        lifecycleTest->markRunning();

        std::cout << "Invalid transition test: FAIL\n";
    } catch (const std::logic_error& exception) {
        invalidTransitionPassed = true;

        std::cout << "Invalid transition test: PASS\n";
        std::cout
            << "Expected error: "
            << exception.what()
            << '\n';
    }

    std::cout << "========================================\n\n";

    if (!invalidTransitionPassed) {
        std::cerr << "Job lifecycle test failed.\n";
        return 1;
    }

    std::cout << "========== JobManager Test ==========\n";

    jobq::JobQueue managerTestQueue;
    jobq::JobManager managerTest(managerTestQueue);

    auto trackedJob = managerTest.submit(
        "EMAIL",
        "job-manager-submit-smoke-test"
    );

    const auto jobId = trackedJob->getId();

    auto foundJob = managerTest.getJob(jobId);
    auto foundStatus = managerTest.getStatus(jobId);

    if (!foundJob || !foundStatus.has_value()) {
        std::cerr << "JobManager lookup test: FAIL\n";
        return 1;
    }

    std::cout
        << "Generated Job ID: "
        << jobId
        << '\n';

    std::cout
        << "Tracked status: "
        << jobq::toString(*foundStatus)
        << '\n';

    std::cout
        << "Tracked jobs: "
        << managerTest.totalJobs()
        << '\n';

    std::cout
        << "Queued jobs: "
        << managerTestQueue.size()
        << '\n';

    const bool submissionPassed =
        foundJob == trackedJob &&
        *foundStatus == jobq::JobStatus::Pending &&
        managerTest.totalJobs() == 1 &&
        managerTestQueue.size() == 1;

    std::cout
        << "Job submission test: "
        << (submissionPassed ? "PASS" : "FAIL")
        << '\n';

    managerTestQueue.close();

    bool closedQueueHandled = false;

    try {
        managerTest.submit(
            "EMAIL",
            "rejected-submission-test"
        );
    } catch (const std::runtime_error& exception) {
        closedQueueHandled = true;

        std::cout
            << "Expected submission error: "
            << exception.what()
            << '\n';
    }

    const bool rollbackPassed =
        closedQueueHandled &&
        managerTest.totalJobs() == 1 &&
        managerTestQueue.size() == 1;

    std::cout
        << "Closed queue rejection test: "
        << (rollbackPassed ? "PASS" : "FAIL")
        << '\n';

    std::cout << "====================================\n\n";

    if (!submissionPassed || !rollbackPassed) {
        std::cerr << "JobManager test failed.\n";
        return 1;
    }

    std::cout << "========== Worker Pool Demo ==========\n";

    jobq::JobQueue queue;
    jobq::JobManager manager(queue);
    jobq::JobExecutor executor;

    executor.registerHandler(
        "REPORT",
        [](const jobq::Job& job) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500)
            );

            std::osyncstream(std::cout)
                << "    Processing REPORT: "
                << job.getPayload()
                << '\n';
        }
    );

    executor.registerHandler(
        "EMAIL",
        [](const jobq::Job& job) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(300)
            );

            std::osyncstream(std::cout)
                << "    Sending EMAIL: "
                << job.getPayload()
                << '\n';
        }
    );

    executor.registerHandler(
        "CALCULATE",
        [](const jobq::Job& job) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(200)
            );

            std::osyncstream(std::cout)
                << "    Running CALCULATION: "
                << job.getPayload()
                << '\n';
        }
    );

    executor.registerHandler(
        "FAIL",
        [](const jobq::Job&) {
            throw std::runtime_error(
                "Intentional job failure"
            );
        }
    );

    constexpr std::size_t workerCount = 3;

    jobq::WorkerPool pool(
        queue,
        executor,
        workerCount
    );

    std::cout
        << "Starting JobQ with "
        << pool.size()
        << " workers...\n";

    pool.start();

    std::vector<std::shared_ptr<jobq::Job>> submittedJobs;
    submittedJobs.reserve(9);

    const auto submitDemoJob =
        [&manager, &submittedJobs](
            const char* type,
            const char* payload
        ) {
            auto job = manager.submit(type, payload);
            submittedJobs.push_back(std::move(job));
        };

    try {
        submitDemoJob("REPORT", "monthly-report");
        submitDemoJob("EMAIL", "welcome-email");
        submitDemoJob("CALCULATE", "sum-1000000");
        submitDemoJob("REPORT", "sales-report");
        submitDemoJob("EMAIL", "notification-email");
        submitDemoJob("CALCULATE", "analytics");
        submitDemoJob("FAIL", "intentional-failure");
        submitDemoJob("UNKNOWN", "no-handler");
        submitDemoJob("EMAIL", "worker-survival-test");
    } catch (const std::exception& exception) {
        std::cerr
            << "Job submission failed: "
            << exception.what()
            << '\n';

        pool.stop();
        return 1;
    }

    std::cout
        << "Submitted jobs: "
        << submittedJobs.size()
        << '\n';

    std::cout
        << "Jobs tracked by manager: "
        << manager.totalJobs()
        << '\n';

    pool.stop();

    std::cout << "JobQ stopped cleanly.\n";

    std::cout << "\n========== Final Job Statuses ==========\n";

    std::size_t completedJobs = 0;
    std::size_t failedJobs = 0;

    bool lifecycleTrackingPassed =
        manager.totalJobs() == submittedJobs.size();

    for (const auto& job : submittedJobs) {
        const auto status = manager.getStatus(job->getId());

        if (!status.has_value()) {
            std::cerr
                << "Missing registry entry for Job "
                << job->getId()
                << '\n';

            lifecycleTrackingPassed = false;
            continue;
        }

        std::cout
            << "Job "
            << job->getId()
            << " ("
            << job->getType()
            << ") final status: "
            << jobq::toString(*status)
            << '\n';

        if (*status == jobq::JobStatus::Completed) {
            ++completedJobs;
        } else if (*status == jobq::JobStatus::Failed) {
            ++failedJobs;
        } else {
            lifecycleTrackingPassed = false;
        }
    }

    const bool expectedResults =
        completedJobs == 7 &&
        failedJobs == 2 &&
        completedJobs + failedJobs == submittedJobs.size();

    lifecycleTrackingPassed =
        lifecycleTrackingPassed &&
        expectedResults &&
        manager.totalJobs() == 9;

    std::cout << "----------------------------------------\n";

    std::cout
        << "Total tracked jobs: "
        << manager.totalJobs()
        << '\n';

    std::cout
        << "Completed jobs: "
        << completedJobs
        << '\n';

    std::cout
        << "Failed jobs: "
        << failedJobs
        << '\n';

    std::cout
        << "Lifecycle tracking test: "
        << (lifecycleTrackingPassed ? "PASS" : "FAIL")
        << '\n';

    std::cout << "========================================\n";

    return lifecycleTrackingPassed ? 0 : 1;
}