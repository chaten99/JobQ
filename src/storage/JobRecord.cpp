#include "storage/JobRecord.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace jobq {

namespace {

constexpr std::string_view kRecordVersion = "JOBQ1";

std::string hexEncode(std::string_view input) {
    constexpr char digits[] = "0123456789ABCDEF";

    std::string output;
    output.reserve(input.size() * 2);

    for (unsigned char value : input) {
        output.push_back(digits[value >> 4]);
        output.push_back(digits[value & 0x0F]);
    }

    return output;
}

int hexValue(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }

    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }

    return -1;
}

std::string hexDecode(std::string_view input) {
    if (input.size() % 2 != 0) {
        throw std::invalid_argument(
            "Serialized hexadecimal field has an odd length"
        );
    }

    std::string output;
    output.reserve(input.size() / 2);

    for (std::size_t index = 0;
         index < input.size();
         index += 2) {

        const int high = hexValue(input[index]);
        const int low = hexValue(input[index + 1]);

        if (high < 0 || low < 0) {
            throw std::invalid_argument(
                "Serialized record contains invalid hexadecimal data"
            );
        }

        const auto value = static_cast<unsigned char>(
            (high << 4) | low
        );

        output.push_back(static_cast<char>(value));
    }

    return output;
}

std::vector<std::string_view> splitFields(
    std::string_view input
) {
    std::vector<std::string_view> fields;

    std::size_t start = 0;

    while (true) {
        const auto separator = input.find('|', start);

        if (separator == std::string_view::npos) {
            fields.push_back(input.substr(start));
            break;
        }

        fields.push_back(
            input.substr(start, separator - start)
        );

        start = separator + 1;
    }

    return fields;
}

template <typename Integer>
Integer parseInteger(
    std::string_view value,
    const char* fieldName
) {
    if (value.empty()) {
        throw std::invalid_argument(
            std::string("Missing numeric field: ") + fieldName
        );
    }

    Integer result{};

    const char* begin = value.data();
    const char* end = begin + value.size();

    const auto [position, error] =
        std::from_chars(begin, end, result);

    if (error != std::errc{} || position != end) {
        throw std::invalid_argument(
            std::string("Invalid numeric field: ") + fieldName
        );
    }

    return result;
}

double parseDouble(
    std::string_view value,
    const char* fieldName
) {
    std::string text(value);

    std::size_t consumed = 0;
    double result{};

    try {
        result = std::stod(text, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(
            std::string("Invalid numeric field: ") + fieldName
        );
    }

    if (consumed != text.size()) {
        throw std::invalid_argument(
            std::string("Invalid numeric field: ") + fieldName
        );
    }

    return result;
}

JobStatus parseStatus(std::string_view value) {
    if (value == "PENDING") {
        return JobStatus::Pending;
    }

    if (value == "RUNNING") {
        return JobStatus::Running;
    }

    if (value == "COMPLETED") {
        return JobStatus::Completed;
    }

    if (value == "FAILED") {
        return JobStatus::Failed;
    }

    if (value == "CANCELLED") {
        return JobStatus::Cancelled;
    }

    throw std::invalid_argument(
        "Serialized record contains an unknown job status"
    );
}

JobPriority parsePriority(std::string_view value) {
    if (value == "LOW") {
        return JobPriority::Low;
    }

    if (value == "NORMAL") {
        return JobPriority::Normal;
    }

    if (value == "HIGH") {
        return JobPriority::High;
    }

    throw std::invalid_argument(
        "Serialized record contains an unknown job priority"
    );
}

void validateRecord(const JobRecord& record) {
    if (record.id == 0) {
        throw std::invalid_argument(
            "Job ID must be greater than zero"
        );
    }

    if (toString(record.status) == "UNKNOWN") {
        throw std::invalid_argument(
            "Job record contains an invalid status"
        );
    }

    if (toString(record.priority) == "UNKNOWN") {
        throw std::invalid_argument(
            "Job record contains an invalid priority"
        );
    }

    if (record.retryPolicy.initialDelay.count() < 0) {
        throw std::invalid_argument(
            "Retry initial delay cannot be negative"
        );
    }

    if (record.retryPolicy.maxDelay.count() < 0) {
        throw std::invalid_argument(
            "Retry maximum delay cannot be negative"
        );
    }

    if (!std::isfinite(record.retryPolicy.multiplier) ||
        record.retryPolicy.multiplier < 1.0) {
        throw std::invalid_argument(
            "Retry multiplier must be finite and at least 1.0"
        );
    }

    const bool requiresAttempt =
        record.status == JobStatus::Running ||
        record.status == JobStatus::Completed ||
        record.status == JobStatus::Failed;

    if (requiresAttempt && record.attemptsMade == 0) {
        throw std::invalid_argument(
            "Running, completed, and failed jobs must have an attempt"
        );
    }
}

} // namespace

std::string serializeJobRecord(const JobRecord& record) {
    validateRecord(record);

    std::ostringstream output;

    output
        << kRecordVersion << '|'
        << record.id << '|'
        << toString(record.status) << '|'
        << toString(record.priority) << '|'
        << record.attemptsMade << '|'
        << record.retryPolicy.maxRetries << '|'
        << record.retryPolicy.initialDelay.count() << '|'
        << std::setprecision(
               std::numeric_limits<double>::max_digits10
           )
        << record.retryPolicy.multiplier << '|'
        << record.retryPolicy.maxDelay.count() << '|'
        << hexEncode(record.type) << '|'
        << hexEncode(record.payload);

    return output.str();
}

JobRecord deserializeJobRecord(std::string_view serialized) {
    const auto fields = splitFields(serialized);

    if (fields.size() != 11) {
        throw std::invalid_argument(
            "Serialized job record must contain exactly 11 fields"
        );
    }

    if (fields[0] != kRecordVersion) {
        throw std::invalid_argument(
            "Unsupported serialized job record version"
        );
    }

    JobRecord record;

    record.id = parseInteger<uint64_t>(
        fields[1],
        "id"
    );

    record.status = parseStatus(fields[2]);
    record.priority = parsePriority(fields[3]);

    record.attemptsMade = parseInteger<std::size_t>(
        fields[4],
        "attemptsMade"
    );

    record.retryPolicy.maxRetries =
        parseInteger<std::size_t>(
            fields[5],
            "maxRetries"
        );

    using MillisecondsRep =
        std::chrono::milliseconds::rep;

    record.retryPolicy.initialDelay =
        std::chrono::milliseconds{
            parseInteger<MillisecondsRep>(
                fields[6],
                "initialDelayMs"
            )
        };

    record.retryPolicy.multiplier = parseDouble(
        fields[7],
        "multiplier"
    );

    record.retryPolicy.maxDelay =
        std::chrono::milliseconds{
            parseInteger<MillisecondsRep>(
                fields[8],
                "maxDelayMs"
            )
        };

    record.type = hexDecode(fields[9]);
    record.payload = hexDecode(fields[10]);

    validateRecord(record);

    return record;
}

}