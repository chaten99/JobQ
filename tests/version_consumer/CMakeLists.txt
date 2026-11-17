cmake_minimum_required(VERSION 3.20)

project(JobQVersionConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(JOBQ_REQUEST_VERSION "" CACHE STRING "Requested JobQ version")
option(JOBQ_REQUEST_EXACT "Require an exact JobQ version" OFF)
option(JOBQ_EXPECT_FOUND "Expect successful package discovery" ON)

if(JOBQ_REQUEST_EXACT)
    find_package(JobQ ${JOBQ_REQUEST_VERSION} EXACT CONFIG QUIET)
else()
    find_package(JobQ ${JOBQ_REQUEST_VERSION} CONFIG QUIET)
endif()

if(NOT JOBQ_EXPECT_FOUND)
    if(JobQ_FOUND)
        message(FATAL_ERROR "Incompatible version was accepted")
    endif()
    if(NOT JOBQ_AVAILABLE_VERSION IN_LIST JobQ_CONSIDERED_VERSIONS)
        message(FATAL_ERROR "Expected package was not considered")
    endif()
    return()
endif()

if(NOT JobQ_FOUND)
    message(FATAL_ERROR "Compatible package was rejected")
endif()

if(NOT JobQ_VERSION STREQUAL JOBQ_AVAILABLE_VERSION)
    message(FATAL_ERROR "Package metadata differs from the producer version")
endif()

add_executable(jobq_version_consumer main.cpp)
target_link_libraries(jobq_version_consumer PRIVATE JobQ::JobQ)
target_compile_definitions(jobq_version_consumer PRIVATE
    JOBQ_EXPECTED_MAJOR=${JobQ_VERSION_MAJOR}
    JOBQ_EXPECTED_MINOR=${JobQ_VERSION_MINOR}
    JOBQ_EXPECTED_PATCH=${JobQ_VERSION_PATCH}
    JOBQ_EXPECTED_STRING="${JobQ_VERSION}"
)

enable_testing()
add_test(NAME jobq_version_consumer COMMAND jobq_version_consumer)
