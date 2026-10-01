#include <iostream>
#include <string_view>
#include <vector>
#include <string>
#include <cwchar>
#include <exception>
#ifdef _WIN32
#include <windows.h>
#endif

#include "capture/desktop_capture.hpp"
#include "config/app_config.hpp"
#include "core/logger.hpp"
#include "app/collect.hpp"

#ifndef PUBGVISION_VERSION
#define PUBGVISION_VERSION "0.0.0"
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> owned_args;
    owned_args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        const int length = static_cast<int>(std::wcslen(argv[i]));
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], length,
                                            nullptr, 0, nullptr, nullptr);
        if (length && count <= 0) { std::cerr << "Invalid Unicode argument\n"; return 2; }
        std::string value(static_cast<std::size_t>(count), '\0');
        if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], length,
                                       value.data(), count, nullptr, nullptr);
        owned_args.push_back(std::move(value));
    }
#else
int main(int argc, char* argv[]) {
#endif
    std::vector<std::string_view> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
#ifdef _WIN32
        args.emplace_back(owned_args[static_cast<std::size_t>(i - 1)]);
#else
        args.emplace_back(argv[i]);
#endif
    }

    auto parsed = pubg_vision::config::parse_arguments(args);
    if (!parsed) {
        std::cerr << "Configuration error: " << parsed.error << "\n\n"
                  << pubg_vision::config::help_text();
        return 2;
    }

    if (parsed.config.show_help) {
        std::cout << pubg_vision::config::help_text();
        return 0;
    }
    if (parsed.config.show_version) {
        std::cout << "PUBG Vision " << PUBGVISION_VERSION << '\n';
        return 0;
    }

    const pubg_vision::core::Logger logger(parsed.config.verbose);
    try {
        if (parsed.config.list_windows) {
            pubg_vision::capture::list_windows();
            return 0;
        }
        if (parsed.config.capture_once) {
            pubg_vision::capture::capture_once(parsed.config, logger);
            return 0;
        }
        if (parsed.config.collect) {
            pubg_vision::app::collect(parsed.config, logger);
            return 0;
        }
    } catch (const std::exception& exception) {
        logger.write(pubg_vision::core::LogLevel::error, exception.what());
        return 1;
    }

    logger.write(pubg_vision::core::LogLevel::info,
                 "Use --list-windows, --capture-once or --collect.");

    const auto message = "Configured ROI: " + std::to_string(parsed.config.roi.width) + "x" +
                         std::to_string(parsed.config.roi.height) + "; output directory: " +
                         parsed.config.output.string();
    logger.write(pubg_vision::core::LogLevel::info, message);
    logger.write(pubg_vision::core::LogLevel::debug,
                 "Debug logging is enabled.");
    return 0;
}
