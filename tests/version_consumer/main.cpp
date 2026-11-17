#include <iostream>
#include <string_view>

#include "jobq/Version.h"

static_assert(jobq::version::major == JOBQ_EXPECTED_MAJOR);
static_assert(jobq::version::minor == JOBQ_EXPECTED_MINOR);
static_assert(jobq::version::patch == JOBQ_EXPECTED_PATCH);
static_assert(jobq::version::string == std::string_view(JOBQ_EXPECTED_STRING));

int main() {
    std::cout << jobq::version::string << '\n';
    return 0;
}
