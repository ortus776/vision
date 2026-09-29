#include "config/app_config.hpp"

#include <charconv>
#include <limits>
#include <system_error>

namespace pubg_vision::config {
namespace {

bool parse_dimension(std::string_view text, std::int32_t& output) {
    std::int32_t value{};
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != last || value <= 0) {
        return false;
    }
    output = value;
    return true;
}

} // namespace

ParseResult parse_arguments(std::span<const std::string_view> args) {
    ParseResult result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--help" || arg == "-h") {
            result.config.show_help = true;
        } else if (arg == "--version") {
            result.config.show_version = true;
        } else if (arg == "--capture-once") {
            result.config.capture_once = true;
        } else if (arg == "--list-windows") {
            result.config.list_windows = true;
        } else if (arg == "--verbose") {
            result.config.verbose = true;
        } else if (arg == "--roi-width" || arg == "--roi-height" || arg == "--output" ||
                   arg == "--window-title") {
            if (i + 1 >= args.size()) {
                result.error = "missing value for " + std::string(arg);
                return result;
            }

            const auto value = args[++i];
            if (value.empty() || value.starts_with("--")) {
                result.error = "missing value for " + std::string(arg);
                return result;
            }
            if (arg == "--roi-width") {
                if (!parse_dimension(value, result.config.roi.width)) {
                    result.error = "--roi-width must be a positive 32-bit integer";
                    return result;
                }
            } else if (arg == "--roi-height") {
                if (!parse_dimension(value, result.config.roi.height)) {
                    result.error = "--roi-height must be a positive 32-bit integer";
                    return result;
                }
            } else if (arg == "--output") {
                result.config.output = std::filesystem::path(value);
            } else {
                result.config.window_title = value;
            }
        } else {
            result.error = "unknown argument: " + std::string(arg);
            return result;
        }
    }

    if (result.config.capture_once && result.config.list_windows) {
        result.error = "--capture-once and --list-windows cannot be used together";
    } else if (result.config.capture_once && result.config.window_title.empty()) {
        result.error = "--capture-once requires --window-title; use --list-windows to inspect visible windows";
    } else if (!result.config.window_title.empty() && !result.config.capture_once) {
        result.error = "--window-title requires --capture-once";
    }

    return result;
}

std::string_view help_text() noexcept {
    return R"(PUBG Vision — C++/ML learning project

Current milestone: one-shot central desktop capture to PNG.

Usage:
  pubg_vision_app [options]

Options:
  --list-windows        List visible top-level windows and their client sizes
  --capture-once        Capture one central crop and save it as a PNG
  --window-title <text> Case-sensitive substring used to choose one window
  --roi-width <pixels>   Configured ROI width (default: 640)
  --roi-height <pixels>  Configured ROI height (default: 640)
  --output <directory>   Output directory (default: data/raw)
  --verbose              Enable debug logging
  --version              Print the application version
  --help, -h             Print this help
)";
}

} // namespace pubg_vision::config
