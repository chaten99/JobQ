
cmake_minimum_required(VERSION 3.20)

project(JobQ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(jobq_core STATIC
    src/job/Job.cpp
    src/queue/JobQueue.cpp
    src/executor/JobExecutor.cpp
    src/worker/Worker.cpp
    src/worker/WorkerPool.cpp
    src/manager/JobManager.cpp
    src/storage/JobRecord.cpp
    src/storage/FileJobStorage.cpp
    src/metrics/JobMetrics.cpp
    src/metrics/JobMetricsJson.cpp
    src/metrics/LatencyStatistics.cpp
    src/metrics/ContentionMetrics.cpp
)

add_library(JobQ::Core ALIAS jobq_core)

target_include_directories(jobq_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(jobq_core PUBLIC cxx_std_20)
target_link_libraries(jobq_core PUBLIC Threads::Threads)

add_library(jobq_api STATIC
    src/api/Engine.cpp
)

add_library(JobQ::API ALIAS jobq_api)

target_link_libraries(jobq_api PUBLIC JobQ::Core)

add_executable(jobq_public_api_example
    examples/public_api_example.cpp
)

target_link_libraries(jobq_public_api_example PRIVATE JobQ::API)

add_executable(jobq
    src/main.cpp
)

target_link_libraries(jobq PRIVATE JobQ::Core)

add_executable(jobq_stress_test
    examples/stress_test.cpp
)

target_link_libraries(jobq_stress_test PRIVATE JobQ::Core)

add_executable(jobq_throughput_benchmark
    benchmarks/throughput_benchmark.cpp
)

target_link_libraries(jobq_throughput_benchmark PRIVATE JobQ::Core)

add_executable(jobq_queue_stress_benchmark
    benchmarks/queue_stress_benchmark.cpp
)

target_link_libraries(jobq_queue_stress_benchmark PRIVATE JobQ::Core)

add_executable(jobq_contention_benchmark
    benchmarks/contention_benchmark.cpp
)

target_link_libraries(jobq_contention_benchmark PRIVATE JobQ::Core)

add_executable(jobq_cli
    src/cli/main.cpp
    src/cli/CommandLine.cpp
    src/cli/CommandHandlers.cpp
)

target_link_libraries(jobq_cli PRIVATE JobQ::Core)

include(CTest)

if(BUILD_TESTING)

    add_executable(jobq_public_api_test
        tests/public_api_test.cpp
    )

    target_link_libraries(jobq_public_api_test PRIVATE JobQ::API)

    add_test(
        NAME jobq_public_api
        COMMAND jobq_public_api_test
    )

    set_tests_properties(jobq_public_api PROPERTIES TIMEOUT 30)

    add_executable(jobq_core_usage_test
        tests/core_library_usage_test.cpp
    )

    target_link_libraries(jobq_core_usage_test PRIVATE JobQ::Core)

    # Verify that the library raises a consumer's C++ requirement to C++20.
    set_target_properties(jobq_core_usage_test PROPERTIES CXX_STANDARD 17)

    add_test(
        NAME jobq_core_library_usage
        COMMAND jobq_core_usage_test
    )

    add_executable(jobq_manager_test
        tests/job_manager_test.cpp
    )

    target_link_libraries(jobq_manager_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_manager_lifecycle
        COMMAND jobq_manager_test
    )

    add_executable(jobq_priority_test
        tests/job_priority_test.cpp
    )

    target_link_libraries(jobq_priority_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_priority_scheduling
        COMMAND jobq_priority_test
    )

    add_executable(jobq_retry_policy_test
        tests/job_retry_policy_test.cpp
    )

    target_link_libraries(jobq_retry_policy_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_retry_policy
        COMMAND jobq_retry_policy_test
    )

    add_executable(jobq_retry_execution_test
        tests/job_retry_execution_test.cpp
    )

    target_link_libraries(jobq_retry_execution_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_retry_execution
        COMMAND jobq_retry_execution_test
    )

    add_executable(jobq_retry_backoff_test
        tests/job_retry_backoff_test.cpp
    )

    target_link_libraries(jobq_retry_backoff_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_retry_backoff
        COMMAND jobq_retry_backoff_test
    )

    add_executable(jobq_storage_record_test
        tests/job_record_test.cpp
    )

    target_link_libraries(jobq_storage_record_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_storage_record
        COMMAND jobq_storage_record_test
    )

    add_executable(jobq_file_storage_test
        tests/job_file_storage_test.cpp
    )

    target_link_libraries(jobq_file_storage_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_file_storage
        COMMAND jobq_file_storage_test
    )

    add_executable(jobq_manager_persistence_test
        tests/job_manager_persistence_test.cpp
    )

    target_link_libraries(jobq_manager_persistence_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_manager_persistence
        COMMAND jobq_manager_persistence_test
    )

    add_executable(jobq_recovery_test
        tests/job_recovery_test.cpp
    )

    target_link_libraries(jobq_recovery_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_crash_recovery
        COMMAND jobq_recovery_test
    )

    add_executable(jobq_persistence_recovery_regression_test
        tests/job_persistence_recovery_regression_test.cpp
    )

    target_link_libraries(
        jobq_persistence_recovery_regression_test
        PRIVATE JobQ::Core
    )

    add_test(
        NAME jobq_persistence_recovery_regression
        COMMAND jobq_persistence_recovery_regression_test
    )

    add_executable(jobq_cli_test
        tests/cli_foundation_test.cpp
        src/cli/CommandLine.cpp
    )

    target_link_libraries(jobq_cli_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_cli_foundation
        COMMAND jobq_cli_test
    )

    add_executable(jobq_cli_submit_test
        tests/cli_submit_test.cpp
        src/cli/CommandLine.cpp
        src/cli/CommandHandlers.cpp
    )

    target_link_libraries(jobq_cli_submit_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_cli_submit
        COMMAND jobq_cli_submit_test
    )

    add_executable(jobq_cli_inspection_test
        tests/cli_inspection_test.cpp
        src/cli/CommandLine.cpp
        src/cli/CommandHandlers.cpp
    )

    target_link_libraries(
        jobq_cli_inspection_test
        PRIVATE JobQ::Core
    )

    add_test(
        NAME jobq_cli_inspection
        COMMAND jobq_cli_inspection_test
    )

    add_executable(jobq_cli_run_test
        tests/cli_run_test.cpp
        src/cli/CommandLine.cpp
        src/cli/CommandHandlers.cpp
    )

    target_link_libraries(jobq_cli_run_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_cli_run
        COMMAND jobq_cli_run_test
    )

    add_executable(jobq_cli_workflow_test
        tests/cli_workflow_test.cpp
    )

    target_link_libraries(jobq_cli_workflow_test PRIVATE JobQ::Core)

    add_dependencies(
        jobq_cli_workflow_test
        jobq_cli
    )

    add_test(
        NAME jobq_cli_workflow
        COMMAND jobq_cli_workflow_test
            "$<TARGET_FILE:jobq_cli>"
    )

    add_executable(jobq_metrics_test
        tests/job_metrics_test.cpp
    )

    target_link_libraries(jobq_metrics_test PRIVATE
        JobQ::Core
    )

    add_test(
        NAME jobq_metrics_model
        COMMAND jobq_metrics_test
    )

    add_executable(jobq_cli_metrics_test
        tests/cli_metrics_test.cpp
        src/cli/CommandLine.cpp
        src/cli/CommandHandlers.cpp
    )

    target_link_libraries(jobq_cli_metrics_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_cli_metrics
        COMMAND jobq_cli_metrics_test
    )

    add_executable(jobq_metrics_integration_test
        tests/job_metrics_integration_test.cpp
    )

    target_link_libraries(jobq_metrics_integration_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_metrics_integration
        COMMAND jobq_metrics_integration_test
    )

    add_executable(jobq_metrics_json_test
        tests/job_metrics_json_test.cpp
        src/cli/CommandLine.cpp
        src/cli/CommandHandlers.cpp
    )

    target_link_libraries(jobq_metrics_json_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_metrics_json
        COMMAND jobq_metrics_json_test
    )

    add_executable(jobq_worker_shutdown_test
        tests/worker_shutdown_test.cpp
    )

    target_link_libraries(jobq_worker_shutdown_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_worker_shutdown
        COMMAND jobq_worker_shutdown_test
    )

    add_executable(jobq_worker_lifecycle_test
        tests/worker_lifecycle_test.cpp
    )

    target_link_libraries(jobq_worker_lifecycle_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_worker_lifecycle
        COMMAND jobq_worker_lifecycle_test
    )

    add_executable(jobq_queue_reliability_test
        tests/job_queue_reliability_test.cpp
    )

    target_link_libraries(jobq_queue_reliability_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_queue_reliability
        COMMAND jobq_queue_reliability_test
    )

    add_executable(jobq_recovery_hardening_test
        tests/job_recovery_hardening_test.cpp
    )

    target_link_libraries(jobq_recovery_hardening_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_recovery_hardening
        COMMAND jobq_recovery_hardening_test
    )

    add_executable(jobq_fault_injection_test
        tests/fault_injection_test.cpp
    )

    target_include_directories(jobq_fault_injection_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    target_link_libraries(jobq_fault_injection_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_fault_injection
        COMMAND jobq_fault_injection_test
    )

    add_executable(jobq_latency_statistics_test
        tests/job_latency_statistics_test.cpp
    )

    target_link_libraries(jobq_latency_statistics_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_latency_statistics
        COMMAND jobq_latency_statistics_test
    )

    add_executable(jobq_worker_scaling_statistics_test
        tests/worker_scaling_statistics_test.cpp
    )

    target_include_directories(jobq_worker_scaling_statistics_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
    )

    add_test(
        NAME jobq_worker_scaling_statistics
        COMMAND jobq_worker_scaling_statistics_test
    )

    add_executable(jobq_queue_stress_test
        tests/job_queue_stress_test.cpp
    )

    target_include_directories(jobq_queue_stress_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
    )

    target_link_libraries(jobq_queue_stress_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_queue_stress
        COMMAND jobq_queue_stress_test
    )

    add_executable(jobq_contention_metrics_test
        tests/contention_metrics_test.cpp
    )

    target_link_libraries(jobq_contention_metrics_test PRIVATE JobQ::Core)

    add_test(
        NAME jobq_contention_metrics
        COMMAND jobq_contention_metrics_test
    )

endif()
