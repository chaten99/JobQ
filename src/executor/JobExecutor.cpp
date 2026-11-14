#include "executor/JobExecutor.h"

#include <stdexcept>
#include <utility>

#include "metrics/MeasuredLock.h"

namespace jobq {

void JobExecutor::setContentionMetrics(ContentionMetrics* metrics) noexcept {
    contention_ = metrics;
}

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

    MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobExecutor);

    handlers_[std::move(type)] = std::move(handler);
}

void JobExecutor::execute(const Job& job) const {
    JobHandler handler;

    {
        MeasuredMutexLock lock(mutex_, contention_, LockCategory::JobExecutor);

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