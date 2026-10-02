#pragma once
#include "core/frame.hpp"
#include "inference/model.hpp"

namespace pubg_vision::preprocess {
// Bilinear half-pixel resize + centered letterbox; reuse output.tensor across calls.
void prepare(const core::Frame& frame, const inference::ModelSpec& spec,
             inference::PreparedInput& output);
} // namespace pubg_vision::preprocess
