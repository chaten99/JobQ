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
endif()