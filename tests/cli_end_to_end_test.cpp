#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "cli/InteractiveRuntime.h"
#include "cli/MetricsCommands.h"
#include "helpers/TemporaryDirectory.h"
#include "helpers/TestSupport.h"

namespace {

using jobq_test::check;
using namespace std::chrono_literals;

void workflow(bool persistent) {
    jobq_test::TemporaryDirectory directory("cli_e2e");
    jobq_test::Counter entered;
    jobq_test::Gate release;
    jobq::Engine* engine = nullptr;
    unsigned initializations = 0;
    std::vector<jobq::JobId> ids;
    jobq::EngineConfig config{.workerCount = 2};
    if (persistent) config.storagePath = directory.journal();
    jobq::cli::InteractiveRuntime runtime(config, [&](jobq::Engine& current) {
        engine = &current;
        ++initializations;
        current.registerHandler("EMAIL", [&](const jobq::Job& job) {
            if (job.getPayload() != "user@example.com") throw std::runtime_error("Payload changed");
            entered.add();
            if (!release.wait()) throw std::runtime_error("Release timed out");
        });
        current.registerHandler("REPORT", [&](const jobq::Job& job) {
            if (job.getPayload() != "monthly report") throw std::runtime_error("Payload changed");
            entered.add();
            if (!release.wait()) throw std::runtime_error("Release timed out");
        });
        current.registerHandler("FAIL", [](const jobq::Job&) { throw std::runtime_error("expected failure"); });
        current.registerHandler("RETRY", [](const jobq::Job& job) {
            if (job.getAttemptsMade() == 1) throw std::runtime_error("first attempt fails");
        });
    });
    std::ostringstream output;
    std::ostringstream errors;
    auto execute = [&](const std::string& text, std::ostream& stream) {
        jobq::cli::InteractiveCommand command;
        std::string error;
        if (!jobq::cli::parseInteractiveCommand(text, command, error)) throw std::runtime_error(error);
        runtime.commands().dispatch(command, stream);
    };
    runtime.commands().registerCommand("verify-workflow", "Verify real workflow state.",
        [&](const auto&, auto& stream) {
            check(entered.waitAtLeast(2), "Two workers concurrently execute command-submitted jobs");
            const auto transcript = output.str();
            const std::regex identifier("Job ID[ ]+([0-9]+)");
            for (auto entry = std::sregex_iterator(transcript.begin(), transcript.end(), identifier);
                 entry != std::sregex_iterator(); ++entry) ids.push_back(std::stoull((*entry)[1].str()));
            check(ids.size() == 3 && ids[0] != ids[1] && ids[1] != ids[2],
                  "Accepted identifiers are captured from actual command results");
            if (ids.size() != 3) { release.open(); return jobq::cli::CommandAction::Exit; }
            check(engine == &runtime.engine() && initializations == 1 && engine->snapshot().workerCount == 2 &&
                  engine->getJob(ids[0])->status == jobq::JobStatus::Running &&
                  engine->getJob(ids[1])->status == jobq::JobStatus::Running &&
                  engine->getJob(ids[2])->status == jobq::JobStatus::Pending && engine->snapshot().readyJobs == 1,
                  "One Engine owns two running jobs and a genuinely waiting job");
            for (const auto id : ids) execute("job " + std::to_string(id), stream);
            execute("jobs", stream);
            execute("metrics", stream);
            jobq::JobOptions retry;
            retry.retryPolicy.maxRetries = 1;
            retry.retryPolicy.initialDelay = 20ms;
            const auto retried = engine->submit("RETRY", "retry payload", retry);
            ids.push_back(retried);
            release.open();
            check(jobq_test::waitUntil([&] { return engine->metrics().finishedExecutions == 5; }),
                  "Real accepted work reaches three successful and two failed attempts");
            check(engine->getJob(ids[2])->status == jobq::JobStatus::Failed &&
                  engine->getJob(retried)->status == jobq::JobStatus::Completed &&
                  engine->getJob(retried)->attemptsMade == 2 && engine->snapshot().readyJobs == 0 &&
                  engine->snapshot().delayedJobs == 0,
                  "Job records, retry attempt history, and drained queue agree");
            const auto metrics = engine->metrics();
            const auto seconds = std::chrono::duration<double>(metrics.elapsedTime).count();
            check(metrics.submittedJobs == 4 && metrics.startedExecutions == 5 && metrics.completedExecutions == 3 &&
                  metrics.failedExecutions == 2 && metrics.scheduledRetries == 1 &&
                  metrics.averageExecutionTime.count() == metrics.totalExecutionTime.count() / 5 &&
                  metrics.totalExecutionTime.count() > 0 && seconds > 0 &&
                  std::abs(metrics.throughputJobsPerSecond - 5.0 / seconds) < 1e-9,
                  "Shared collector accounts for four jobs, five attempts, retry, duration, and cumulative rate");
            execute("job " + std::to_string(retried), stream);
            return jobq::cli::CommandAction::Continue;
        });
    std::istringstream input(
        "help\nhelp help\nhelp status\nhelp workers\nhelp queue\nhelp submit\nhelp jobs\nhelp job\n"
        "help metrics\nhelp monitor\nhelp exit\nhelp quit\nstatus\nworkers\nqueue\njobs\n"
        "invalid-command\nsubmit\nsubmit UNKNOWN data\njob invalid-id\njob 18446744073709551615\n"
        "submit EMAIL 'broken\nmonitor invalid 3\nmonitor 100 0\njobs extra\nhelp unknown-command\n"
        "submit EMAIL user@example.com\nsubmit REPORT 'monthly report'\nsubmit FAIL payload\n"
        "verify-workflow\njobs\nmetrics\nmonitor 100 3\nstatus\nqueue\nexit\nsubmit EMAIL ignored\n"
    );
    check(runtime.run(input, output, errors) == 0 && initializations == 1,
          "Complete command workflow exits successfully with one Engine initialization");
    check(output.str().find("No jobs available.") != std::string::npos &&
          output.str().find("Worker pool             Running") != std::string::npos &&
          output.str().find("Monitoring completed.") != std::string::npos &&
          output.str().find("Runtime stopped.") != std::string::npos,
          "Empty state, supported workers, bounded monitoring, later commands, and shutdown render correctly");
    const auto transcript = output.str();
    const std::regex samples("\nSample [0-9]+\n");
    check(std::distance(std::sregex_iterator(transcript.begin(), transcript.end(), samples),
                        std::sregex_iterator()) == 3 && engine->snapshot().workersStopped &&
          engine->metrics().submittedJobs == 4 && runtime.snapshot().state == jobq::cli::RuntimeState::Stopped,
          "Exactly three samples complete; workers join; commands after exit are never submitted");
    for (const auto* error : {"Unknown command", "Usage: submit", "No handler registered", "Usage: job",
                              "Job not found", "Unterminated", "Usage: monitor", "Usage: jobs", "Unknown help topic"}) {
        check(errors.str().find(error) != std::string::npos, std::string("Workflow recovers from ") + error);
    }
    if (persistent) {
        {
            std::ofstream journal(directory.journal(), std::ios::binary | std::ios::app);
            journal << "incomplete tail";
        }
        jobq::cli::InteractiveRuntime recovered(config);
        std::istringstream commands("jobs\njob " + std::to_string(ids.back()) + "\nmetrics\nquit\n");
        std::ostringstream recoveredOutput;
        std::ostringstream recoveredErrors;
        check(recovered.run(commands, recoveredOutput, recoveredErrors) == 0 && recoveredErrors.str().empty() &&
              recovered.engine().listJobs().size() == 4 &&
              recovered.engine().getJob(ids.back())->attemptsMade == 2 &&
              recovered.engine().metrics().submittedJobs == 0 &&
              recoveredOutput.str().find("COMPLETED") != std::string::npos,
              "A second configured session recovers latest records and retry history, repairs a tail, and starts fresh metrics");
    }
}

}

int main() {
    try { workflow(false); workflow(true); }
    catch (const std::exception& error) { check(false, error.what()); }
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
