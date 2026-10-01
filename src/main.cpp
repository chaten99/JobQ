#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "job/Job.h"
#include "queue/JobQueue.h"

int main() {
    jobq::JobQueue queue;

    constexpr int producerCount = 4;
    constexpr int jobsPerProducer = 100;

    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&queue, producer]() {
            for (int i = 0; i < jobsPerProducer; ++i) {
                const auto id =
                    static_cast<std::uint64_t>(producer * 1000 + i);

                queue.push(
                    jobq::Job(
                        id,
                        "TEST",
                        "producer-job"
                    )
                );
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    const auto expectedJobs =
        producerCount * jobsPerProducer;

    const auto actualJobs =
        queue.size();

    std::cout << "Expected jobs: "
              << expectedJobs
              << '\n';

    std::cout << "Jobs in queue: "
              << actualJobs
              << '\n';

    if (actualJobs == static_cast<std::size_t>(expectedJobs)) {
        std::cout << "Concurrency test: PASS\n";
    } else {
        std::cout << "Concurrency test: FAIL\n";
    }

    return 0;
}