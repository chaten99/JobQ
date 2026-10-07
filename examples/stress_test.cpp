#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "executor/JobExecutor.h"
#include "job/Job.h"
#include "queue/JobQueue.h"
#include "worker/WorkerPool.h"

int main() {
    constexpr std::size_t workerCount = 4;
    constexpr int producerCount = 4;
    constexpr int jobsPerProducer = 250;

    constexpr int expectedJobs =
        producerCount * jobsPerProducer;

    constexpr int expectedFailures =
        producerCount * 25;

    constexpr int expectedSuccesses =
        expectedJobs - expectedFailures;

    jobq::JobQueue queue;
    jobq::JobExecutor executor;

    std::atomic<int> completedJobs{0};
    std::atomic<int> failedJobs{0};
    std::atomic<int> submittedJobs{0};
    std::atomic<int> submissionFailures{0};

    executor.registerHandler(
        "REPORT",
        [&completedJobs](const jobq::Job&) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1)
            );

            completedJobs.fetch_add(
                1,
                std::memory_order_relaxed
            );
        }
    );

    executor.registerHandler(
        "EMAIL",
        [&completedJobs](const jobq::Job&) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1)
            );

            completedJobs.fetch_add(
                1,
                std::memory_order_relaxed
            );
        }
    );

    executor.registerHandler(
        "CALCULATE",
        [&completedJobs](const jobq::Job&) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1)
            );

            completedJobs.fetch_add(
                1,
                std::memory_order_relaxed
            );
        }
    );

    // ----------------------------------------
    // Intentionally failing handler
    // ----------------------------------------

    executor.registerHandler(
        "FAIL",
        [&failedJobs](const jobq::Job&) {
            failedJobs.fetch_add(
                1,
                std::memory_order_relaxed
            );

            throw std::runtime_error(
                "Intentional stress-test failure"
            );
        }
    );


    jobq::WorkerPool pool(
        queue,
        executor,
        workerCount
    );

    std::cerr
        << "Starting stress test with "
        << workerCount
        << " workers and "
        << expectedJobs
        << " jobs...\n";

    const auto startTime =
        std::chrono::steady_clock::now();

    pool.start();


    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (int producer = 0;
         producer < producerCount;
         ++producer) {

        producers.emplace_back(
            [&queue,
             &submittedJobs,
             &submissionFailures,
             producer]() {

                for (int i = 0;
                     i < jobsPerProducer;
                     ++i) {

                    const auto jobId =
                        static_cast<std::uint64_t>(
                            producer * 1000 + i + 1
                        );

                    std::string jobType;

                    // First 25 jobs from every producer
                    // intentionally fail.
                    if (i < 25) {
                        jobType = "FAIL";
                    } else {
                        switch (i % 3) {
                            case 0:
                                jobType = "REPORT";
                                break;

                            case 1:
                                jobType = "EMAIL";
                                break;

                            default:
                                jobType = "CALCULATE";
                                break;
                        }
                    }

                    const bool accepted =
                        queue.push(
                            jobq::Job(
                                jobId,
                                jobType,
                                "stress-test-job"
                            )
                        );

                    if (accepted) {
                        submittedJobs.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    } else {
                        submissionFailures.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                }
            }
        );
    }

    for (auto& producer : producers) {
        producer.join();
    }


    pool.stop();

    const auto endTime =
        std::chrono::steady_clock::now();

    const auto elapsed =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(endTime - startTime);


    const int submitted =
        submittedJobs.load(
            std::memory_order_relaxed
        );

    const int completed =
        completedJobs.load(
            std::memory_order_relaxed
        );

    const int failed =
        failedJobs.load(
            std::memory_order_relaxed
        );

    const int processed =
        completed + failed;

    const int lost =
        submitted - processed;

    const double elapsedSeconds =
        elapsed.count() / 1000.0;

    const double throughput =
        elapsedSeconds > 0.0
            ? processed / elapsedSeconds
            : 0.0;

    std::cerr << "\n";
    std::cerr << "========== Stress Test Results ==========\n";

    std::cerr
        << "Expected jobs      : "
        << expectedJobs
        << '\n';

    std::cerr
        << "Submitted jobs      : "
        << submitted
        << '\n';

    std::cerr
        << "Submission failures : "
        << submissionFailures.load()
        << '\n';

    std::cerr
        << "Completed jobs      : "
        << completed
        << '\n';

    std::cerr
        << "Failed jobs         : "
        << failed
        << '\n';

    std::cerr
        << "Processed jobs      : "
        << processed
        << '\n';

    std::cerr
        << "Lost jobs           : "
        << lost
        << '\n';

    std::cerr
        << "Elapsed time        : "
        << elapsed.count()
        << " ms\n";

    std::cerr
        << "Throughput          : "
        << throughput
        << " jobs/sec\n";

    std::cerr
        << "==========================================\n";

    const bool passed =
        submitted == expectedJobs &&
        submissionFailures.load() == 0 &&
        completed == expectedSuccesses &&
        failed == expectedFailures &&
        processed == expectedJobs &&
        lost == 0;

    if (passed) {
        std::cerr
            << "Stress test: PASS\n";

        return 0;
    }

    std::cerr
        << "Stress test: FAIL\n";

    return 1;
}