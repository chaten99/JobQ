#include <chrono>
#include <iostream>
#include <stdexcept>
#include <syncstream>
#include <thread>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

int main() {
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

    queue.push(
        jobq::Job(
            1001,
            "REPORT",
            "monthly-report"
        )
    );

    queue.push(
        jobq::Job(
            1002,
            "EMAIL",
            "welcome-email"
        )
    );

    queue.push(
        jobq::Job(
            1003,
            "CALCULATE",
            "sum-1000000"
        )
    );

    queue.push(
        jobq::Job(
            1004,
            "REPORT",
            "sales-report"
        )
    );

    queue.push(
        jobq::Job(
            1005,
            "EMAIL",
            "notification-email"
        )
    );

    queue.push(
        jobq::Job(
            1006,
            "CALCULATE",
            "analytics"
        )
    );

    queue.push(
        jobq::Job(
            1007,
            "FAIL",
            "intentional-failure"
        )
    );

    queue.push(
        jobq::Job(
            1008,
            "UNKNOWN",
            "no-handler"
        )
    );

    queue.push(
        jobq::Job(
            1009,
            "EMAIL",
            "worker-survival-test"
        )
    );

    std::cout << "Submitted 9 jobs.\n";

    pool.stop();

    std::cout
        << "JobQ stopped cleanly.\n";

    return 0;
}