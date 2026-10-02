#include "preprocess/preprocessor.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pubg_vision::preprocess {
void prepare(const core::Frame& frame, const inference::ModelSpec& spec,
             inference::PreparedInput& output) {
    spec.validate();
    if (!frame.size.valid()) throw std::invalid_argument("invalid frame dimensions");
    const auto pixels = static_cast<std::uint64_t>(frame.size.width) * frame.size.height;
    if (pixels > 16U * 1024U * 1024U || frame.pixels.size() != pixels * 4U)
        throw std::invalid_argument("frame must contain a bounded packed BGRA buffer");
    const auto input = spec.input_size;
    const double ratio = std::min(static_cast<double>(input.width) / frame.size.width,
                                  static_cast<double>(input.height) / frame.size.height);
    const core::Size resized{
        std::clamp(static_cast<std::int32_t>(std::lround(frame.size.width * ratio)), 1, input.width),
        std::clamp(static_cast<std::int32_t>(std::lround(frame.size.height * ratio)), 1, input.height)};
    const auto left = (input.width - resized.width) / 2;
    const auto top = (input.height - resized.height) / 2;
    output.transform = {frame.size, input, resized, left, top,
        static_cast<float>(resized.width) / static_cast<float>(frame.size.width),
        static_cast<float>(resized.height) / static_cast<float>(frame.size.height)};
    output.shape = {1, 3, input.height, input.width};
    const auto plane = static_cast<std::size_t>(input.width) * input.height;
    output.tensor.resize(plane * 3U);
    std::fill(output.tensor.begin(), output.tensor.end(), static_cast<float>(spec.padding_value) / 255.F);
    for (std::int32_t y = 0; y < resized.height; ++y) {
        const double sy = std::clamp((y + 0.5) * frame.size.height / resized.height - 0.5,
                                     0.0, static_cast<double>(frame.size.height - 1));
        const auto y0 = static_cast<std::int32_t>(sy), y1 = std::min(y0 + 1, frame.size.height - 1);
        const float wy = static_cast<float>(sy - y0);
        for (std::int32_t x = 0; x < resized.width; ++x) {
            const double sx = std::clamp((x + 0.5) * frame.size.width / resized.width - 0.5,
                                         0.0, static_cast<double>(frame.size.width - 1));
            const auto x0 = static_cast<std::int32_t>(sx), x1 = std::min(x0 + 1, frame.size.width - 1);
            const float wx = static_cast<float>(sx - x0);
            const auto read = [&](std::int32_t px, std::int32_t py, std::size_t channel) {
                return static_cast<float>(frame.pixels[(static_cast<std::size_t>(py) * frame.size.width + px) * 4U + channel]);
            };
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto bgra_channel = 2U - channel;
                const float a = read(x0, y0, bgra_channel) * (1 - wx) + read(x1, y0, bgra_channel) * wx;
                const float b = read(x0, y1, bgra_channel) * (1 - wx) + read(x1, y1, bgra_channel) * wx;
                output.tensor[channel * plane + static_cast<std::size_t>(y + top) * input.width + x + left] =
                    (a * (1 - wy) + b * wy) / 255.F;
            }
        }
    }
}
} // namespace pubg_vision::preprocess
