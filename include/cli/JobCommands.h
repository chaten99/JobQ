#pragma once

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "job/Job.h"

namespace jobq::cli {

class InteractiveRuntime;

std::string summarizeJobText(std::string_view text);
void printJobs(const std::vector<JobSnapshot>& jobs, std::ostream& output);
void printJob(const JobSnapshot& job, std::ostream& output);
void registerJobCommands(InteractiveRuntime& runtime);

}
