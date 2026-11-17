cmake_minimum_required(VERSION 3.20)

project(JobQInstalledConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(JobQ 0.1 CONFIG REQUIRED)

add_executable(jobq_installed_consumer main.cpp)
target_link_libraries(jobq_installed_consumer PRIVATE JobQ::JobQ)

enable_testing()
add_test(NAME jobq_installed_consumer COMMAND jobq_installed_consumer)
