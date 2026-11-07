#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "job/Job.h"
#include "queue/JobQueue.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto kTimeout = 10s;

int testsRun = 0;
int testsFailed = 0;

void check(bool condition, const std::string& description) {
    ++testsRun;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

std::shared_ptr<jobq::Job> makeJob(
    std::uint64_t id,
    jobq::JobPriority priority = jobq::JobPriority::Normal
) {
    return std::make_shared<jobq::Job>(id, "T", "p", priority);
}

std::vector<std::uint64_t> drainIds(jobq::JobQueue& queue) {
    std::vector<std::uint64_t> ids;
    while (auto job = queue.tryPop()) {
        ids.push_back(job->getId());
    }
    return ids;
}

bool allUnique(const std::vector<std::uint64_t>& ids) {
    return std::set<std::uint64_t>(ids.begin(), ids.end()).size() ==
           ids.size();
}

class Counter {
public:
    void add(std::size_t n = 1) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_ += n;
        }
        cv_.notify_all();
    }

    bool waitAtLeast(std::size_t target) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(
            lock, kTimeout, [&] { return value_ >= target; }
        );
    }

    std::size_t value() {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t value_{0};
};

constexpr std::size_t kProducers = 4;
constexpr std::size_t kPerProducer = 250;
constexpr std::size_t kTotal = kProducers * kPerProducer;

std::uint64_t idFor(std::size_t producer, std::size_t index) {
    return producer * kPerProducer + index + 1;
}

void testConcurrentProducers() {
    std::cout << "\n--- Test A: Multiple Concurrent Producers ---\n";

    jobq::JobQueue queue;
    std::latch start(kProducers);
    std::atomic<std::size_t> accepted{0};
    std::vector<std::thread> threads;

    for (std::size_t p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                if (queue.push(makeJob(idFor(p, i)))) {
                    accepted.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    check(accepted.load() == kTotal, "All 1000 submissions accepted");
    check(queue.size() == kTotal, "Queue size matches accepted submissions");

    const auto ids = drainIds(queue);
    check(ids.size() == kTotal, "Every accepted job is present");
    check(allUnique(ids), "No duplicate job appears");
    check(queue.empty(), "Queue empty after draining");
}

void testConcurrentConsumers() {
    std::cout << "\n--- Test B: Multiple Concurrent Consumers ---\n";

    constexpr std::size_t kConsumers = 4;
    jobq::JobQueue queue;
    Counter consumed;
    std::mutex idsMutex;
    std::vector<std::uint64_t> ids;
    std::vector<std::thread> threads;

    for (std::size_t c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            while (auto job = queue.waitAndPop()) {
                {
                    std::lock_guard<std::mutex> lock(idsMutex);
                    ids.push_back(job->getId());
                }
                consumed.add();
            }
        });
    }

    for (std::size_t i = 0; i < kTotal; ++i) {
        queue.push(makeJob(i + 1));
    }

    const bool all = consumed.waitAtLeast(kTotal);
    queue.close();
    for (auto& t : threads) {
        t.join();
    }

    check(all, "Consumers retrieved every accepted job in time");
    check(ids.size() == kTotal, "Each accepted job retrieved once");
    check(allUnique(ids), "No duplicate IDs consumed");
    check(queue.empty(), "No accepted jobs left behind");
    check(true, "All consumers terminated after close");
}

void testProducersAndConsumers() {
    std::cout << "\n--- Test C: Concurrent Producers and Consumers ---\n";

    constexpr std::size_t kConsumers = 4;
    jobq::JobQueue queue;
    std::atomic<std::size_t> accepted{0};
    std::mutex idsMutex;
    std::vector<std::uint64_t> ids;
    std::latch start(kProducers + kConsumers);
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (std::size_t c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            start.arrive_and_wait();
            while (auto job = queue.waitAndPop()) {
                std::lock_guard<std::mutex> lock(idsMutex);
                ids.push_back(job->getId());
            }
        });
    }

    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                const auto job = makeJob(idFor(p, i));
                const bool ok = (i % 5 == 0)
                    ? queue.pushRetry(job, std::chrono::milliseconds(i % 3))
                    : queue.push(job);
                if (ok) {
                    accepted.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }
    queue.close();
    for (auto& t : consumers) {
        t.join();
    }

    const std::size_t remaining = queue.size();
    check(accepted.load() == kTotal, "All submissions accepted");
    check(accepted.load() == ids.size() + remaining,
          "Accepted = consumed + remaining");
    check(remaining == 0, "Closed queue fully drained, including retries");
    check(allUnique(ids), "No job consumed twice");
}

