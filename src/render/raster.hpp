#pragma once
#include "inference/model.hpp"
#include "core/frame.hpp"
#include <span>

namespace pubg_vision::render {
enum class Style { box, point };
// Transparent premultiplied BGRA surface; detections are local to the ROI.
[[nodiscard]] std::vector<std::uint8_t> rasterize(core::Size size,
    std::span<const inference::Detection> detections, Style style = Style::box);
void composite(core::Frame& frame, std::span<const std::uint8_t> surface);
} // namespace pubg_vision::render
