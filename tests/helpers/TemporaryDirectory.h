#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace jobq_test {

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& label) {
        static std::atomic<unsigned> counter{0};

        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();

        path_ = std::filesystem::temp_directory_path() /
                ("jobq_fault_" + label + "_" +
                 std::to_string(stamp) + "_" +
                 std::to_string(counter.fetch_add(1)));

        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path journal() const {
        return path_ / "jobs.journal";
    }

private:
    std::filesystem::path path_;
};

}
