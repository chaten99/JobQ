#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "cli/InteractiveRuntime.h"
#include "cli/JobCommands.h"
#include "helpers/TemporaryDirectory.h"
#include "helpers/TestSupport.h"

namespace {

using jobq_test::check;

void dispatch(jobq::cli::InteractiveRuntime& runtime, const std::string& line, std::ostream& output) {
    jobq::cli::InteractiveCommand command;
    std::string error;
    if (!jobq::cli::parseInteractiveCommand(line, command, error)) {
        throw std::invalid_argument(error);
    }
    runtime.commands().dispatch(command, output);
}

void testSession() {
    jobq_test::Counter entered;
    jobq_test::Gate release;
    unsigned initialized = 0;
    jobq::Engine* engine = nullptr;
    jobq::cli::InteractiveRuntime runtime({.workerCount = 1}, [&](jobq::Engine& current) {
        ++initialized;
        engine = &current;
        current.registerHandler("TASK", [&](const jobq::Job&) {
            entered.add();
            if (!release.wait()) {
                throw std::runtime_error("Session handler timed out");
            }
        });
        current.registerHandler("OTHER", [](const jobq::Job&) {});
    });
    std::ostringstream output;
    std::ostringstream errors;
    check(jobq_test::throwsWith<std::logic_error>([&] { dispatch(runtime, "submit TASK data", output); },
          "initialized"), "Submission before runtime startup reports unavailable Engine");
    std::vector<jobq::JobId> ids;
    runtime.commands().registerCommand("inspect-submissions", "Inspect actual returned identifiers.",
        [&](const auto&, auto& stream) {
            check(entered.waitAtLeast(1), "Real pool processes the submitted job");
            const auto text = output.str();
            const std::regex identifier("Job ID[ ]+([0-9]+)");
            for (auto it = std::sregex_iterator(text.begin(), text.end(), identifier);
                 it != std::sregex_iterator(); ++it) {
                ids.push_back(std::stoull((*it)[1].str()));
            }
            check(ids.size() == 3 && ids[0] != ids[1] && ids[1] != ids[2] &&
                  &runtime.engine() == engine, "Command results contain unique actual IDs from the same Engine");
            if (ids.size() != 3) {
                release.open();
                return jobq::cli::CommandAction::Exit;
            }
            const auto first = engine->getJob(ids[0]);
            check(first && first->type == "TASK" && first->payload == "  multi word, C:\\work  " &&
                  first->status == jobq::JobStatus::Running,
                  "Quoted payload keeps whitespace, comma, and literal Windows backslash");
            check(engine->getJob(ids[1])->payload.empty() &&
                  engine->getJob(ids[2])->payload == "a\"b",
                  "Empty payload and literal quote in alternate quote delimiters are preserved");
            const auto before = engine->listJobs();
            for (const auto id : ids) {
                dispatch(runtime, "job " + std::to_string(id), stream);
            }
            dispatch(runtime, "jobs", stream);
            const auto after = engine->listJobs();
            check(before.size() == after.size() && after[0].status == jobq::JobStatus::Running &&
                  after[1].status == jobq::JobStatus::Pending && after[1].attemptsMade == 0 &&
                  after[2].status == jobq::JobStatus::Pending,
                  "Inspection preserves active and pending job state while formatting copied records");
            release.open();
            return jobq::cli::CommandAction::Continue;
        });
    std::istringstream input(
        "help\nhelp submit\nhelp jobs\nhelp job\njobs\n"
        "submit\nsubmit TASK\nsubmit '' value\nsubmit TASK a b\nsubmit UNKNOWN data\n"
        "jobs extra\njob\njob ''\njob 0\njob -1\njob +1\njob abc\njob 1x\n"
        "job 18446744073709551616\njob 1 2\njob 18446744073709551615\n"
        "submit TASK '  multi word, C:\\work  '\nsubmit OTHER ''\nsubmit OTHER 'a\"b'\n"
        "inspect-submissions\nstatus\nworkers\nqueue\nexit\n"
    );
    check(runtime.run(input, output, errors) == 0 && initialized == 1,
          "Recoverable input errors do not restart or terminate the persistent session");
    check(output.str().find("No jobs available.") != std::string::npos &&
          output.str().find("Usage: submit <type> <payload>") != std::string::npos &&
          output.str().find("Usage: job <job-id>") != std::string::npos &&
          output.str().find("Job Details") != std::string::npos &&
          output.str().find("PENDING") != std::string::npos && output.str().find("RUNNING") != std::string::npos,
          "Help, empty state, and job inspection use registered syntax and real states");
    check(errors.str().find("Usage: submit") != std::string::npos &&
          errors.str().find("Usage: jobs") != std::string::npos &&
          errors.str().find("Usage: job") != std::string::npos &&
          errors.str().find("No handler registered") != std::string::npos &&
          errors.str().find("Job not found: 18446744073709551615") != std::string::npos,
          "Arguments, unknown handler, identifier format/overflow, and missing jobs report clear errors");
    check(engine->listJobs().size() == 3 && engine->metrics().completedExecutions == 3,
          "All accepted jobs are processed by the existing pool and retained after shutdown");
    check(jobq_test::throwsWith<std::logic_error>([&] { dispatch(runtime, "submit TASK data", output); },
          "running"), "Submission after shutdown is rejected");
    auto jobs = engine->listJobs();
    std::ostringstream details;
    jobq::cli::printJob(jobs.front(), details);
    check(details.str().find("COMPLETED") != std::string::npos &&
          details.str().find("Unavailable") != std::string::npos,
          "Completed details expose real attempts and explicit unavailable timestamps/errors");
}

void testRendering() {
    const auto rendered = jobq::cli::summarizeJobText(std::string("a\n\x1b\\\"\xFF", 6));
    check(rendered.find('\\') != std::string::npos && rendered.find('\n') == std::string::npos &&
          rendered.find('\x1b') == std::string::npos && rendered.find("\\xFF") != std::string::npos,
          "Text rendering escapes controls, terminal escapes, quotes, backslashes, and non-ASCII bytes");
    const auto longText = jobq::cli::summarizeJobText(std::string(10000, 'x'));
    check(longText.size() < 220 && longText.find("truncated; 10000 bytes total") != std::string::npos,
          "Long text is bounded and explicitly reports truncation");
    std::vector<jobq::JobSnapshot> many;
    for (unsigned id = 1; id <= 1000; ++id) {
        many.push_back({id, std::string(200, 'T'), "", jobq::JobPriority::Normal,
                        jobq::JobStatus::Pending, {}, 0});
    }
    std::ostringstream listing;
    jobq::cli::printJobs(many, listing);
    check(listing.str().find("Total jobs: 1000") != std::string::npos &&
          listing.str().find("1000 |") != std::string::npos,
          "Large listings have deterministic rows, bounded type text, and no silent record truncation");
}

void testRetriedDetailsAndRecovery() {
    jobq_test::TemporaryDirectory directory("cli_retry_lookup");
    jobq::JobId id = 0;
    {
        jobq::cli::InteractiveRuntime runtime({.workerCount = 1, .storagePath = directory.journal()},
            [](jobq::Engine& engine) {
                engine.registerHandler("FAIL", [](const jobq::Job&) { throw std::runtime_error("expected"); });
            });
        runtime.commands().registerCommand("retry-fixture", "Submit and inspect a retrying job.",
            [&](const auto&, auto& stream) {
                jobq::JobOptions options;
                options.retryPolicy.maxRetries = 1;
                options.retryPolicy.initialDelay = std::chrono::milliseconds{0};
                id = runtime.engine().submit("FAIL", "retry payload", options);
                check(jobq_test::waitUntil([&] {
                    const auto job = runtime.engine().getJob(id);
                    return job && job->attemptsMade == 2 && job->status == jobq::JobStatus::Failed;
                }), "Retrying job reaches its genuine final failed state");
                dispatch(runtime, "job " + std::to_string(id), stream);
                dispatch(runtime, "jobs", stream);
                return jobq::cli::CommandAction::Continue;
            });
        std::istringstream input("retry-fixture\nexit\n");
        std::ostringstream output;
        std::ostringstream errors;
        check(runtime.run(input, output, errors) == 0 && errors.str().empty() &&
              output.str().find("FAILED") != std::string::npos &&
              output.str().find("Attempts made           2") != std::string::npos &&
              output.str().find("Maximum retries         1") != std::string::npos,
              "Individual and listed failed records expose actual retry attempts and policy");
    }
    jobq::cli::InteractiveRuntime recovered({.workerCount = 1, .storagePath = directory.journal()});
    std::istringstream input("jobs\njob " + std::to_string(id) + "\nexit\n");
    std::ostringstream output;
    std::ostringstream errors;
    check(recovered.run(input, output, errors) == 0 && errors.str().empty() &&
          recovered.engine().listJobs().size() == 1 && output.str().find("FAILED") != std::string::npos &&
          output.str().find("retry payload") != std::string::npos,
          "CLI listing and lookup inspect the latest persistent recovered record without a second storage layer");
}

void testStorageFailures() {
    jobq_test::TemporaryDirectory directory("cli_job_failure");
    jobq::cli::InteractiveRuntime runtime({.workerCount = 1, .storagePath = directory.journal()},
        [&](jobq::Engine& engine) {
            engine.registerHandler("TASK", [](const jobq::Job&) {});
            std::filesystem::create_directory(directory.journal());
        });
    std::istringstream input("submit TASK value\njobs\nexit\n");
    std::ostringstream output;
    std::ostringstream errors;
    check(runtime.run(input, output, errors) == 0 && !errors.str().empty() &&
          output.str().find("Job submitted successfully") == std::string::npos &&
          output.str().find("No jobs available.") != std::string::npos &&
          runtime.engine().listJobs().empty(),
          "Storage write failure reports no success, rolls back registry membership, and keeps the session alive");
    std::filesystem::remove(directory.journal());
    {
        std::ofstream journal(directory.journal());
        journal << "invalid complete record\n";
    }
    jobq::cli::InteractiveRuntime corrupt({.workerCount = 1, .storagePath = directory.journal()});
    std::istringstream recoveryInput("jobs\nexit\n");
    std::ostringstream recoveryOutput;
    std::ostringstream recoveryErrors;
    check(corrupt.run(recoveryInput, recoveryOutput, recoveryErrors) == 1 &&
          !recoveryErrors.str().empty() && recoveryOutput.str().find("jobq>") == std::string::npos,
          "Corrupted recovery prevents a partially initialized command loop");
    jobq::cli::InteractiveRuntime initializationFailure({}, [](jobq::Engine&) {
        throw std::runtime_error("handler setup failed");
    });
    std::istringstream noInput;
    check(initializationFailure.run(noInput, recoveryOutput, recoveryErrors) == 1 &&
          initializationFailure.snapshot().state == jobq::cli::RuntimeState::Failed,
          "Handler initialization errors follow the existing startup failure path");
}

}

int main() {
    try {
        testSession();
        testRendering();
        testRetriedDetailsAndRecovery();
        testStorageFailures();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    return jobq_test::testsFailed() == 0 ? 0 : 1;
}
