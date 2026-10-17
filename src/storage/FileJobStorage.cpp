
#include "storage/FileJobStorage.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jobq {

FileJobStorage::FileJobStorage(
    std::filesystem::path filePath
)
    : filePath_(std::move(filePath)) {

    if (filePath_.empty()) {
        throw std::invalid_argument(
            "Storage file path cannot be empty"
        );
    }

    const auto parentPath = filePath_.parent_path();

    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath);
    }
}

void FileJobStorage::save(const JobRecord& record) {
    const std::string serialized =
        serializeJobRecord(record);

    std::lock_guard<std::mutex> lock(mutex_);

    repairIncompleteTailLocked();

    std::ofstream output(
        filePath_,
        std::ios::binary | std::ios::app
    );

    if (!output) {
        throw std::runtime_error(
            "Unable to open job storage file for writing"
        );
    }

    output.write(
        serialized.data(),
        static_cast<std::streamsize>(serialized.size())
    );

    output.put('\n');
    output.flush();

    if (!output) {
        throw std::runtime_error(
            "Failed to write job record to storage file"
        );
    }
}

void FileJobStorage::repairIncompleteTailLocked() {
    if (!std::filesystem::exists(filePath_)) {
        return;
    }

    const auto fileSize =
        std::filesystem::file_size(filePath_);

    if (fileSize == 0) {
        return;
    }

    std::ifstream input(
        filePath_,
        std::ios::binary
    );

    if (!input) {
        throw std::runtime_error(
            "Unable to open job storage file for inspection"
        );
    }

    input.seekg(-1, std::ios::end);

    char lastCharacter{};
    input.get(lastCharacter);

    if (!input) {
        throw std::runtime_error(
            "Unable to inspect the end of the storage file"
        );
    }

    if (lastCharacter == '\n') {
        return;
    }

    constexpr std::uintmax_t chunkSize = 4096;

    std::uintmax_t remaining = fileSize;

    while (remaining > 0) {
        const std::uintmax_t chunkStart =
            remaining > chunkSize
                ? remaining - chunkSize
                : 0;

        const std::uintmax_t chunkLength =
            remaining - chunkStart;

        std::vector<char> buffer(
            static_cast<std::size_t>(chunkLength)
        );

        input.clear();

        input.seekg(
            static_cast<std::streamoff>(chunkStart),
            std::ios::beg
        );

        input.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size())
        );

        if (input.gcount() !=
            static_cast<std::streamsize>(buffer.size())) {
            throw std::runtime_error(
                "Unable to inspect storage journal tail"
            );
        }

        for (std::size_t index = buffer.size();
             index > 0;
             --index) {

            if (buffer[index - 1] == '\n') {
                input.close();

                std::filesystem::resize_file(
                    filePath_,
                    chunkStart + index
                );

                return;
            }
        }

        remaining = chunkStart;
    }

    input.close();

    std::filesystem::resize_file(filePath_, 0);
}

std::vector<JobRecord> FileJobStorage::loadAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!std::filesystem::exists(filePath_)) {
        return {};
    }

    std::ifstream input(
        filePath_,
        std::ios::binary
    );

    if (!input) {
        throw std::runtime_error(
            "Unable to open job storage file for reading"
        );
    }

    const std::string contents = std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );

    if (input.bad()) {
        throw std::runtime_error(
            "Failed while reading the job storage file"
        );
    }

    std::vector<JobRecord> records;

    std::unordered_map<uint64_t, std::size_t> recordIndices;

    std::size_t cursor = 0;
    std::size_t lineNumber = 0;

    while (cursor < contents.size()) {
        const auto newline = contents.find('\n', cursor);

        if (newline == std::string::npos) {
            break;
        }

        const std::string_view line(
            contents.data() + cursor,
            newline - cursor
        );

        ++lineNumber;

        if (line.empty()) {
            throw std::runtime_error(
                "Empty record in job storage at line " +
                std::to_string(lineNumber)
            );
        }

        JobRecord record;

        try {
            record = deserializeJobRecord(line);
        } catch (const std::exception& exception) {
            throw std::runtime_error(
                "Invalid job storage record at line " +
                std::to_string(lineNumber) +
                ": " +
                exception.what()
            );
        }

        const auto existing = recordIndices.find(record.id);

        if (existing == recordIndices.end()) {
            recordIndices.emplace(
                record.id,
                records.size()
            );

            records.push_back(std::move(record));
        } else {
            records[existing->second] = std::move(record);
        }

        cursor = newline + 1;
    }

    return records;
}

}
