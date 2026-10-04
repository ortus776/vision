#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "core/geometry.hpp"

namespace pubg_vision::core {
struct Frame {
    Size size;
    Size source_size;
    Rect roi; // Absolute physical desktop coordinates.
    std::vector<std::uint8_t> pixels; // Owned packed BGRA, opaque alpha.
    std::uint64_t generation{};
    std::int64_t captured_ms{};
    std::int64_t captured_utc_ms{};
    std::optional<std::int64_t> source_qpc; // DXGI LastPresentTime, not a UTC timestamp.
    std::optional<std::int64_t> capture_end_qpc; // CPU capture completion, for writer queue timing.
};
} // namespace pubg_vision::core