void testPushRejectedAfterClose() {
    std::cout << "\n--- Test D: Push Rejected After Closure ---\n";

    jobq::JobQueue queue;
    queue.push(makeJob(1));
    queue.close();

    check(!queue.push(makeJob(2)), "push() rejected after close");
    check(!queue.push(nullptr), "null push rejected");
    check(!queue.pushRetry(nullptr, 0ms), "null retry rejected");
    check(queue.size() == 1, "Queue contents unchanged by rejections");
    check(queue.isClosed(), "Queue stays closed");

    check(queue.pushRetry(makeJob(3), 0ms),
          "Internal retry still accepted after close (9.1 drain policy)");
    const auto ids = drainIds(queue);
    check(ids.size() == 2, "Accepted jobs remain retrievable after close");
}

void testMultipleClose() {
    std::cout << "\n--- Test E: Multiple Close Calls ---\n";

    jobq::JobQueue queue;
    queue.push(makeJob(1));
    queue.close();
    queue.close();
    queue.close();

    check(queue.isClosed(), "Queue closed after repeated close()");
    check(queue.size() == 1, "Repeated close() does not drop queued jobs");
    auto job = queue.waitAndPop();
    check(job && job->getId() == 1, "Queued job still delivered");
    check(queue.waitAndPop() == nullptr, "Drained closed queue returns null");
}

void testWaitersAwakenedByClose() {
    std::cout << "\n--- Test F: Waiting Consumers Awakened By Close ---\n";

    constexpr std::size_t kWaiters = 4;
    jobq::JobQueue queue;
    Counter entered;
    std::vector<std::promise<bool>> promises(kWaiters);
    std::vector<std::future<bool>> futures;
    std::vector<std::thread> threads;

    for (auto& p : promises) {
        futures.push_back(p.get_future());
    }
    for (std::size_t i = 0; i < kWaiters; ++i) {
        threads.emplace_back([&, i] {
            entered.add();
            promises[i].set_value(queue.waitAndPop() == nullptr);
        });
    }

    entered.waitAtLeast(kWaiters);
    queue.close();

    bool allWoke = true;
    bool allNull = true;
    for (auto& f : futures) {
        if (f.wait_for(kTimeout) != std::future_status::ready) {
            allWoke = false;
        } else if (!f.get()) {
            allNull = false;
        }
    }
    queue.close();
    for (auto& t : threads) {
        t.join();
    }

    check(allWoke, "Every waiting consumer woke after close");
    check(allNull, "Waiters returned null from an empty closed queue");
}

void testPriorityOrdering() {
    std::cout << "\n--- Test G: Priority Ordering ---\n";

    using jobq::JobPriority;
    jobq::JobQueue queue;
    queue.push(makeJob(1, JobPriority::Low));
    queue.push(makeJob(2, JobPriority::Normal));
    queue.push(makeJob(3, JobPriority::High));
    queue.push(makeJob(4, JobPriority::Low));
    queue.push(makeJob(5, JobPriority::High));
    queue.push(makeJob(6, JobPriority::Normal));

    const auto ids = drainIds(queue);
    check((ids == std::vector<std::uint64_t>{3, 5, 2, 6, 1, 4}),
          "Higher priority first, FIFO within equal priority");

    for (std::uint64_t i = 1; i <= 50; ++i) {
        queue.push(makeJob(i, JobPriority::Normal));
    }
    const auto fifo = drainIds(queue);
    bool ordered = fifo.size() == 50;
    for (std::size_t i = 0; i < fifo.size(); ++i) {
        ordered = ordered && fifo[i] == i + 1;
    }
    check(ordered, "50 equal-priority jobs preserve FIFO order");
}

