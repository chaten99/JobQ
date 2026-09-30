#include <iostream>

#include "job/Job.h"

int main() {
    jobq::Job job(
        1001,
        "REPORT",
        "monthly-report"
    );

    std::cout << "Job ID: " << job.getId() << '\n';
    std::cout << "Job Type: " << job.getType() << '\n';
    std::cout << "Payload: " << job.getPayload() << '\n';

    return 0;
}