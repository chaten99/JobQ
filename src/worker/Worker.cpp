#include "worker/Worker.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
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

Worker::~Worker() {
    try {
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id()) {
                thread_.detach();
            } else {
                thread_.join();
            }
        }
    } catch (...) {
    }
}

void Worker::start() {
    std::lock_guard<std::mutex> lock(threadMutex_);

    if (state_ != State::Created) {
        throw std::logic_error(
            state_ == State::Running
                ? "Worker has already been started"
                : "Worker has been joined and cannot be restarted"
        );
    }

    thread_ = std::thread(&Worker::run, this);
    state_ = State::Running;
}

void Worker::join() {
    if (isCurrentThread()) {
        throw std::logic_error("Worker cannot join its own thread");
    }

    std::lock_guard<std::mutex> lock(threadMutex_);

    if (thread_.joinable()) {
        thread_.join();
    }

    state_ = State::Joined;
}

bool Worker::isCurrentThread() const {
    return threadId_.load() == std::this_thread::get_id();
}

void Worker::run() {
    threadId_.store(std::this_thread::get_id());

    try {
        while (true) {
            std::shared_ptr<Job> job;

            try {
                job = queue_.waitAndPop();
            } catch (const std::exception& exception) {
                std::osyncstream(std::cerr)
                    << "[Worker " << id_
                    << "] Queue wait failed, worker exiting: "
                    << exception.what() << '\n';
                break;
            } catch (...) {
                std::osyncstream(std::cerr)
                    << "[Worker " << id_
                    << "] Queue wait failed with unknown error, "
                       "worker exiting\n";
                break;
            }

            if (!job) {
                break;
            }

            try {
                processJob(job);
            } catch (const std::exception& exception) {
                std::osyncstream(std::cerr)
                    << "[Worker " << id_
                    << "] Unexpected error while processing Job "
                    << job->getId() << ": " << exception.what() << '\n';
            } catch (...) {
                std::osyncstream(std::cerr)
                    << "[Worker " << id_
                    << "] Unexpected unknown error while processing Job "
                    << job->getId() << '\n';
            }
        }

        std::osyncstream(std::cout)
            << "[Worker " << id_ << "] stopped\n";
    } catch (...) {
    }
}

void Worker::processJob(const std::shared_ptr<Job>& job) {
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

        handleJobFailure(id_, queue_, job, metrics_, exception.what());
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

} // namespace jobq
