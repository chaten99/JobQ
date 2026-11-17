#pragma once

#include "job/Job.h"

namespace jobq {

struct JobOptions {
    JobPriority priority{JobPriority::Normal};
    RetryPolicy retryPolicy{};
};

}
