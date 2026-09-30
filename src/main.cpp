#include <iostream>

#include "job/Job.h"
#include "queue/JobQueue.h"

int main() {
    jobq::JobQueue queue;

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

    std::cout << "Queue size: "
              << queue.size()
              << '\n';

    auto firstJob = queue.tryPop();

    if (firstJob.has_value()) {
        std::cout << "Popped Job ID: "
                  << firstJob->getId()
                  << '\n';

        std::cout << "Type: "
                  << firstJob->getType()
                  << '\n';
    }

    std::cout << "Queue size after pop: "
              << queue.size()
              << '\n';

    auto secondJob = queue.tryPop();

    if (secondJob.has_value()) {
        std::cout << "Popped Job ID: "
                  << secondJob->getId()
                  << '\n';
    }

    std::cout << "Queue empty: "
              << std::boolalpha
              << queue.empty()
              << '\n';

    return 0;
}