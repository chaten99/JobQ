#pragma once

#include "jobq/backend/Backend.h"
#include "../helpers/TestSupport.h"

#include <algorithm>
#include <array>
#include <future>
#include <latch>
#include <set>

namespace jobq::backend::test {

template<class Fixture>
void verifyContract() {
    using namespace std::chrono_literals;
    using jobq_test::check;
    Fixture fixture;
    Backend& backend = fixture;
    auto submit = [&](JobPriority priority = JobPriority::Normal) {
        return std::get<JobId>(backend.submit({"WORK", "payload", priority, {.maxRetries = 1}}));
    };
    auto owner = [&](const char* id, Duration ttl = 100ms) {
        return std::get<WorkerLease>(backend.registerWorker({id}, ttl));
    };
    auto take = [&](const WorkerLease& worker) {
        return std::get<std::optional<Claim>>(backend.claim(worker, 10ms));
    };
    auto snapshot = [&] { return std::get<Snapshot>(backend.snapshot()); };
    auto record = [&](JobId id) { return std::get<JobRecord>(backend.get(id)); };
    auto ok = [](const Outcome& outcome) { return std::holds_alternative<std::monostate>(outcome); };

    const auto a = owner("process-a/worker-1");
    const auto b = owner("process-b/worker-1");
    check(snapshot().workers.size() == 2, "Independent workers register");
    check(std::get<Error>(backend.registerWorker(a.worker, 1ms)) == Error::Conflict, "Active identities are unique");
    const auto low = submit(JobPriority::Low);
    const auto high1 = submit(JobPriority::High);
    const auto high2 = submit(JobPriority::High);
    const auto normal = submit();
    check(snapshot().readyJobs == 4 && record(low).status == JobStatus::Pending, "Submission persists and queues together");
    auto first = take(a).value();
    auto second = take(b).value();
    check(first.record.id == high1.value && second.record.id == high2.value &&
          first.record.payload == "payload" && first.record.status == JobStatus::Running &&
          first.record.attemptsMade == 1 && first.lease.token.value != second.lease.token.value,
          "Priority, FIFO and actual claim records");
    auto stale = first.lease;
    stale.token = {"wrong"};
    check(std::get<Error>(backend.renew(stale, 20ms)) == Error::OwnershipLost &&
          std::get<Error>(backend.complete(stale)) == Error::OwnershipLost,
          "Stale tokens cannot renew or complete");
    check(ok(backend.renew(first.lease, 20ms)) && record(high1).status == JobStatus::Running,
          "Renewal preserves logical state");
    fixture.inject(Error::Unavailable);
    check(std::get<Error>(backend.fail(first.lease, RetryAfter{5ms})) == Error::Unavailable &&
          std::get<Error>(backend.submit({"WORK", ""})) == Error::Unavailable,
          "Unavailable failures propagate");
    fixture.inject(Error::Timeout);
    check(std::get<Error>(backend.get(low)) == Error::Timeout, "Timeout differs from missing records");
    fixture.inject({});
    check(snapshot().readyJobs == 2 && snapshot().delayedJobs == 0 &&
          record(high1).status == JobStatus::Running &&
          std::get<std::vector<JobRecord>>(backend.list()).size() == 4, "Rejected operations make no partial changes");
    check(ok(backend.fail(first.lease, RetryAfter{5ms})) && ok(backend.fail(second.lease, RetryAfter{5ms})) &&
          snapshot().delayedJobs == 2 && record(high1).status == JobStatus::Pending, "Retry moves record and queue together");
    auto normalClaim = take(a).value();
    auto lowClaim = take(b).value();
    check(normalClaim.record.id == normal.value && lowClaim.record.id == low.value && !take(a),
          "Delayed work cannot be claimed before due");
    check(ok(backend.complete(normalClaim.lease)) && ok(backend.fail(lowClaim.lease, FinalFailure{})) &&
          record(normal).status == JobStatus::Completed && record(low).status == JobStatus::Failed,
          "Final outcomes update records and release leases");
    fixture.advance(5ms);
    const auto retry1 = take(a).value();
    const auto retry2 = take(b).value();
    check(retry1.record.id == high1.value && retry2.record.id == high2.value &&
          retry1.record.attemptsMade == 2 && retry1.lease.generation.value > first.lease.generation.value,
          "Due retries preserve deterministic order and advance the generation");
    check(ok(backend.complete(retry2.lease)), "Retry owner can complete");
    fixture.advance(10ms);
    check(std::get<Error>(backend.renew(retry1.lease, 1ms)) == Error::LeaseExpired &&
          std::get<Error>(backend.complete(retry1.lease)) == Error::LeaseExpired &&
          std::get<std::uint64_t>(backend.recoverExpiredClaims()) == 1, "Expired claim cannot commit and is recovered");
    auto reassigned = take(b).value();
    check(reassigned.record.id == high1.value && reassigned.lease.generation.value > retry1.lease.generation.value &&
          std::get<Error>(backend.complete(retry1.lease)) == Error::OwnershipLost &&
          ok(backend.complete(reassigned.lease)), "Reassignment fences the old owner");

    check(ok(backend.heartbeat(a, 20ms)), "Heartbeat renews the matching worker");
    const auto abandoned = submit();
    const auto abandonedClaim = take(a).value();
    check(ok(backend.unregisterWorker(a)) && std::get<Error>(backend.heartbeat(a, 5ms)) == Error::NotFound &&
          snapshot().workers.size() == 1 && ok(backend.heartbeat(b, 100ms)), "Unregistration leaves other owners intact");
    check(std::get<Error>(backend.complete(abandonedClaim.lease)) == Error::OwnershipLost &&
          std::get<std::uint64_t>(backend.recoverExpiredClaims()) == 1, "Unregistration invalidates job ownership");
    const auto only = take(b).value();
    check(only.record.id == abandoned.value && ok(backend.complete(only.lease)), "Abandoned work returns to ready queue");
    const auto shortLived = owner("short-lived", 1ms);
    const auto orphan = submit();
    const auto orphanClaim = take(shortLived).value();
    check(ok(backend.heartbeat(shortLived, 3ms)), "Heartbeat changes the current worker deadline");
    fixture.advance(1ms);
    check(ok(backend.renew(orphanClaim.lease, 10ms)), "Renewed registration stays valid past its original deadline");
    fixture.advance(2ms);
    const auto workers = snapshot().workers;
    check(std::ranges::any_of(workers, [](const auto& worker) {
        return worker.worker.value == "short-lived" && worker.state == WorkerState::Expired;
    }) && std::get<Error>(backend.heartbeat(shortLived, 1ms)) == Error::LeaseExpired, "Expired workers are observable");
    check(std::get<Error>(backend.complete(orphanClaim.lease)) == Error::LeaseExpired &&
          std::get<std::uint64_t>(backend.recoverExpiredClaims()) == 1,
          "Job renewal cannot keep an expired worker registration alive");
    const auto replacement = owner("short-lived");
    check(replacement.token.value != shortLived.token.value &&
          std::get<Error>(backend.unregisterWorker(shortLived)) == Error::OwnershipLost,
          "A replaced worker token cannot unregister the current worker");
    const auto recovered = take(replacement).value();
    check(recovered.record.id == orphan.value && ok(backend.complete(recovered.lease)),
          "Recovered worker-owned job can be completed");
    check(std::get<Error>(backend.get({999999})) == Error::NotFound &&
          std::get<Error>(backend.submit({})) == Error::InvalidInput &&
          std::get<Error>(backend.claim(b, 0ms)) == Error::InvalidInput, "Explicit missing and invalid outcomes");

    submit();
    const auto c = owner("process-c/worker-1");
    std::latch claimStart{2};
    auto raceClaim = [&](const WorkerLease& worker) {
        claimStart.arrive_and_wait();
        return take(worker);
    };
    auto left = std::async(std::launch::async, raceClaim, b);
    auto right = std::async(std::launch::async, raceClaim, c);
    auto one = left.get();
    auto two = right.get();
    check(one.has_value() != two.has_value(), "Concurrent claims cannot duplicate ownership");

    std::latch producerStart{4};
    std::array<std::future<std::vector<std::uint64_t>>, 4> producers;
    for (auto& producer : producers) {
        producer = std::async(std::launch::async, [&] {
            producerStart.arrive_and_wait();
            std::vector<std::uint64_t> ids;
            for (int i = 0; i < 20; ++i) ids.push_back(submit().value);
            return ids;
        });
    }
    std::set<std::uint64_t> ids;
    for (auto& producer : producers) {
        const auto allocated = producer.get();
        ids.insert(allocated.begin(), allocated.end());
    }
    check(ids.size() == 80 && snapshot().readyJobs == 80, "Concurrent producers allocate globally unique ready jobs within the backend");
}

}
