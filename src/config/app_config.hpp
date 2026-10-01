#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "core/geometry.hpp"
#include "collection/capture_scheduler.hpp"

namespace pubg_vision::config {

struct AppConfig {
    core::Size roi{640, 640};
    std::filesystem::path output{"data/raw"};
    std::string window_title;
    bool verbose{false};
    bool show_help{false};
    bool show_version{false};
    bool capture_once{false};
    bool list_windows{false};
    bool collect{false};
    collection::Settings collection;
    std::size_t writer_capacity{16};
};

struct ParseResult {
    AppConfig config;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

[[nodiscard]] ParseResult parse_arguments(std::span<const std::string_view> args);
[[nodiscard]] std::string_view help_text() noexcept;

} // namespace pubg_vision::config
