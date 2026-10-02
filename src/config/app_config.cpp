#include "config/app_config.hpp"

#include <charconv>
#include <cmath>
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
        } else if (arg == "--collect") {
            result.config.collect = true;
        } else if (arg == "--live") {
            result.config.live = true;
        } else if (arg == "--inference-demo") {
            result.config.inference_demo = true;
        } else if (arg == "--synthetic-source") {
            result.config.synthetic_source = true;
        } else if (arg == "--start-active") {
            result.config.start_active = true;
        } else if (arg == "--list-windows") {
            result.config.list_windows = true;
        } else if (arg == "--verbose") {
            result.config.verbose = true;
        } else if (arg == "--roi-width" || arg == "--roi-height" || arg == "--output" ||
                   arg == "--window-title" || arg == "--periodic-ms" ||
                   arg == "--merge-ms" || arg == "--max-lateness-ms" ||
                   arg == "--max-pending" || arg == "--writer-capacity" ||
                   arg == "--input-width" || arg == "--input-height" || arg == "--inference-fps" ||
                   arg == "--result-ttl-ms" || arg == "--confidence" || arg == "--nms-iou" ||
                   arg == "--mock-seed" || arg == "--demo-frames" || arg == "--backend" || arg == "--overlay-style") {
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
                result.config.output = std::filesystem::path(std::u8string(
                    reinterpret_cast<const char8_t*>(value.data()), value.size()));
            } else if (arg == "--window-title") {
                result.config.window_title = value;
            } else if (arg == "--backend") {
                if (value != "mock") { result.error = "only --backend mock is implemented; connect your model through inference::Model"; return result; }
            } else if (arg == "--overlay-style") {
                if (value == "box") result.config.overlay_style = render::Style::box;
                else if (value == "point") result.config.overlay_style = render::Style::point;
                else { result.error = "--overlay-style must be box or point"; return result; }
            } else if (arg == "--confidence" || arg == "--nms-iou") {
                float number{};
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    !std::isfinite(number) || number < 0 || number > 1) {
                    result.error = std::string(arg) + " must be a finite number in 0..1"; return result;
                }
                if (arg == "--confidence") result.config.inference.postprocess.confidence_threshold = number;
                else result.config.inference.postprocess.nms_iou = number;
            } else if (arg == "--mock-seed") {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.config.mock_seed);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                    result.error = "--mock-seed must be an unsigned 32-bit integer"; return result;
                }
            } else {
                std::int32_t number{};
                const bool allow_zero = arg == "--merge-ms" || arg == "--max-lateness-ms";
                if (!(allow_zero && value == "0") && !parse_dimension(value, number)) {
                    result.error = "invalid integer for " + std::string(arg);
                    return result;
                }
                const auto limit = (arg == "--input-width" || arg == "--input-height") ? 2048 :
                    (arg == "--inference-fps" ? 120 : (arg == "--demo-frames" ? 1000 :
                    (arg == "--periodic-ms" ? 3600000 :
                    (arg == "--max-pending" || arg == "--writer-capacity" ? 4096 : 60000))));
                if (number > limit) {
                    result.error = "value exceeds limit for " + std::string(arg);
                    return result;
                }
                if (arg == "--periodic-ms") result.config.collection.periodic_interval = number;
                else if (arg == "--merge-ms") result.config.collection.merge_window = number;
                else if (arg == "--max-lateness-ms") result.config.collection.max_lateness = number;
                else if (arg == "--max-pending") result.config.collection.max_pending = static_cast<std::size_t>(number);
                else if (arg == "--input-width") result.config.model.input_size.width = number;
                else if (arg == "--input-height") result.config.model.input_size.height = number;
                else if (arg == "--inference-fps") result.config.inference.fps = number;
                else if (arg == "--result-ttl-ms") result.config.inference.ttl_ms = number;
                else if (arg == "--demo-frames") result.config.demo_frames = number;
                else result.config.writer_capacity = static_cast<std::size_t>(number);
            }
        } else {
            result.error = "unknown argument: " + std::string(arg);
            return result;
        }
    }

    if (result.config.show_help || result.config.show_version) return result;
    if (result.config.synthetic_source && !result.config.live) {
        result.error = "--synthetic-source requires --live"; return result;
    }
    if (result.config.start_active && !result.config.collect) {
        result.error = "--start-active requires --collect";
        return result;
    }
    if (static_cast<int>(result.config.capture_once) + static_cast<int>(result.config.collect) +
        static_cast<int>(result.config.live) + static_cast<int>(result.config.inference_demo) +
        static_cast<int>(result.config.list_windows) > 1) {
        result.error = "--capture-once, --collect, --live, --inference-demo and --list-windows are mutually exclusive";
    } else if ((result.config.capture_once || result.config.collect || result.config.live) && result.config.window_title.empty()) {
        result.error = "capture requires --window-title; use --list-windows to inspect visible windows";
    } else if (!result.config.window_title.empty() && !result.config.capture_once && !result.config.collect && !result.config.live) {
        result.error = "--window-title requires --capture-once, --collect or --live";
    }

    return result;
}

std::string_view help_text() noexcept {
    return R"(PUBG Vision — C++/ML learning project

Current milestone: dataset collection and mock inference/overlay architecture.

Usage:
  pubg_vision_app [options]

Options:
  --list-windows        List visible top-level windows and their client sizes
  --capture-once        Capture one central crop and save it as a PNG
  --collect             Collect a dataset session (F8: start/pause, F9: stop)
  --start-active        Enable collection at launch; it starts when selected window has focus
  --live                Live capture -> mock model -> transparent overlay (F8: pause, F9: stop)
  --inference-demo      Render mock detections on synthetic frames to PNG, without desktop access
  --synthetic-source    In --live, generate test frames so overlay also animates on a static window
  --backend <name>      Currently mock; model integration uses inference::Model
  --input-width <px>    Model tensor width (default: 416, max: 2048)
  --input-height <px>   Model tensor height (default: 416, max: 2048)
  --inference-fps <n>   Maximum model calls per second (default: 10, max: 120)
  --result-ttl-ms <ms>  Maximum age of displayed detections from capture (default: 500)
  --confidence <0..1>  Detection confidence threshold (default: 0.25)
  --nms-iou <0..1>     Class-aware NMS overlap threshold (default: 0.45)
  --overlay-style <s>  box or point (default: box)
  --mock-seed <n>      Seed for reproducible random rectangles (default: 42)
  --demo-frames <n>    Synthetic PNG count (default: 10, max: 1000)
  --window-title <text> Case-sensitive substring used to choose one window
  --roi-width <pixels>   Configured ROI width (default: 640)
  --roi-height <pixels>  Configured ROI height (default: 640)
  --output <directory>   Output directory (default: data/raw)
  --periodic-ms <ms>     Independent timer interval (default: 5000, max: 3600000)
  --merge-ms <ms>        Merge due requests within this window (default: 20)
  --max-lateness-ms <ms> Skip older requests (default: 100)
  --max-pending <count>  Pending request capacity (default: 32, max: 4096)
  --writer-capacity <n>  Waiting PNG frame capacity (default: 16, max: 4096)
  --verbose              Enable debug logging
  --version              Print the application version
  --help, -h             Print this help
)";
}

} // namespace pubg_vision::config
