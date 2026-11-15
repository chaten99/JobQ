#include "jobq/Engine.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "executor/JobExecutor.h"
#include "manager/JobManager.h"
#include "queue/JobQueue.h"
#include "storage/FileJobStorage.h"
#include "worker/WorkerPool.h"

namespace jobq {

namespace {

thread_local const Engine* executingEngine = nullptr;

class HandlerScope {
public:
    explicit HandlerScope(const Engine* engine)
        : previous_(executingEngine) {
        executingEngine = engine;
    }

    ~HandlerScope() {
        executingEngine = previous_;
    }

private:
    const Engine* previous_;
};

EngineConfig validateConfig(EngineConfig config) {
    if (config.workerCount == 0) {
        throw std::invalid_argument("Worker count must be greater than zero");
    }
    return config;
}

}

struct Engine::Impl {
    enum class State { Created, Starting, Running, Stopping, Stopped };

    explicit Impl(EngineConfig config)
        : pool(queue, executor, config.workerCount, metrics) {
        if (config.storagePath.empty()) {
            manager.emplace(queue, metrics);
        } else {
            manager.emplace(
                queue,
                std::make_shared<FileJobStorage>(std::move(config.storagePath)),
                metrics
            );
        }
    }

    void finishTransition(State next) {
        {
            std::lock_guard lock(mutex);
            state = next;
        }
        changed.notify_all();
    }

    JobQueue queue;
    JobMetrics metrics;
    JobExecutor executor;
    std::optional<JobManager> manager;
    WorkerPool pool;
    std::unordered_set<std::string> handlerTypes;
    mutable std::mutex mutex;
    std::condition_variable changed;
    State state{State::Created};
};

Engine::Engine(EngineConfig config)
    : impl_(std::make_unique<Impl>(validateConfig(std::move(config)))) {
}

Engine::~Engine() noexcept {
    stop();
}

void Engine::registerHandler(std::string type, JobHandler handler) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != Impl::State::Created) {
        throw std::logic_error("Handlers must be registered before start");
    }
    if (type.empty() || !handler) {
        throw std::invalid_argument("Handler type and callable must be non-empty");
    }

    const auto [entry, inserted] = impl_->handlerTypes.insert(type);
    if (!inserted) {
        throw std::invalid_argument("Handler already registered for job type: " + type);
    }

    try {
        impl_->executor.registerHandler(
            type,
            [this, handler = std::move(handler)](const Job& job) {
                HandlerScope scope(this);
                handler(job);
            }
        );
    } catch (...) {
        impl_->handlerTypes.erase(entry);
        throw;
    }
}

void Engine::start() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->state != Impl::State::Created) {
            throw std::logic_error("Engine can only be started once");
        }
        impl_->state = Impl::State::Starting;
    }

    try {
        impl_->pool.start();
    } catch (...) {
        impl_->finishTransition(Impl::State::Stopped);
        throw;
    }
    impl_->finishTransition(Impl::State::Running);
}

void Engine::stop() {
    if (executingEngine == this) {
        throw std::logic_error("Engine cannot be stopped from its own handler");
    }

    {
        std::unique_lock lock(impl_->mutex);
        impl_->changed.wait(lock, [this] {
            return impl_->state != Impl::State::Starting;
        });
        if (impl_->state == Impl::State::Stopping) {
            impl_->changed.wait(lock, [this] {
                return impl_->state == Impl::State::Stopped;
            });
        }
        if (impl_->state == Impl::State::Stopped) {
            return;
        }
        impl_->state = Impl::State::Stopping;
    }

    try {
        impl_->pool.stop();
    } catch (...) {
        impl_->finishTransition(Impl::State::Stopped);
        throw;
    }
    impl_->finishTransition(Impl::State::Stopped);
}

JobId Engine::submit(std::string type, std::string payload, JobOptions options) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != Impl::State::Running) {
        throw std::logic_error("Jobs can only be submitted to a running engine");
    }
    if (!impl_->handlerTypes.contains(type)) {
        throw std::invalid_argument("No handler registered for job type: " + type);
    }
    return impl_->manager->submit(
        std::move(type), std::move(payload), options.priority, options.retryPolicy
    )->getId();
}

std::optional<JobStatus> Engine::getStatus(JobId id) const {
    return impl_->manager->getStatus(id);
}

JobMetricsSnapshot Engine::metrics() const {
    return impl_->metrics.snapshot();
}

bool Engine::isRunning() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->state == Impl::State::Running;
}

}
