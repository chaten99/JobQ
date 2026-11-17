#pragma once

#include <cstddef>
#include <filesystem>

namespace jobq {

struct EngineConfig {
    std::size_t workerCount{4};
    std::filesystem::path storagePath{};
};

}
