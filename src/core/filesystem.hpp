#pragma once
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace pubg_vision::core {
// create_directory atomically reserves names across threads and processes.
inline std::filesystem::path unique_directory(const std::filesystem::path& root, const std::string& prefix) {
    std::filesystem::create_directories(root);
    for (unsigned suffix = 0; suffix < 10000; ++suffix) {
        auto path = root / (prefix + '_' + std::to_string(suffix));
        if (std::filesystem::create_directory(path)) return path;
    }
    throw std::runtime_error("could not reserve a unique output directory");
}
inline std::int64_t utc_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace pubg_vision::core
