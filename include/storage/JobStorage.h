#pragma once

#include <vector>

#include "storage/JobRecord.h"

namespace jobq {
class JobStorage {
public:
    virtual ~JobStorage() = default;

    virtual void save(const JobRecord& record) = 0;

    virtual std::vector<JobRecord> loadAll() = 0;
};

}