#pragma once

#include <filesystem>
#include <mutex>

#include "storage/JobStorage.h"

namespace jobq {

class FileJobStorage final : public JobStorage {
public:
    explicit FileJobStorage(std::filesystem::path filePath);

    void save(const JobRecord& record) override;

    std::vector<JobRecord> loadAll() override;

private:
    void repairIncompleteTailLocked();

    std::filesystem::path filePath_;
    std::mutex mutex_;
};

}