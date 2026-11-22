include(FetchContent)

if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR "The optional HTTP server requires CMake 3.28 or newer")
endif()

function(jobq_fetch_drogon)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    set(BUILD_TESTING OFF)
    set(BUILD_CTL OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_ORM OFF)
    set(BUILD_POSTGRESQL OFF)
    set(BUILD_MYSQL OFF)
    set(BUILD_SQLITE OFF)
    set(BUILD_REDIS OFF)
    set(BUILD_DOC OFF)
    set(BUILD_BROTLI OFF)
    set(BUILD_YAML_CONFIG OFF)
    set(BUILD_C-ARES OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(USE_SUBMODULE ON)
    set(TRANTOR_USE_TLS none)
    FetchContent_Declare(drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG v1.9.14
        GIT_SHALLOW TRUE
        GIT_SUBMODULES trantor
        GIT_SUBMODULES_RECURSE FALSE
        PATCH_COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=<SOURCE_DIR>
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PatchTrantor.cmake"
            COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=<SOURCE_DIR>
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PatchHttpErrors.cmake"
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(drogon)
endfunction()

jobq_fetch_drogon()

add_library(jobq_server_runtime STATIC ServerConfig.cpp ServerRuntime.cpp HealthRoutes.cpp ApiJson.cpp ApiRoutes.cpp)
target_include_directories(jobq_server_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(jobq_server_runtime PUBLIC JobQ::JobQ PRIVATE Drogon::Drogon)
target_compile_features(jobq_server_runtime PUBLIC cxx_std_20)
add_executable(jobq_server main.cpp)
target_link_libraries(jobq_server PRIVATE jobq_server_runtime Drogon::Drogon)

if(BUILD_TESTING)
    add_executable(jobq_rest_api_test tests/RestApiTest.cpp)
    target_include_directories(jobq_rest_api_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(jobq_rest_api_test PRIVATE jobq_server_runtime Drogon::Drogon)
    add_test(NAME jobq_rest_api COMMAND jobq_rest_api_test)
    set_tests_properties(jobq_rest_api PROPERTIES TIMEOUT 30)
    add_executable(jobq_rest_workflow_test tests/RestWorkflowTest.cpp)
    target_include_directories(jobq_rest_workflow_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(jobq_rest_workflow_test PRIVATE jobq_server_runtime Drogon::Drogon)
    add_test(NAME jobq_rest_workflow COMMAND jobq_rest_workflow_test)
    set_tests_properties(jobq_rest_workflow PROPERTIES TIMEOUT 30)
    add_executable(jobq_server_config_test tests/ServerConfigTest.cpp)
    target_include_directories(jobq_server_config_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(jobq_server_config_test PRIVATE jobq_server_runtime Drogon::Drogon)
    add_test(NAME jobq_server_config COMMAND jobq_server_config_test)
    set_tests_properties(jobq_server_config PROPERTIES TIMEOUT 30)
    add_executable(jobq_server_stop_test tests/ServerStopTest.cpp)
    target_link_libraries(jobq_server_stop_test PRIVATE jobq_server_runtime Drogon::Drogon)
    add_test(NAME jobq_server_stop COMMAND jobq_server_stop_test)
    add_test(NAME jobq_server_failures COMMAND "${CMAKE_COMMAND}" -DSERVER=$<TARGET_FILE:jobq_server>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ServerFailures.cmake")
    set_tests_properties(jobq_server_stop jobq_server_failures PROPERTIES TIMEOUT 30)
    if(WIN32)
        add_executable(jobq_server_framework_failure_test tests/FrameworkFailureTest.cpp)
        target_link_libraries(jobq_server_framework_failure_test PRIVATE jobq_server_runtime Drogon::Drogon ws2_32)
        add_test(NAME jobq_server_framework_failure COMMAND jobq_server_framework_failure_test)
        set_tests_properties(jobq_server_framework_failure PROPERTIES TIMEOUT 30)
        add_executable(jobq_server_smoke_test tests/ServerSmokeTest.cpp)
        target_link_libraries(jobq_server_smoke_test PRIVATE Drogon::Drogon ws2_32)
        add_test(NAME jobq_server_smoke COMMAND jobq_server_smoke_test $<TARGET_FILE:jobq_server>)
        set_tests_properties(jobq_server_smoke PROPERTIES TIMEOUT 30)
        add_executable(jobq_rest_test_host tests/RestTestHost.cpp)
        target_link_libraries(jobq_rest_test_host PRIVATE jobq_server_runtime Drogon::Drogon)
        add_test(NAME jobq_rest_process COMMAND jobq_server_smoke_test $<TARGET_FILE:jobq_rest_test_host> --rest)
        set_tests_properties(jobq_rest_process PROPERTIES TIMEOUT 30)
    endif()
endif()
