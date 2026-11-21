#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>

#include "jobq/Engine.h"
#include "../helpers/TemporaryDirectory.h"
#include "../helpers/TestSupport.h"
#include "storage/JobRecord.h"

namespace {

using jobq_test::check;
using namespace std::chrono_literals;

void testLiveSnapshots() {
    jobq_test::Counter entered;
    jobq_test::Gate release;
    jobq::Engine engine({.workerCount = 1});
    check(engine.listJobs().empty() && !engine.getJob(0) && !engine.getJob(999),
          "Empty and unknown lookups have no fabricated records");
    engine.registerHandler("BLOCK", [&](const jobq::Job&) {
        entered.add();
        if (!release.wait()) {
            throw std::runtime_error("Handler timed out");
        }
    });
    engine.registerHandler("FAIL", [](const jobq::Job&) { throw std::runtime_error("expected"); });
    check(jobq_test::throwsWith<std::logic_error>([&] { engine.submit("BLOCK", ""); }, "running"),
          "Submission before startup is rejected");
    engine.start();
    const auto active = engine.submit("BLOCK", "  preserved payload  ");
    check(entered.waitAtLeast(1), "Worker entered the registered handler");
    jobq::JobOptions options;
    options.priority = jobq::JobPriority::High;
    options.retryPolicy.maxRetries = 1;
    options.retryPolicy.initialDelay = 0ms;
    const auto waiting = engine.submit("FAIL", "", options);
    const auto saved = engine.getJob(active);
    check(saved && saved->id == active && saved->type == "BLOCK" &&
          saved->payload == "  preserved payload  " && saved->status == jobq::JobStatus::Running &&
          saved->attemptsMade == 1, "Lookup returns consistent running state and intact payload");
    auto reader = std::async(std::launch::async, [&] {
        for (int index = 0; index < 100; ++index) {
            const auto jobs = engine.listJobs();
            if (jobs.size() != 2 || jobs[0].id != active || jobs[1].id != waiting ||
                jobs[0].status != jobq::JobStatus::Running ||
                jobs[1].status != jobq::JobStatus::Pending || jobs[1].attemptsMade != 0 ||
                jobs[1].priority != jobq::JobPriority::High || jobs[1].retryPolicy.maxRetries != 1) {
                return false;
            }
        }
        return true;
    });
    check(reader.wait_for(5s) == std::future_status::ready && reader.get(),
          "Concurrent listing preserves membership, order, options, and per-job state");
    auto stopping = std::async(std::launch::async, [&] { engine.stop(); });
    check(jobq_test::waitUntil([&] { return engine.snapshot().state == jobq::EngineState::Stopping; }),
          "Shutdown waits for the active handler");
    check(jobq_test::throwsWith<std::logic_error>([&] { engine.submit("BLOCK", ""); }, "running") &&
          engine.getJob(waiting)->status == jobq::JobStatus::Pending,
          "Shutdown rejects submissions and still permits read-only lookup");
    release.open();
    stopping.get();
    const auto jobs = engine.listJobs();
    check(jobs.size() == 2 && jobs[0].status == jobq::JobStatus::Completed &&
          jobs[1].status == jobq::JobStatus::Failed && jobs[1].attemptsMade == 2 &&
          saved->status == jobq::JobStatus::Running,
          "Final snapshots show completed and retried failed jobs; prior value copies remain unchanged");
    auto copy = engine.getJob(active);
    copy->payload = "changed locally";
    check(engine.getJob(active)->payload == "  preserved payload  ",
          "Returned values cannot mutate the Engine's jobs");
}

void testPersistenceAndRecovery() {
    jobq_test::TemporaryDirectory directory("lookup");
    jobq::JobId submitted = 0;
    {
        jobq::Engine engine({.workerCount = 1, .storagePath = directory.journal()});
        engine.registerHandler("TASK", [](const jobq::Job&) {});
        engine.start();
        submitted = engine.submit("TASK", "persisted payload");
        engine.stop();
        check(engine.getJob(submitted)->status == jobq::JobStatus::Completed,
              "Persistent submission is observable through the public API");
    }
    {
        jobq::Engine recovered({.workerCount = 1, .storagePath = directory.journal()});
        const auto jobs = recovered.listJobs();
        check(jobs.size() == 1 && jobs[0].id == submitted && jobs[0].payload == "persisted payload" &&
              jobs[0].status == jobq::JobStatus::Completed && jobs[0].attemptsMade == 1,
              "Recovery lists one latest record despite multiple journal transitions");
        recovered.stop();
    }
    jobq::JobRecord retry;
    retry.id = submitted + 10;
    retry.type = "TASK";
    retry.status = jobq::JobStatus::Running;
    retry.attemptsMade = 1;
    retry.retryPolicy.maxRetries = 1;
    retry.retryPolicy.initialDelay = 1h;
    retry.retryPolicy.maxDelay = 1h;
    {
        std::ofstream journal(directory.journal(), std::ios::binary | std::ios::app);
        journal << jobq::serializeJobRecord(retry) << '\n';
    }
    {
        jobq::Engine recovered({.workerCount = 1, .storagePath = directory.journal()});
        const auto job = recovered.getJob(retry.id);
        check(recovered.listJobs().size() == 2 && job && job->status == jobq::JobStatus::Pending &&
              job->attemptsMade == 1 && recovered.snapshot().delayedJobs == 1,
              "Recovered interrupted retry is Pending with its real attempt policy and delayed queue entry");
        recovered.stop();
    }
    {
        std::ofstream journal(directory.journal(), std::ios::binary | std::ios::app);
        journal << "corrupted complete record\n";
    }
    check(jobq_test::throwsWith<std::runtime_error>([&] {
        jobq::Engine corrupted({.workerCount = 1, .storagePath = directory.journal()});
    }, ""), "Corrupted completed journal records fail recovery rather than inventing jobs");
}

}

int main() {
    try {
        testLiveSnapshots();
        testPersistenceAndRecovery();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
