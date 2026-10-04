#pragma once
#include <cstdint>
namespace pubg_vision::core {
// Windows QPC is the same clock domain used by PresentMon --qpc_time.
[[nodiscard]] std::int64_t qpc_ticks() noexcept;
[[nodiscard]] std::int64_t qpc_frequency() noexcept;
}
