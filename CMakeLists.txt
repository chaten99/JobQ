
cmake_minimum_required(VERSION 3.20)

project(JobQ VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

configure_file(
    cmake/Version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/jobq/Version.h"
    @ONLY
)

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

set_target_properties(jobq_core PROPERTIES EXPORT_NAME Core VERSION ${PROJECT_VERSION})

target_include_directories(jobq_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(jobq_core PUBLIC cxx_std_20)
target_link_libraries(jobq_core PUBLIC Threads::Threads)

add_library(jobq_api STATIC
    src/api/Engine.cpp
)

add_library(JobQ::API ALIAS jobq_api)
add_library(JobQ::JobQ ALIAS jobq_api)

set_target_properties(jobq_api PROPERTIES EXPORT_NAME JobQ VERSION ${PROJECT_VERSION})

target_link_libraries(jobq_api PUBLIC JobQ::Core)

include(CMakePackageConfigHelpers)

set(JOBQ_BUILD_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/cmake/JobQ")
set(JOBQ_INSTALL_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/JobQ")

configure_package_config_file(
    cmake/JobQConfig.cmake.in
    "${JOBQ_BUILD_PACKAGE_DIR}/JobQConfig.cmake"
    INSTALL_DESTINATION cmake/JobQ
)

write_basic_package_version_file(
    "${JOBQ_BUILD_PACKAGE_DIR}/JobQConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(TARGETS jobq_core jobq_api
    EXPORT JobQTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY
    include/jobq
    include/job
    include/queue
    include/executor
    include/worker
    include/manager
    include/storage
    include/metrics
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
    PATTERN "MeasuredLock.h" EXCLUDE
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/include/jobq/Version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/jobq
)

configure_package_config_file(
    cmake/JobQConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/install/JobQConfig.cmake"
    INSTALL_DESTINATION ${JOBQ_INSTALL_PACKAGE_DIR}
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/install/JobQConfig.cmake"
    "${JOBQ_BUILD_PACKAGE_DIR}/JobQConfigVersion.cmake"
    DESTINATION ${JOBQ_INSTALL_PACKAGE_DIR}
)

install(EXPORT JobQTargets
    FILE JobQTargets.cmake
    NAMESPACE JobQ::
    DESTINATION ${JOBQ_INSTALL_PACKAGE_DIR}
)

export(EXPORT JobQTargets
    FILE "${JOBQ_BUILD_PACKAGE_DIR}/JobQTargets.cmake"
    NAMESPACE JobQ::
)

option(JOBQ_BUILD_EXAMPLES "Build JobQ developer examples" ON)

if(JOBQ_BUILD_EXAMPLES)
    add_executable(jobq_public_api_example examples/public_api_example.cpp)
    target_link_libraries(jobq_public_api_example PRIVATE JobQ::JobQ)

    foreach(example IN ITEMS basic-usage multiple-handlers job-options error-handling)
        string(REPLACE "-" "_" example_name "${example}")
        add_executable(jobq_example_${example_name} examples/${example}/main.cpp)
        target_link_libraries(jobq_example_${example_name} PRIVATE JobQ::JobQ)
    endforeach()
endif()

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

add_library(jobq_cli_runtime STATIC
    src/cli/InteractiveCommand.cpp
    src/cli/CommandDispatcher.cpp
    src/cli/InteractiveRuntime.cpp
    src/cli/InspectionCommands.cpp
    src/cli/JobCommands.cpp
    src/cli/MetricsCommands.cpp
    src/cli/TerminalSession.cpp
)

target_link_libraries(jobq_cli_runtime PUBLIC JobQ::JobQ)

add_executable(jobq_cli
    src/cli/main.cpp
    src/cli/CommandLine.cpp
    src/cli/CommandHandlers.cpp
)

target_link_libraries(jobq_cli PRIVATE jobq_cli_runtime)

include(CTest)

option(JOBQ_BUILD_SERVER "Build the Drogon HTTP server" OFF)
if(JOBQ_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(BUILD_TESTING)
    add_executable(jobq_backend_contract_test tests/backend/BackendContractTest.cpp)
    target_link_libraries(jobq_backend_contract_test PRIVATE JobQ::Core)
    add_test(NAME jobq_backend_contract COMMAND jobq_backend_contract_test)
    set_tests_properties(jobq_backend_contract PROPERTIES TIMEOUT 30)


    add_executable(jobq_cli_end_to_end_test tests/cli_end_to_end_test.cpp)
    target_link_libraries(jobq_cli_end_to_end_test PRIVATE jobq_cli_runtime)
    add_test(NAME jobq_cli_end_to_end COMMAND jobq_cli_end_to_end_test)
    set_tests_properties(jobq_cli_end_to_end PROPERTIES TIMEOUT 30 LABELS "cli;integration")

    add_executable(jobq_cli_lifecycle_test tests/cli_lifecycle_test.cpp)
    target_link_libraries(jobq_cli_lifecycle_test PRIVATE jobq_cli_runtime)
    add_test(NAME jobq_cli_lifecycle COMMAND jobq_cli_lifecycle_test)
    set_tests_properties(jobq_cli_lifecycle PROPERTIES TIMEOUT 30)

    if(WIN32)
        add_executable(jobq_cli_windows_signals_test tests/cli_windows_signals_test.cpp)
        target_link_libraries(jobq_cli_windows_signals_test PRIVATE jobq_cli_runtime)
        add_test(NAME jobq_cli_windows_signals COMMAND jobq_cli_windows_signals_test)
        set_tests_properties(jobq_cli_windows_signals PROPERTIES TIMEOUT 60)
    elseif(UNIX)
        add_executable(jobq_cli_posix_signals_test tests/cli_posix_signals_test.cpp)
        target_link_libraries(jobq_cli_posix_signals_test PRIVATE jobq_cli_runtime)
        add_test(NAME jobq_cli_posix_signals COMMAND jobq_cli_posix_signals_test)
        set_tests_properties(jobq_cli_posix_signals PROPERTIES TIMEOUT 60)
    endif()

    foreach(component IN ITEMS metrics_commands monitoring)
        add_executable(jobq_cli_${component}_test tests/cli_${component}_test.cpp)
        target_link_libraries(jobq_cli_${component}_test PRIVATE jobq_cli_runtime)
        add_test(NAME jobq_cli_${component} COMMAND jobq_cli_${component}_test)
        set_tests_properties(jobq_cli_${component} PROPERTIES TIMEOUT 30)
    endforeach()

    add_executable(jobq_cli_job_commands_test tests/cli_job_commands_test.cpp)
    target_link_libraries(jobq_cli_job_commands_test PRIVATE jobq_cli_runtime)
    add_test(NAME jobq_cli_job_commands COMMAND jobq_cli_job_commands_test)
    set_tests_properties(jobq_cli_job_commands PROPERTIES TIMEOUT 30)

    add_executable(jobq_public_api_job_lookup_test tests/public_api/job_lookup_test.cpp)
    target_link_libraries(jobq_public_api_job_lookup_test PRIVATE JobQ::JobQ)
    add_test(NAME jobq_public_api_job_lookup COMMAND jobq_public_api_job_lookup_test)
    set_tests_properties(jobq_public_api_job_lookup PROPERTIES TIMEOUT 30)

    add_executable(jobq_cli_inspection_commands_test tests/cli_inspection_commands_test.cpp)
    target_link_libraries(jobq_cli_inspection_commands_test PRIVATE jobq_cli_runtime)
    add_test(NAME jobq_cli_inspection_commands COMMAND jobq_cli_inspection_commands_test)
    set_tests_properties(jobq_cli_inspection_commands PROPERTIES TIMEOUT 30)

    foreach(component IN ITEMS parser dispatcher runtime)
        add_executable(jobq_interactive_${component}_test tests/interactive_${component}_test.cpp)
        target_link_libraries(jobq_interactive_${component}_test PRIVATE jobq_cli_runtime)
        add_test(NAME jobq_interactive_${component} COMMAND jobq_interactive_${component}_test)
        set_tests_properties(jobq_interactive_${component} PROPERTIES TIMEOUT 20)
    endforeach()

    add_test(NAME jobq_interactive_session
        COMMAND ${CMAKE_COMMAND}
            "-DJOBQ_CLI=$<TARGET_FILE:jobq_cli>"
            "-DJOBQ_SESSION_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/interactive-sessions"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/interactive_cli_session_test.cmake"
    )
    set_tests_properties(jobq_interactive_session PROPERTIES TIMEOUT 70)

    add_executable(jobq_version_test tests/version_consumer/main.cpp)
    target_link_libraries(jobq_version_test PRIVATE JobQ::JobQ)
    target_compile_definitions(jobq_version_test PRIVATE
        JOBQ_EXPECTED_MAJOR=${PROJECT_VERSION_MAJOR}
        JOBQ_EXPECTED_MINOR=${PROJECT_VERSION_MINOR}
        JOBQ_EXPECTED_PATCH=${PROJECT_VERSION_PATCH}
        JOBQ_EXPECTED_STRING="${PROJECT_VERSION}"
    )
    add_test(NAME jobq_version COMMAND jobq_version_test)

    add_test(NAME jobq_package_version_compatibility
        COMMAND ${CMAKE_COMMAND}
            "-DJOBQ_PACKAGE_DIR=${JOBQ_BUILD_PACKAGE_DIR}"
            "-DJOBQ_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/version-consumers"
            "-DJOBQ_TEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DJOBQ_TEST_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DJOBQ_TEST_CONFIG=$<CONFIG>"
            "-DJOBQ_TEST_VERSION=${PROJECT_VERSION}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/package_version_test.cmake"
    )
    set_tests_properties(jobq_package_version_compatibility PROPERTIES TIMEOUT 120)

    add_executable(jobq_public_api_test
        tests/public_api/engine_test.cpp
    )

    target_link_libraries(jobq_public_api_test PRIVATE JobQ::API)

    add_test(
        NAME jobq_public_api
        COMMAND jobq_public_api_test
    )

    set_tests_properties(jobq_public_api PROPERTIES TIMEOUT 30)

    add_executable(jobq_public_api_edge_test tests/public_api/edge_cases_test.cpp)
    target_link_libraries(jobq_public_api_edge_test PRIVATE JobQ::JobQ)
    add_test(NAME jobq_public_api_edge_cases COMMAND jobq_public_api_edge_test)
    set_tests_properties(jobq_public_api_edge_cases PROPERTIES TIMEOUT 30)

    if(JOBQ_BUILD_EXAMPLES)
        foreach(example IN ITEMS basic_usage multiple_handlers job_options error_handling)
            add_test(NAME jobq_example_${example} COMMAND jobq_example_${example})
            set_tests_properties(jobq_example_${example} PROPERTIES TIMEOUT 15)
        endforeach()
    endif()

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
