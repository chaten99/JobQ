cmake_minimum_required(VERSION 3.20)

project(JobQ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(JOBQ_CORE_SOURCES
    src/job/Job.cpp
    src/queue/JobQueue.cpp
    src/executor/JobExecutor.cpp
    src/worker/Worker.cpp
    src/worker/WorkerPool.cpp
    src/manager/JobManager.cpp
    src/storage/JobRecord.cpp
    src/storage/FileJobStorage.cpp
)

add_executable(jobq
    src/main.cpp
    ${JOBQ_CORE_SOURCES}
)

target_include_directories(jobq PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(jobq PRIVATE
    Threads::Threads
)

add_executable(jobq_stress_test
    examples/stress_test.cpp
    ${JOBQ_CORE_SOURCES}
)

target_include_directories(jobq_stress_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(jobq_stress_test PRIVATE
    Threads::Threads
)

include(CTest)

if(BUILD_TESTING)
    add_executable(jobq_manager_test
        tests/job_manager_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_manager_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_manager_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_manager_lifecycle
        COMMAND jobq_manager_test
    )

    add_executable(jobq_priority_test
        tests/job_priority_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_priority_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_priority_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_priority_scheduling
        COMMAND jobq_priority_test
    )

    add_executable(jobq_retry_policy_test
        tests/job_retry_policy_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_retry_policy_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_retry_policy_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_retry_policy
        COMMAND jobq_retry_policy_test
    )

    add_executable(jobq_retry_execution_test
        tests/job_retry_execution_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_retry_execution_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_retry_execution_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_retry_execution
        COMMAND jobq_retry_execution_test
    )

    add_executable(jobq_retry_backoff_test
        tests/job_retry_backoff_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_retry_backoff_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_retry_backoff_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_retry_backoff
        COMMAND jobq_retry_backoff_test
    )

    add_executable(jobq_storage_record_test
        tests/job_record_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_storage_record_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_storage_record_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_storage_record
        COMMAND jobq_storage_record_test
    )

    add_executable(jobq_file_storage_test
        tests/job_file_storage_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_file_storage_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_file_storage_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_file_storage
        COMMAND jobq_file_storage_test
    )

    add_executable(jobq_manager_persistence_test
        tests/job_manager_persistence_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_manager_persistence_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_manager_persistence_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_manager_persistence
        COMMAND jobq_manager_persistence_test
    )

    add_executable(jobq_recovery_test
        tests/job_recovery_test.cpp
        ${JOBQ_CORE_SOURCES}
    )

    target_include_directories(jobq_recovery_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(jobq_recovery_test PRIVATE
        Threads::Threads
    )

    add_test(
        NAME jobq_crash_recovery
        COMMAND jobq_recovery_test
    )
endif()
