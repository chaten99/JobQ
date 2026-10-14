#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <syncstream>
#include <thread>

#include "executor/JobExecutor.h"
#include "job/Job.h"
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

    try {
        lifecycleTest->markRunning();

        std::cout << "Invalid transition test: FAIL\n";
    }
    catch (const std::logic_error& exception) {
        std::cout << "Invalid transition test: PASS\n";
        std::cout << "Expected error: "
                  << exception.what()
                  << '\n';
    }

    std::cout << "========================================\n\n";

    jobq::JobQueue queue;
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


    bool allJobsAccepted = true;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1001, "REPORT", "monthly-report"
            )
        ) && allJobsAccepted;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1002, "EMAIL", "welcome-email"
            )
        ) && allJobsAccepted;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1003, "CALCULATE", "sum-1000000"
            )
        ) && allJobsAccepted;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1004, "REPORT", "sales-report"
            )
        ) && allJobsAccepted;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1005, "EMAIL", "notification-email"
            )
        ) && allJobsAccepted;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1006, "CALCULATE", "analytics"
            )
        ) && allJobsAccepted;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1007, "FAIL", "intentional-failure"
            )
        ) && allJobsAccepted;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1008, "UNKNOWN", "no-handler"
            )
        ) && allJobsAccepted;

    allJobsAccepted =
        queue.push(
            std::make_shared<jobq::Job>(
                1009, "EMAIL", "worker-survival-test"
            )
        ) && allJobsAccepted;

    if (!allJobsAccepted) {
        std::cerr
            << "ERROR: One or more jobs were rejected.\n";

        pool.stop();
        return 1;
    }

    std::cout << "Submitted 9 jobs.\n";

    pool.stop();

    std::cout << "JobQ stopped cleanly.\n";

    return 0;
}