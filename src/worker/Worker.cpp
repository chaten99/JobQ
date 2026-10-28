#include "worker/Worker.h"

#include <exception>
#include <iostream>
#include <syncstream>
#include "metrics/JobMetrics.h"

namespace jobq {

namespace {

void recordExecutionStartedSafely(JobMetrics* metrics) noexcept {
    if (!metrics) {
        return;
    }

    try {
        metrics->recordExecutionStarted();
    } catch (...) {
    }
}

void recordExecutionCompletedSafely(
    JobMetrics* metrics,
    JobMetrics::Clock::duration executionTime
) noexcept {
    if (!metrics) {
        return;
    }

    try {
        metrics->recordExecutionCompleted(executionTime);
    } catch (...) {
    }
}

void recordExecutionFailedSafely(
    JobMetrics* metrics,
    JobMetrics::Clock::duration executionTime
) noexcept {
    if (!metrics) {
        return;
    }

    try {
        metrics->recordExecutionFailed(executionTime);
    } catch (...) {
    }
}

void recordRetryScheduledSafely(JobMetrics* metrics) noexcept {
    if (!metrics) {
        return;
    }

    try {
        metrics->recordRetryScheduled();
    } catch (...) {
    }
}

void handleJobFailure(
    std::size_t workerId,
    JobQueue& queue,
    const std::shared_ptr<Job>& job,
    JobMetrics* metrics,
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
            recordRetryScheduledSafely(metrics);

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
    JobExecutor& executor,
    JobMetrics* metrics
)
    : id_(id),
      queue_(queue),
      executor_(executor),
      metrics_(metrics) {}

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

        bool executionStarted = false;
        JobMetrics::Clock::time_point executionStartedAt{};

        try {
            job->markRunning();

            executionStartedAt = JobMetrics::Clock::now();
            executionStarted = true;

            recordExecutionStartedSafely(metrics_);

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

            recordExecutionCompletedSafely(
                metrics_,
                JobMetrics::Clock::now() - executionStartedAt
            );

            std::osyncstream(std::cout)
                << "[Worker " << id_ << "] Job "
                << job->getId()
                << " completed after "
                << job->getAttemptsMade()
                << " attempt(s)\n";
        } catch (const std::exception& exception) {
            if (executionStarted) {
                recordExecutionFailedSafely(
                    metrics_,
                    JobMetrics::Clock::now() - executionStartedAt
                );
            }

            handleJobFailure(
                id_,
                queue_,
                job,
                metrics_,
                exception.what()
            );
        } catch (...) {
            if (executionStarted) {
                recordExecutionFailedSafely(
                    metrics_,
                    JobMetrics::Clock::now() - executionStartedAt
                );
            }

            handleJobFailure(
                id_,
                queue_,
                job,
                metrics_,
                "Unknown non-standard exception"
            );
        }
    }

    std::osyncstream(std::cout)
        << "[Worker " << id_ << "] stopped\n";
}

} // namespace jobq