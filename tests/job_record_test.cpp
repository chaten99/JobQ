#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "storage/JobRecord.h"
#include "storage/JobStorage.h"

namespace {

using namespace std::chrono_literals;

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

template <typename Function>
bool throwsInvalidArgument(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }

    return false;
}

bool recordsEqual(
    const jobq::JobRecord& left,
    const jobq::JobRecord& right
) {
    return left.id == right.id &&
           left.type == right.type &&
           left.payload == right.payload &&
           left.priority == right.priority &&
           left.status == right.status &&
           left.attemptsMade == right.attemptsMade &&
           left.retryPolicy.maxRetries ==
               right.retryPolicy.maxRetries &&
           left.retryPolicy.initialDelay ==
               right.retryPolicy.initialDelay &&
           left.retryPolicy.multiplier ==
               right.retryPolicy.multiplier &&
           left.retryPolicy.maxDelay ==
               right.retryPolicy.maxDelay;
}

class InMemoryJobStorage final : public jobq::JobStorage {
public:
    void save(const jobq::JobRecord& record) override {
        const auto iterator = std::find_if(
            records_.begin(),
            records_.end(),
            [&record](const jobq::JobRecord& existing) {
                return existing.id == record.id;
            }
        );

        if (iterator == records_.end()) {
            records_.push_back(record);
        } else {
            *iterator = record;
        }
    }

    std::vector<jobq::JobRecord> loadAll() override {
        return records_;
    }

private:
    std::vector<jobq::JobRecord> records_;
};

void testRoundTrip() {
    std::cout << "\n--- Serialization Round Trip ---\n";

    jobq::JobRecord original;

    original.id = 42;
    original.type = "REPORT";
    original.payload = "monthly-report";
    original.priority = jobq::JobPriority::High;
    original.status = jobq::JobStatus::Running;
    original.retryPolicy = jobq::RetryPolicy{
        3,
        125ms,
        2.5,
        2000ms
    };
    original.attemptsMade = 2;

    const auto serialized =
        jobq::serializeJobRecord(original);

    const auto restored =
        jobq::deserializeJobRecord(serialized);

    check(
        !serialized.empty(),
        "A valid job record can be serialized"
    );

    check(
        serialized.starts_with("JOBQ1|"),
        "Serialized record includes a format version"
    );

    check(
        recordsEqual(original, restored),
        "All job fields survive a serialization round trip"
    );
}

void testSpecialCharacters() {
    std::cout << "\n--- Special Characters ---\n";

    jobq::JobRecord original;

    original.id = 43;
    original.type = "EMAIL|BATCH\n";
    original.payload = "line-one|line-two\nline-three\\";

    original.payload.push_back('\0');
    original.payload += "binary-tail";

    const auto serialized =
        jobq::serializeJobRecord(original);

    const auto restored =
        jobq::deserializeJobRecord(serialized);

    check(
        serialized.find('\n') == std::string::npos,
        "Encoded fields do not introduce raw newlines"
    );

    check(
        restored.type == original.type,
        "Type preserves separators and newlines"
    );

    check(
        restored.payload == original.payload,
        "Payload preserves separators, newlines, and null bytes"
    );
}

void testEmptyStrings() {
    std::cout << "\n--- Empty String Fields ---\n";

    jobq::JobRecord original;

    original.id = 44;
    original.type = "";
    original.payload = "";

    const auto restored = jobq::deserializeJobRecord(
        jobq::serializeJobRecord(original)
    );

    check(
        restored.type.empty() && restored.payload.empty(),
        "Empty type and payload survive serialization"
    );
}

void testMalformedRecords() {
    std::cout << "\n--- Malformed Record Rejection ---\n";

    check(
        throwsInvalidArgument([] {
            jobq::deserializeJobRecord(
                "JOBQ9|1|PENDING|NORMAL|0|0|100|2|500||"
            );
        }),
        "Unsupported format version is rejected"
    );

    check(
        throwsInvalidArgument([] {
            jobq::deserializeJobRecord(
                "JOBQ1|1|PENDING"
            );
        }),
        "Incorrect field count is rejected"
    );

    check(
        throwsInvalidArgument([] {
            jobq::deserializeJobRecord(
                "JOBQ1|1|WAITING|NORMAL|0|0|100|2|500||"
            );
        }),
        "Unknown status is rejected"
    );

    check(
        throwsInvalidArgument([] {
            jobq::deserializeJobRecord(
                "JOBQ1|1|PENDING|URGENT|0|0|100|2|500||"
            );
        }),
        "Unknown priority is rejected"
    );

    check(
        throwsInvalidArgument([] {
            jobq::deserializeJobRecord(
                "JOBQ1|1|PENDING|NORMAL|0|0|100|0.5|500||"
            );
        }),
        "Invalid multiplier is rejected"
    );

    check(
        throwsInvalidArgument([] {
            jobq::deserializeJobRecord(
                "JOBQ1|1|PENDING|NORMAL|0|0|100|2|500|0|"
            );
        }),
        "Malformed hexadecimal fields are rejected"
    );

    check(
        throwsInvalidArgument([] {
            jobq::deserializeJobRecord(
                "JOBQ1|1|RUNNING|NORMAL|0|0|100|2|500||"
            );
        }),
        "RUNNING record without an attempt is rejected"
    );

    check(
        throwsInvalidArgument([] {
            jobq::deserializeJobRecord(
                "JOBQ1|0|PENDING|NORMAL|0|0|100|2|500||"
            );
        }),
        "Job ID zero is rejected"
    );
}

void testSerializationRejectsInvalidRecord() {
    std::cout << "\n--- Invalid Record Serialization ---\n";

    jobq::JobRecord invalid;
    invalid.id = 0;

    check(
        throwsInvalidArgument([&invalid] {
            jobq::serializeJobRecord(invalid);
        }),
        "Serializer rejects an invalid record"
    );
}

void testStorageInterfaceContract() {
    std::cout << "\n--- Storage Interface Contract ---\n";

    InMemoryJobStorage storage;

    jobq::JobRecord first;

    first.id = 50;
    first.type = "EMAIL";
    first.payload = "notification";
    first.status = jobq::JobStatus::Pending;

    storage.save(first);

    auto records = storage.loadAll();

    check(
        records.size() == 1,
        "Storage returns saved records"
    );

    first.status = jobq::JobStatus::Completed;
    first.attemptsMade = 1;

    storage.save(first);

    records = storage.loadAll();

    check(
        records.size() == 1,
        "Saving an existing ID replaces its snapshot"
    );

    check(
        records.front().status == jobq::JobStatus::Completed,
        "Storage exposes the latest snapshot"
    );
}

} // namespace

int main() {
    std::cout << "========================================\n";
    std::cout << "         Job Record Tests               \n";
    std::cout << "========================================\n";

    try {
        testRoundTrip();
        testSpecialCharacters();
        testEmptyStrings();
        testMalformedRecords();
        testSerializationRejectsInvalidRecord();
        testStorageInterfaceContract();
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

    return testsFailed == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}