void testDelayedRetryTiming() {
    std::cout << "\n--- Test H: Delayed Retry Timing ---\n";

    using jobq::JobPriority;
    {
        jobq::JobQueue queue;
        const auto start = Clock::now();
        queue.pushRetry(makeJob(1), 200ms);
        queue.pushRetry(makeJob(2), 100ms);

        check(queue.tryPop() == nullptr,
              "Retries unavailable before their ready time");
        check(queue.size() == 2 && !queue.empty(),
              "Pending retries counted in size()");

        auto first = queue.waitAndPop();
        const auto firstAt = Clock::now() - start;
        check(first && first->getId() == 2, "Earliest retry delivered first");
        check(firstAt >= 100ms, "Retry not delivered before its ready time");

        auto second = queue.waitAndPop();
        const auto secondAt = Clock::now() - start;
        check(second && second->getId() == 1, "Later retry delivered second");
        check(secondAt >= 200ms, "Second retry respects its ready time");
    }

    {
        jobq::JobQueue queue;
        queue.pushRetry(makeJob(1, JobPriority::Low), 100ms);
        queue.pushRetry(makeJob(2, JobPriority::High), 100ms);
        queue.pushRetry(makeJob(3, JobPriority::Normal), 100ms);
        check(queue.tryPop() == nullptr, "Retries not yet due");

        std::this_thread::sleep_for(250ms);

        const auto ids = drainIds(queue);
        check((ids == std::vector<std::uint64_t>{2, 3, 1}),
              "Promoted retries obey priority ordering");
        check(queue.empty(), "No duplicates after promotion");
    }

    {
        jobq::JobQueue queue;
        queue.pushRetry(makeJob(1), 0ms);
        queue.pushRetry(makeJob(2), -50ms);
        const auto ids = drainIds(queue);
        check((ids == std::vector<std::uint64_t>{1, 2}),
              "Zero and negative delays are immediately available in order");
    }
}

void testImmediateAndDelayed() {
    std::cout << "\n--- Test I: Immediate Jobs and Delayed Retries ---\n";

    {
        jobq::JobQueue queue;
        queue.pushRetry(makeJob(100), 1000ms);
        queue.push(makeJob(1));

        const auto start = Clock::now();
        auto job = queue.waitAndPop();
        const auto elapsed = Clock::now() - start;

        check(job && job->getId() == 1, "Immediate job returned first");
        check(elapsed < 500ms,
              "Immediate job not delayed by a future retry");
        check(queue.size() == 1, "Future retry still pending");
    }

    {
        jobq::JobQueue queue;
        queue.pushRetry(makeJob(100), 5000ms);

        Counter entered;
        std::promise<std::pair<std::uint64_t, Clock::duration>> result;
        auto future = result.get_future();
        const auto start = Clock::now();
        std::thread consumer([&] {
            entered.add();
            auto job = queue.waitAndPop();
            result.set_value({
                job ? job->getId() : 0,
                Clock::now() - start
            });
        });

        entered.waitAtLeast(1);
        queue.pushRetry(makeJob(200), 50ms);

        const bool ready = future.wait_for(kTimeout) ==
                           std::future_status::ready;
        std::pair<std::uint64_t, Clock::duration> value{0, 0s};
        if (ready) {
            value = future.get();
        }
        queue.close();
        consumer.join();

        check(ready && value.first == 200,
              "Sleeping consumer re-targets an earlier retry");
        check(value.second < 3s,
              "Consumer woke for the earlier retry, not the 5s one");
    }
}

