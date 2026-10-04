#include "core/performance_clock.hpp"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <chrono>
#endif
namespace pubg_vision::core {
std::int64_t qpc_ticks() noexcept {
#ifdef _WIN32
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
std::int64_t qpc_frequency() noexcept {
#ifdef _WIN32
    LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart;
#else
    return 1000000000;
#endif
}
}
