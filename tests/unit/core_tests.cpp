#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <string_view>

#include "config/app_config.hpp"
#include "core/geometry.hpp"

namespace {

int failures = 0;

void expect(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

template <typename Exception, typename Callable>
void expect_throws(Callable&& callable, std::string_view name) {
    try {
        callable();
        expect(false, name);
    } catch (const Exception&) {
        expect(true, name);
    } catch (...) {
        expect(false, name);
    }
}

void test_centered_roi() {
    using pubg_vision::core::Rect;
    using pubg_vision::core::Size;
    const auto roi = pubg_vision::core::centered_rect(Rect{0, 0, 1920, 1080}, Size{640, 640});
    expect(roi.left == 640 && roi.top == 220 && roi.width == 640 && roi.height == 640,
           "centers 640x640 ROI in 1920x1080 source");

    const auto offset_roi = pubg_vision::core::centered_rect(
        Rect{-1920, 100, 1920, 1080}, Size{640, 640});
    expect(offset_roi.left == -1280 && offset_roi.top == 320,
           "supports source with a negative virtual-desktop origin");

    const auto odd_roi = pubg_vision::core::centered_rect(Rect{0, 0, 5, 7}, Size{2, 2});
    expect(odd_roi.left == 1 && odd_roi.top == 2,
           "rounds odd center remainder toward top-left");

    expect_throws<std::out_of_range>(
        [] { (void)pubg_vision::core::centered_rect({0, 0, 640, 640}, {641, 640}); },
        "rejects ROI wider than source");
    expect_throws<std::invalid_argument>(
        [] { (void)pubg_vision::core::centered_rect({0, 0, 0, 640}, {100, 100}); },
        "rejects invalid source dimensions");
    expect_throws<std::invalid_argument>(
        [] { (void)pubg_vision::core::centered_rect({0, 0, 640, 640}, {0, 100}); },
        "rejects invalid ROI dimensions");
    expect_throws<std::out_of_range>(
        [] { (void)pubg_vision::core::centered_rect(
                  {std::numeric_limits<std::int32_t>::max() - 4, 0, 16, 4}, {2, 2}); },
        "rejects ROI origin overflow");
}

void test_relative_rect() {
    using pubg_vision::core::Rect;
    const auto local = pubg_vision::core::relative_rect(
        Rect{-1180, 320, 640, 640}, Rect{-1920, 0, 1920, 1080});
    expect(local.left == 740 && local.top == 320 && local.width == 640 && local.height == 640,
           "converts virtual-desktop ROI to monitor-local crop coordinates");
    expect_throws<std::out_of_range>(
        [] { (void)pubg_vision::core::relative_rect({-100, 0, 20, 20}, {0, 0, 1920, 1080}); },
        "rejects crop outside selected monitor");
}

void test_cli_configuration() {
    using pubg_vision::config::parse_arguments;

    const auto defaults = parse_arguments({});
    expect(static_cast<bool>(defaults), "accepts empty arguments");
    expect(defaults.config.roi.width == 640 && defaults.config.roi.height == 640,
           "uses 640x640 default ROI");
    expect(defaults.config.output == "data/raw", "uses default output directory");

    constexpr std::array<std::string_view, 5> custom_args{
        "--roi-width", "800", "--roi-height", "600", "--output"};
    const auto missing_output = parse_arguments(custom_args);
    expect(!missing_output && missing_output.error.find("missing value") != std::string::npos,
           "rejects missing option value");

    constexpr std::array<std::string_view, 6> complete_args{
        "--roi-width", "800", "--roi-height", "600", "--output", "frames"};
    const auto custom = parse_arguments(complete_args);
    expect(static_cast<bool>(custom), "accepts valid custom configuration");
    expect(custom.config.roi.width == 800 && custom.config.roi.height == 600,
           "parses custom ROI dimensions");
    expect(custom.config.output == "frames", "parses custom output directory");

    constexpr std::array<std::string_view, 2> zero_args{"--roi-width", "0"};
    expect(!parse_arguments(zero_args), "rejects zero ROI dimension");
    constexpr std::array<std::string_view, 2> overflow_args{
        "--roi-height", "999999999999999999999"};
    expect(!parse_arguments(overflow_args), "rejects dimension integer overflow");
    constexpr std::array<std::string_view, 1> unknown_args{"--capture"};
    expect(!parse_arguments(unknown_args), "rejects unknown argument");

    constexpr std::array<std::string_view, 2> capture_without_window{"--capture-once", "--verbose"};
    expect(!parse_arguments(capture_without_window), "requires an explicit window for capture");
    constexpr std::array<std::string_view, 3> window_title_without_capture{
        "--list-windows", "--window-title", "PUBG"};
    expect(!parse_arguments(window_title_without_capture),
           "rejects a window title when no capture action is selected");
    constexpr std::array<std::string_view, 4> mutually_exclusive_args{
        "--capture-once", "--window-title", "PUBG", "--list-windows"};
    expect(!parse_arguments(mutually_exclusive_args), "rejects conflicting capture actions");
    constexpr std::array<std::string_view, 3> valid_capture_args{
        "--capture-once", "--window-title", "PUBG"};
    const auto valid_capture = parse_arguments(valid_capture_args);
    expect(static_cast<bool>(valid_capture) && valid_capture.config.capture_once &&
               valid_capture.config.window_title == "PUBG",
           "accepts an explicit window for one-shot capture");
    constexpr std::array<std::string_view, 7> collect_args{
        "--collect", "--window-title", "PUBG", "--writer-capacity", "2", "--merge-ms", "0"};
    const auto collect = parse_arguments(collect_args);
    expect(collect && collect.config.collect && collect.config.writer_capacity == 2 &&
           collect.config.collection.merge_window == 0, "accepts bounded collector settings");
    constexpr std::array<std::string_view, 4> conflict{
        "--collect", "--capture-once", "--window-title", "PUBG"};
    expect(!parse_arguments(conflict), "collector excludes one-shot capture");
    constexpr std::array<std::string_view, 2> help{"--collect", "--help"};
    expect(static_cast<bool>(parse_arguments(help)), "help does not require a capture target");
    constexpr std::array<std::string_view, 2> capacity{"--writer-capacity", "4097"};
    expect(!parse_arguments(capacity), "rejects unreasonable queue size");
}

} // namespace

int main() {
    test_centered_roi();
    test_relative_rect();
    test_cli_configuration();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
