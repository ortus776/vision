#pragma once
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace pubg_vision::core {
// Dataset sessions repeat their unique ID in image filenames. Nested experiment
// folders can exceed MAX_PATH; use the extended Win32 namespace for file IO.
inline std::filesystem::path native_output_path(const std::filesystem::path& path) {
#ifdef _WIN32
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    absolute.make_preferred();
    const auto& text = absolute.native();
    if (text.starts_with(L"\\\\?\\")) return absolute;
    if (text.starts_with(L"\\\\")) return std::filesystem::path(L"\\\\?\\UNC\\" + text.substr(2));
    return std::filesystem::path(L"\\\\?\\" + text);
#else
    return path;
#endif
}
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
