#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "storage/FileJobStorage.h"

namespace {

using namespace std::chrono_literals;

int testsRun = 0;
int testsFailed = 0;

const std::filesystem::path& testRoot() {
    static const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "jobq_file_storage_tests";

    return root;
}

const std::filesystem::path& journalPath() {
    static const std::filesystem::path path =
        testRoot() / "nested" / "jobs.journal";

    return path;
}

void resetStorage() {
    std::filesystem::remove_all(testRoot());
}

void check(bool condition, const std::string& description) {
    ++testsRun;

    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++testsFailed;
        std::cout << "[FAIL] " << description << '\n';
    }
}

jobq::JobRecord makeRecord(
    std::uint64_t id,
    std::string type,
    std::string payload
) {
    jobq::JobRecord record;

    record.id = id;
    record.type = std::move(type);
    record.payload = std::move(payload);
    record.priority = jobq::JobPriority::Normal;
    record.status = jobq::JobStatus::Pending;

    return record;
}

bool hasRecord(
    const std::vector<jobq::JobRecord>& records,
    std::uint64_t id
) {
    return std::any_of(
        records.begin(),
        records.end(),
        [id](const jobq::JobRecord& record) {
            return record.id == id;
        }
    );
}

void testMissingFileAndDirectoryCreation() {
    std::cout << "\n--- Missing File and Directory Creation ---\n";

    resetStorage();

    jobq::FileJobStorage storage(journalPath());

    check(
        storage.loadAll().empty(),
        "Missing storage file returns no records"
    );

    check(
        std::filesystem::exists(journalPath().parent_path()),
        "Storage constructor creates parent directories"
    );
}

void testSaveAndReload() {
    std::cout << "\n--- Save and Reload ---\n";

    resetStorage();

    auto first = makeRecord(
        101,
        "EMAIL",
        "first-record"
    );

    first.priority = jobq::JobPriority::High;
    first.retryPolicy = jobq::RetryPolicy{
        3,
        50ms,
        2.0,
        500ms
    };

    auto updated = first;
    updated.status = jobq::JobStatus::Completed;
    updated.attemptsMade = 1;

    auto second = makeRecord(
        102,
        "REPORT",
        "second-record"
    );

    {
        jobq::FileJobStorage storage(journalPath());

        storage.save(first);
        storage.save(updated);
        storage.save(second);

        const auto records = storage.loadAll();

        check(
            records.size() == 2,
            "Latest snapshots are returned once per job ID"
        );

        const auto found = std::find_if(
            records.begin(),
            records.end(),
            [](const jobq::JobRecord& record) {
                return record.id == 101;
            }
        );

        check(
            found != records.end() &&
                found->status == jobq::JobStatus::Completed &&
                found->attemptsMade == 1,
            "Latest status snapshot replaces the earlier record"
        );

        check(
            found != records.end() &&
                found->priority == jobq::JobPriority::High &&
                found->retryPolicy.maxRetries == 3,
            "Priority and retry policy survive storage"
        );
    }

    {
        jobq::FileJobStorage restoredStorage(journalPath());

        const auto records = restoredStorage.loadAll();

        check(
            records.size() == 2,
            "Records remain available after reopening storage"
        );

        check(
            hasRecord(records, 101) &&
                hasRecord(records, 102),
            "Reopened storage returns all known job IDs"
        );
    }
}

void testIncompleteTailRecovery() {
    std::cout << "\n--- Incomplete Tail Recovery ---\n";

    resetStorage();

    {
        jobq::FileJobStorage storage(journalPath());

        storage.save(
            makeRecord(201, "EMAIL", "complete-record")
        );
    }

    // Simulate a process interrupted in the middle of an append:
    // the partial line has no terminating newline.
    {
        std::ofstream output(
            journalPath(),
            std::ios::binary | std::ios::app
        );

        output << "JOBQ1|202|PENDING|";
    }

    {
        jobq::FileJobStorage storage(journalPath());

        const auto beforeRepair = storage.loadAll();

        check(
            beforeRepair.size() == 1 &&
                beforeRepair.front().id == 201,
            "Loader ignores an incomplete trailing record"
        );

        storage.save(
            makeRecord(203, "REPORT", "record-after-repair")
        );

        const auto afterRepair = storage.loadAll();

        check(
            afterRepair.size() == 2,
            "Next save repairs the tail before appending"
        );

        check(
            hasRecord(afterRepair, 201) &&
                hasRecord(afterRepair, 203) &&
                !hasRecord(afterRepair, 202),
            "Tail repair preserves complete records and discards the fragment"
        );
    }
}

void testCompleteCorruptionIsRejected() {
    std::cout << "\n--- Corrupt Complete Record ---\n";

    resetStorage();

    jobq::FileJobStorage storage(journalPath());

    storage.save(
        makeRecord(301, "EMAIL", "valid-record")
    );

    {
        std::ofstream output(
            journalPath(),
            std::ios::binary | std::ios::app
        );

        output << "THIS_IS_NOT_A_VALID_RECORD\n";
    }

    bool corruptionRejected = false;

    try {
        (void)storage.loadAll();
    } catch (const std::runtime_error&) {
        corruptionRejected = true;
    }

    check(
        corruptionRejected,
        "Corrupt complete record is reported as an error"
    );
}

void testInvalidRecordIsNotSaved() {
    std::cout << "\n--- Invalid Record Rejection ---\n";

    resetStorage();

    jobq::FileJobStorage storage(journalPath());

    auto invalidRecord = makeRecord(
        0,
        "EMAIL",
        "invalid-ID"
    );

    bool rejected = false;

    try {
        storage.save(invalidRecord);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    check(
        rejected,
        "Invalid records are rejected before saving"
    );

    check(
        storage.loadAll().empty(),
        "Rejected record is absent from storage"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "        File Job Storage Tests          \n";
    std::cout << "========================================\n";

    try {
        testMissingFileAndDirectoryCreation();
        testSaveAndReload();
        testIncompleteTailRecovery();
        testCompleteCorruptionIsRejected();
        testInvalidRecordIsNotSaved();
    } catch (const std::exception& exception) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected exception: "
            << exception.what()
            << '\n';
    } catch (...) {
        ++testsFailed;

        std::cerr
            << "[FAIL] Unexpected non-standard exception\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "Tests run: " << testsRun << '\n';
    std::cout << "Tests failed: " << testsFailed << '\n';

    if (testsFailed == 0) {
        std::cout << "Result: ALL TESTS PASSED\n";
    } else {
        std::cout << "Result: TESTS FAILED\n";
    }

    std::cout << "========================================\n";

    resetStorage();

    return testsFailed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}