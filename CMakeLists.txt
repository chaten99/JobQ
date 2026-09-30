cmake_minimum_required(VERSION 3.20)

project(JobQ
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(jobq
    src/main.cpp
    src/job/Job.cpp
    src/queue/JobQueue.cpp
)

target_include_directories(jobq
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)