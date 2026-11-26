#include "ContractSuite.h"
#include "FakeBackend.h"

int main() {
    try {
        jobq::backend::test::verifyContract<jobq::backend::test::FakeBackend>();
    } catch (const std::exception& error) {
        jobq_test::check(false, error.what());
    }
    return jobq_test::testsFailed() ? 1 : 0;
}
