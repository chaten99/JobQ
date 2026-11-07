#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "storage/JobRecord.h"
#include "storage/JobStorage.h"

namespace jobq_test {

class FaultInjectingJobStorage final : public jobq::JobStorage {
public:
    using SaveFault = std::function<bool(const jobq::JobRecord&)>;

    void save(const jobq::JobRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (saveFault_ && saveFault_(record)) {
            ++faultsInjected_;
            throw std::runtime_error("injected save failure");
        }

        history_.push_back(record);
    }

    std::vector<jobq::JobRecord> loadAll() override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (failLoad_) {
            throw std::runtime_error("injected load failure");
        }

        std::vector<jobq::JobRecord> records;
        std::unordered_map<std::uint64_t, std::size_t> positions;

        for (const auto& record : history_) {
            const auto found = positions.find(record.id);

            if (found == positions.end()) {
                positions.emplace(record.id, records.size());
                records.push_back(record);
            } else {
                records[found->second] = record;
            }
        }

        return records;
    }

    void setFailLoad(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        failLoad_ = fail;
    }
    void setSaveFault(SaveFault fault) {
        std::lock_guard<std::mutex> lock(mutex_);
        saveFault_ = std::move(fault);
    }

    void clearSaveFault() {
        std::lock_guard<std::mutex> lock(mutex_);
        saveFault_ = nullptr;
    }

    void seed(const jobq::JobRecord& record) {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(record);
    }

    std::size_t faultsInjected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return faultsInjected_;
    }

    std::vector<jobq::JobRecord> history() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_;
    }

    std::optional<jobq::JobRecord> latest(std::uint64_t id) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::optional<jobq::JobRecord> result;

        for (const auto& record : history_) {
            if (record.id == id) {
                result = record;
            }
        }

        return result;
    }

private:
    mutable std::mutex mutex_;
    std::vector<jobq::JobRecord> history_;
    SaveFault saveFault_;
    bool failLoad_{false};
    std::size_t faultsInjected_{0};
};

}