void testConcurrentPushAndClose() {
    std::cout << "\n--- Test J: Concurrent Push and Close ---\n";

    for (int round = 0; round < 20; ++round) {
        jobq::JobQueue queue;
        Counter progress;
        std::latch start(kProducers + 1);
        std::mutex acceptedMutex;
        std::set<std::uint64_t> acceptedIds;
        std::atomic<std::size_t> rejected{0};
        std::atomic<std::size_t> attempted{0};
        std::vector<std::thread> producers;

        for (std::size_t p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                start.arrive_and_wait();
                for (std::size_t i = 0; i < kPerProducer; ++i) {
                    const auto id = idFor(p, i);
                    attempted.fetch_add(1);
                    if (queue.push(makeJob(id))) {
                        std::lock_guard<std::mutex> lock(acceptedMutex);
                        acceptedIds.insert(id);
                    } else {
                        rejected.fetch_add(1);
                    }
                    progress.add();
                }
            });
        }

        std::thread closer([&] {
            start.arrive_and_wait();
            progress.waitAtLeast(50);
            queue.close();
        });

        for (auto& t : producers) {
            t.join();
        }
        closer.join();

        const auto remaining = queue.size();
        const auto ids = drainIds(queue);
        const std::set<std::uint64_t> drained(ids.begin(), ids.end());

        const bool consistent =
            attempted.load() == kTotal &&
            acceptedIds.size() + rejected.load() == kTotal &&
            remaining == acceptedIds.size() &&
            drained == acceptedIds &&
            allUnique(ids);

        if (!consistent) {
            check(false, "Push/close round was inconsistent");
            return;
        }
    }

    check(true,
          "20 rounds: accepted + rejected = attempted; accepted jobs all "
          "retrievable; rejected jobs never inserted");
}

void testEmptySizeConsistency() {
    std::cout << "\n--- Test K: Empty and Size Consistency ---\n";

    jobq::JobQueue queue;
    check(queue.empty() && queue.size() == 0 && !queue.isClosed(),
          "New queue: empty, size 0, open");

    queue.push(makeJob(1));
    check(!queue.empty() && queue.size() == 1, "Immediate job counted");

    queue.pushRetry(makeJob(2), 100ms);
    check(queue.size() == 2, "Delayed retry counted in size()");

    auto first = queue.tryPop();
    check(first && first->getId() == 1 && queue.size() == 1,
          "Immediate job popped, retry remains");
    check(!queue.empty(), "Queue with only a delayed retry is not empty");

    std::this_thread::sleep_for(250ms);
    check(queue.size() == 1, "Promotion does not change the total count");

    queue.close();
    check(queue.isClosed() && queue.size() == 1 && !queue.empty(),
          "Closed queue keeps accepted work");

    auto retry = queue.tryPop();
    check(retry && retry->getId() == 2, "Promoted retry retrieved");
    check(queue.empty() && queue.size() == 0, "Drained closed queue is empty");

    jobq::JobQueue shared;
    std::atomic<bool> stop{false};
    std::vector<std::thread> observers;
    for (int i = 0; i < 3; ++i) {
        observers.emplace_back([&] {
            while (!stop.load()) {
                (void)shared.size();
                (void)shared.empty();
                (void)shared.isClosed();
            }
        });
    }
    for (std::uint64_t i = 1; i <= 500; ++i) {
        shared.push(makeJob(i));
        if (i % 2 == 0) {
            shared.tryPop();
        }
    }
    stop.store(true);
    for (auto& t : observers) {
        t.join();
    }
    check(shared.size() == 250,
          "size() stays correct while observed concurrently");
}

}

int main() {
    std::cout << "========================================\n";
    std::cout << "     JobQ Queue Reliability Tests\n";
    std::cout << "========================================\n";

    testConcurrentProducers();
    testConcurrentConsumers();
    testProducersAndConsumers();
    testPushRejectedAfterClose();
    testMultipleClose();
    testWaitersAwakenedByClose();
    testPriorityOrdering();
    testDelayedRetryTiming();
    testImmediateAndDelayed();
    testConcurrentPushAndClose();
    testEmptySizeConsistency();

    std::cout << "\nTests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';
    std::cout << (testsFailed == 0 ? "Result: ALL TESTS PASSED\n"
                                   : "Result: SOME TESTS FAILED\n");

    return testsFailed == 0 ? 0 : 1;
}
