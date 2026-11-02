#pragma once

#include <string>

#include "metrics/JobMetrics.h"

namespace jobq {

std::string toJson(const JobMetricsSnapshot& snapshot);

} // namespace jobq
