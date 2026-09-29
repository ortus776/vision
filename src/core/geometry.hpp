#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace pubg_vision::core {

struct Size {
    std::int32_t width{};
    std::int32_t height{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return width > 0 && height > 0;
    }
};

struct Rect {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t width{};
    std::int32_t height{};

    [[nodiscard]] constexpr std::int64_t right() const noexcept {
        return static_cast<std::int64_t>(left) + width;
    }

    [[nodiscard]] constexpr std::int64_t bottom() const noexcept {
        return static_cast<std::int64_t>(top) + height;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return width > 0 && height > 0;
    }
};

// Coordinates and dimensions are physical pixels. The source origin may be
// negative when the selected window is on a monitor left of the primary one.
[[nodiscard]] inline Rect centered_rect(Rect source, Size roi) {
    if (!source.valid()) {
        throw std::invalid_argument("source rectangle must have positive dimensions");
    }
    if (!roi.valid()) {
        throw std::invalid_argument("ROI dimensions must be positive");
    }
    if (roi.width > source.width || roi.height > source.height) {
        throw std::out_of_range("ROI does not fit inside the source rectangle");
    }

    const auto left = static_cast<std::int64_t>(source.left) +
                      (static_cast<std::int64_t>(source.width) - roi.width) / 2;
    const auto top = static_cast<std::int64_t>(source.top) +
                     (static_cast<std::int64_t>(source.height) - roi.height) / 2;
    constexpr auto min = std::numeric_limits<std::int32_t>::min();
    constexpr auto max = std::numeric_limits<std::int32_t>::max();
    if (left < min || left > max || top < min || top > max) {
        throw std::out_of_range("centered ROI origin is outside the supported coordinate range");
    }

    return Rect{
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        roi.width,
        roi.height,
    };
}

// Converts an absolute rectangle into coordinates relative to its parent.
// Rejects rectangles that cross the parent boundary.
[[nodiscard]] inline Rect relative_rect(Rect absolute, Rect parent) {
    if (!absolute.valid() || !parent.valid()) {
        throw std::invalid_argument("rectangles must have positive dimensions");
    }
    if (absolute.left < parent.left || absolute.top < parent.top ||
        absolute.right() > parent.right() || absolute.bottom() > parent.bottom()) {
        throw std::out_of_range("rectangle does not fit inside its parent");
    }

    const auto left = static_cast<std::int64_t>(absolute.left) - parent.left;
    const auto top = static_cast<std::int64_t>(absolute.top) - parent.top;
    if (left > std::numeric_limits<std::int32_t>::max() ||
        top > std::numeric_limits<std::int32_t>::max()) {
        throw std::out_of_range("relative rectangle origin exceeds the supported range");
    }
    return Rect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                absolute.width, absolute.height};
}

} // namespace pubg_vision::core
