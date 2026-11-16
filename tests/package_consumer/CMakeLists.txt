cmake_minimum_required(VERSION 3.20)

project(JobQPackageConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(JobQ 0.1 CONFIG REQUIRED)
find_package(JobQ 0.1 CONFIG REQUIRED)

foreach(required_target IN ITEMS JobQ::JobQ JobQ::Core JobQ::API Threads::Threads)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR "Missing package target ${required_target}")
    endif()
endforeach()

add_executable(jobq_package_consumer main.cpp)
target_link_libraries(jobq_package_consumer PRIVATE JobQ::JobQ)

enable_testing()
add_test(NAME jobq_package_consumer COMMAND jobq_package_consumer)
