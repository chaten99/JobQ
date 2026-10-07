#include "worker/Worker.h"

#include <iostream>
#include <stdexcept>
#include <syncstream>

namespace jobq {

Worker::Worker(
    std::size_t id,
    JobQueue& queue,
    JobExecutor& executor
)
    : id_(id),
      queue_(queue),
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

        if (!job) {
            break;
        }

        job->setStatus(JobStatus::Running);

        std::osyncstream(std::cout)
            << "[Worker "
            << id_
            << "] picked Job "
            << job->getId()
            << " ("
            << job->getType()
            << ")\n";

        try {
            executor_.execute(*job);

            job->setStatus(JobStatus::Completed);

            std::osyncstream(std::cout)
                << "[Worker "
                << id_
                << "] Job "
                << job->getId()
                << " completed\n";
        }
        catch (const std::exception& exception) {
            job->setStatus(JobStatus::Failed);

            std::osyncstream(std::cout)
                << "[Worker "
                << id_
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
                << id_
                << "] Job "
                << job->getId()
                << " failed: unknown error\n";
        }
    }

    std::osyncstream(std::cout)
        << "[Worker "
        << id_
        << "] stopped\n";
}

} // namespace jobq