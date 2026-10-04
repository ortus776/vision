#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>
#include "core/geometry.hpp"

namespace pubg_vision::dataset {
// Lossless RGBA PNG with stored DEFLATE blocks: trades compression for low CPU cost.
// Input is packed BGRA. Does not change alpha or image dimensions.
void encode_png_stored(const std::filesystem::path& path, core::Size size,
                       const std::vector<std::uint8_t>& pixels);
} // namespace pubg_vision::dataset
