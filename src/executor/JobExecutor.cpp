#include "executor/JobExecutor.h"

#include <stdexcept>
#include <utility>

namespace jobq {

void JobExecutor::registerHandler(
    std::string type,
    JobHandler handler
) {
    if (type.empty()) {
        throw std::invalid_argument(
            "Handler type cannot be empty"
        );
    }

    if (!handler) {
        throw std::invalid_argument(
            "Handler cannot be empty"
        );
    }

    std::lock_guard<std::mutex> lock(mutex_);

    handlers_[std::move(type)] = std::move(handler);
}

void JobExecutor::execute(const Job& job) const {
    JobHandler handler;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto it = handlers_.find(job.getType());

        if (it == handlers_.end()) {
            throw std::runtime_error(
                "No handler registered for job type: " +
                job.getType()
            );
        }

        handler = it->second;
    }

    handler(job);
}

} // namespace jobq