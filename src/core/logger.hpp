#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string_view>

namespace pubg_vision::core {

enum class LogLevel { info, warning, error, debug };

class Logger {
public:
    explicit Logger(bool verbose = false) : verbose_(verbose) {}

    void write(LogLevel level, std::string_view message) const {
        if (level == LogLevel::debug && !verbose_) {
            return;
        }

        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local_time{};
#ifdef _WIN32
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif

        std::scoped_lock lock(mutex_);
        auto& stream = level == LogLevel::error || level == LogLevel::warning
                           ? std::cerr
                           : std::cout;
        const auto* label = "INFO";
        switch (level) {
        case LogLevel::info: label = "INFO"; break;
        case LogLevel::warning: label = "WARN"; break;
        case LogLevel::error: label = "ERROR"; break;
        case LogLevel::debug: label = "DEBUG"; break;
        }
        stream << '[' << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << "] ["
               << label << "] " << message << '\n';
    }

private:
    bool verbose_;
    inline static std::mutex mutex_;
};

} // namespace pubg_vision::core
