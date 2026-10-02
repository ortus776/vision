#include "render/raster.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pubg_vision::render {
std::vector<std::uint8_t> rasterize(core::Size size, std::span<const inference::Detection> detections, Style style) {
    if (!size.valid() || static_cast<std::uint64_t>(size.width) * size.height > 16U * 1024U * 1024U)
        throw std::invalid_argument("invalid or excessive raster dimensions");
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size.width) * size.height * 4U, 0);
    const auto paint = [&](std::int32_t x, std::int32_t y) {
        if (x < 0 || y < 0 || x >= size.width || y >= size.height) return;
        const auto offset = (static_cast<std::size_t>(y) * size.width + x) * 4U;
        pixels[offset + 1] = 255; pixels[offset + 3] = 255;
    };
    for (const auto& d : detections) {
        const auto b = d.box;
        if (!std::isfinite(b.left) || !std::isfinite(b.top) || !std::isfinite(b.right) ||
            !std::isfinite(b.bottom) || b.right <= b.left || b.bottom <= b.top) continue;
        const auto left = static_cast<std::int32_t>(std::floor(std::clamp(b.left, 0.F, static_cast<float>(size.width))));
        const auto top = static_cast<std::int32_t>(std::floor(std::clamp(b.top, 0.F, static_cast<float>(size.height))));
        const auto right = static_cast<std::int32_t>(std::ceil(std::clamp(b.right, 0.F, static_cast<float>(size.width))));
        const auto bottom = static_cast<std::int32_t>(std::ceil(std::clamp(b.bottom, 0.F, static_cast<float>(size.height))));
        if (right <= left || bottom <= top) continue;
        if (style == Style::point) {
            const auto cx = left + (right - left) / 2, cy = top + (bottom - top) / 2;
            for (std::int32_t y = -3; y <= 3; ++y)
                for (std::int32_t x = -3; x <= 3; ++x) if (x*x + y*y <= 9) paint(cx + x, cy + y);
        } else {
            for (auto x = left; x < right; ++x)
                for (std::int32_t stroke = 0; stroke < std::min(2, bottom - top); ++stroke) {
                    paint(x, top + stroke); paint(x, bottom - 1 - stroke);
                }
            for (auto y = top; y < bottom; ++y)
                for (std::int32_t stroke = 0; stroke < std::min(2, right - left); ++stroke) {
                    paint(left + stroke, y); paint(right - 1 - stroke, y);
                }
        }
    }
    return pixels;
}
void composite(core::Frame& frame, std::span<const std::uint8_t> surface) {
    if (!frame.size.valid() || frame.pixels.size() != static_cast<std::uint64_t>(frame.size.width) * frame.size.height * 4U ||
        frame.pixels.size() != surface.size()) throw std::invalid_argument("surface dimensions do not match frame");
    for (std::size_t i = 0; i < surface.size(); i += 4U)
        if (surface[i + 3] != 0) std::copy_n(surface.begin() + static_cast<std::ptrdiff_t>(i), 4, frame.pixels.begin() + static_cast<std::ptrdiff_t>(i));
}
} // namespace pubg_vision::render
