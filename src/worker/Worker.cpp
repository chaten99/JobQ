#include "worker/Worker.h"

#include <exception>
#include <iostream>
#include <syncstream>

namespace jobq {

namespace {

void handleJobFailure(
    std::size_t workerId,
    JobQueue& queue,
    const std::shared_ptr<Job>& job,
    const char* errorMessage
) {
    try {
        job->markFailed();
    } catch (...) {
        // Keep the worker alive if a status transition fails.
    }

    const auto attemptsMade = job->getAttemptsMade();

    {
        std::osyncstream output(std::cout);

        output
            << "[Worker " << workerId << "] Job "
            << job->getId()
            << " failed on attempt "
            << attemptsMade
            << ": " << errorMessage
            << '\n';
    }

    if (!job->canRetry()) {
        std::osyncstream(std::cout)
            << "[Worker " << workerId << "] Job "
            << job->getId()
            << " exhausted its retry policy; final status: "
            << toString(job->getStatus())
            << '\n';

        return;
    }

    try {
        const auto retryDelay = job->getRetryDelay();

        if (!job->prepareForRetry()) {
            std::osyncstream(std::cout)
                << "[Worker " << workerId
                << "] Retry unavailable for Job "
                << job->getId()
                << '\n';

            return;
        }

        if (queue.pushRetry(job, retryDelay)) {
            std::osyncstream(std::cout)
                << "[Worker " << workerId
                << "] Retry scheduled for Job "
                << job->getId()
                << " after "
                << retryDelay.count()
                << " ms"
                << " (next attempt "
                << attemptsMade + 1
                << ", priority "
                << toString(job->getPriority())
                << ")\n";
        } else {
            std::osyncstream(std::cerr)
                << "[Worker " << workerId
                << "] Could not enqueue retry for Job "
                << job->getId()
                << '\n';
        }
    } catch (const std::exception& exception) {
        std::osyncstream(std::cerr)
            << "[Worker " << workerId
            << "] Error while scheduling retry for Job "
            << job->getId()
            << ": " << exception.what()
            << '\n';
    } catch (...) {
        std::osyncstream(std::cerr)
            << "[Worker " << workerId
            << "] Unknown error while scheduling retry for Job "
            << job->getId()
            << '\n';
    }
}

} // namespace

Worker::Worker(
    std::size_t id,
    JobQueue& queue,
    JobExecutor& executor
)
    : id_(id),
      queue_(queue),
      executor_(executor) {}

void Worker::start() {
    if (thread_.joinable()) {
        return;
    }

    thread_ = std::thread(&Worker::run, this);
}

void Worker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Worker::run() {
    while (true) {
        auto job = queue_.waitAndPop();

        if (!job) {
            break;
        }

        try {
            job->markRunning();

            std::osyncstream(std::cout)
                << "[Worker " << id_ << "] picked Job "
                << job->getId()
                << " (" << job->getType()
                << ", priority "
                << toString(job->getPriority())
                << ", attempt "
                << job->getAttemptsMade()
                << ")\n";

            executor_.execute(*job);

            job->markCompleted();

            std::osyncstream(std::cout)
                << "[Worker " << id_ << "] Job "
                << job->getId()
                << " completed after "
                << job->getAttemptsMade()
                << " attempt(s)\n";
        } catch (const std::exception& exception) {
            handleJobFailure(
                id_,
                queue_,
                job,
                exception.what()
            );
        } catch (...) {
            handleJobFailure(
                id_,
                queue_,
                job,
                "Unknown non-standard exception"
            );
        }
    }

    std::osyncstream(std::cout)
        << "[Worker " << id_ << "] stopped\n";
}

}