#include "inference/mock_model.hpp"
#include <stdexcept>
#include <utility>

namespace pubg_vision::inference {
MockModel::MockModel(ModelSpec spec, std::uint32_t seed) : spec_(std::move(spec)), random_(seed) {
    spec_.validate();
}
std::vector<Candidate> MockModel::run(const PreparedInput& input) {
    if (input.shape != std::array<std::int64_t, 4>{1, 3, spec_.input_size.height, spec_.input_size.width} ||
        input.tensor.size() != static_cast<std::size_t>(spec_.input_size.width) * spec_.input_size.height * 3U)
        throw std::invalid_argument("mock model received a tensor incompatible with its ModelSpec");
    const auto sample = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(random_); };
    const auto& t = input.transform;
    const float w = static_cast<float>(t.resized_size.width), h = static_cast<float>(t.resized_size.height);
    const float bw = sample(0.10F, 0.35F) * w, bh = sample(0.10F, 0.35F) * h;
    const float x = static_cast<float>(t.padding_left) + sample(0.F, w - bw);
    const float y = static_cast<float>(t.padding_top) + sample(0.F, h - bh);
    return {{{x, y, x + bw, y + bh}, 1.F, 0}};
}
} // namespace pubg_vision::inference
