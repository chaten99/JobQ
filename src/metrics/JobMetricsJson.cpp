#include "metrics/JobMetricsJson.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace jobq {

namespace {

std::string formatThroughputJson(double throughput) {
    if (!std::isfinite(throughput)) {
        return "null";
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(10) << throughput;

    std::string value = stream.str();
    if (value.find('.') == std::string::npos &&
        value.find('e') == std::string::npos &&
        value.find('E') == std::string::npos) {
        value += ".0";
    }

    return value;
}

} // namespace

std::string toJson(const JobMetricsSnapshot& snapshot) {
    std::ostringstream out;
    out.imbue(std::locale::classic());

    out << "{\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"metrics\": {\n";
    out << "    \"submittedJobs\": " << snapshot.submittedJobs << ",\n";
    out << "    \"startedExecutions\": " << snapshot.startedExecutions << ",\n";
    out << "    \"completedExecutions\": " << snapshot.completedExecutions << ",\n";
    out << "    \"failedExecutions\": " << snapshot.failedExecutions << ",\n";
    out << "    \"scheduledRetries\": " << snapshot.scheduledRetries << ",\n";
    out << "    \"finishedExecutions\": " << snapshot.finishedExecutions << "\n";
    out << "  },\n";
    out << "  \"performance\": {\n";
    out << "    \"totalExecutionTimeNs\": " << snapshot.totalExecutionTime.count() << ",\n";
    out << "    \"averageExecutionTimeNs\": " << snapshot.averageExecutionTime.count() << ",\n";
    out << "    \"elapsedTimeNs\": " << snapshot.elapsedTime.count() << ",\n";
    out << "    \"throughputJobsPerSecond\": " << formatThroughputJson(snapshot.throughputJobsPerSecond) << "\n";
    out << "  }\n";
    out << "}";

    return out.str();
}

} // namespace jobq
