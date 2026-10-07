#include "worker/Worker.h"

#include <iostream>
#include <stdexcept>
#include <syncstream>

namespace jobq {

Worker::Worker(
    JobQueue& queue,
    JobExecutor& executor
)
    : queue_(queue),
      executor_(executor) {
}

void Worker::start() {
    if (thread_.joinable()) {
        throw std::logic_error(
            "Worker has already been started"
        );
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

        if (!job.has_value()) {
            break;
        }

        job->setStatus(JobStatus::Running);

        try {
            executor_.execute(*job);

            job->setStatus(JobStatus::Completed);

            std::osyncstream(std::cout)
                << "[Worker "
                << std::this_thread::get_id()
                << "] Job "
                << job->getId()
                << " completed\n";
        }
        catch (const std::exception& exception) {
            job->setStatus(JobStatus::Failed);

            std::osyncstream(std::cout)
                << "[Worker "
                << std::this_thread::get_id()
                << "] Job "
                << job->getId()
                << " failed: "
                << exception.what()
                << '\n';
        }
        catch (...) {
            job->setStatus(JobStatus::Failed);

            std::osyncstream(std::cout)
                << "[Worker "
                << std::this_thread::get_id()
                << "] Job "
                << job->getId()
                << " failed: unknown error\n";
        }
    }

    std::osyncstream(std::cout)
        << "[Worker "
        << std::this_thread::get_id()
        << "] stopped\n";
}

} // namespace jobq