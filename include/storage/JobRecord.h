#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "job/Job.h"

namespace jobq {

struct JobRecord {
    uint64_t id{0};

    std::string type;
    std::string payload;

    JobPriority priority{JobPriority::Normal};
    JobStatus status{JobStatus::Pending};

    RetryPolicy retryPolicy{};

    std::size_t attemptsMade{0};
};

std::string serializeJobRecord(const JobRecord& record);

JobRecord deserializeJobRecord(std::string_view serialized);

}