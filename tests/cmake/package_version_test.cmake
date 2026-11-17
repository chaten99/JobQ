cmake_minimum_required(VERSION 3.20)

string(REPLACE "." ";" version_parts "${JOBQ_TEST_VERSION}")
list(GET version_parts 0 major)
list(GET version_parts 1 minor)
list(GET version_parts 2 patch)
math(EXPR next_major "${major} + 1")
math(EXPR next_minor "${minor} + 1")
math(EXPR next_patch "${patch} + 1")

function(check_request name requested exact expected)
    set(consumer_build "${JOBQ_TEST_BINARY_DIR}/${name}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${CMAKE_CURRENT_LIST_DIR}/../version_consumer"
            -B "${consumer_build}"
            -G "${JOBQ_TEST_GENERATOR}"
            "-DCMAKE_CXX_COMPILER=${JOBQ_TEST_COMPILER}"
            "-DCMAKE_BUILD_TYPE=${JOBQ_TEST_CONFIG}"
            "-DJobQ_DIR=${JOBQ_PACKAGE_DIR}"
            "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
            "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"
            "-DJOBQ_REQUEST_VERSION=${requested}"
            "-DJOBQ_REQUEST_EXACT=${exact}"
            "-DJOBQ_EXPECT_FOUND=${expected}"
            "-DJOBQ_AVAILABLE_VERSION=${JOBQ_TEST_VERSION}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name} discovery failed\n${output}\n${error}")
    endif()
endfunction()

check_request(unversioned "" OFF ON)
check_request(current "${JOBQ_TEST_VERSION}" OFF ON)
check_request(exact "${JOBQ_TEST_VERSION}" ON ON)
check_request(minor "${major}.${minor}" OFF ON)
check_request(newer_patch "${major}.${minor}.${next_patch}" OFF OFF)
check_request(other_minor "${major}.${next_minor}.0" OFF OFF)
check_request(other_major "${next_major}.0.0" OFF OFF)

if(minor GREATER 0)
    math(EXPR previous_minor "${minor} - 1")
    check_request(older_minor "${major}.${previous_minor}.0" OFF OFF)
endif()

if(patch GREATER 0)
    math(EXPR previous_patch "${patch} - 1")
    check_request(older_patch "${major}.${minor}.${previous_patch}" OFF ON)
    check_request(older_exact "${major}.${minor}.${previous_patch}" ON OFF)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${JOBQ_TEST_BINARY_DIR}/exact"
        --config "${JOBQ_TEST_CONFIG}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Version consumer build failed\n${output}\n${error}")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${JOBQ_TEST_BINARY_DIR}/exact"
        -C "${JOBQ_TEST_CONFIG}" --output-on-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Version consumer execution failed\n${output}\n${error}")
endif()
