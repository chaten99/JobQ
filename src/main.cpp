#include <chrono>
#include <iostream>
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

            std::cout
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

            std::cout
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

            std::cout
                << "    Running CALCULATION: "
                << job.getPayload()
                << '\n';
        }
    );

    jobq::WorkerPool pool(
        queue,
        executor,
        3
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

    std::cout << "Submitted 6 jobs.\n";

    pool.stop();

    std::cout
        << "JobQ stopped cleanly.\n";

    return 0;
}