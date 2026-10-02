#pragma once
#include <span>
#include "inference/model.hpp"

namespace pubg_vision::postprocess {
[[nodiscard]] std::vector<inference::Detection> decode(std::span<const inference::Candidate> candidates,
    const inference::Transform& transform, const inference::ModelSpec& spec,
    const inference::PostprocessSettings& settings);
} // namespace pubg_vision::postprocess
