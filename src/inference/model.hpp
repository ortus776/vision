#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <stop_token>
#include <exception>
#include <vector>
#include "core/geometry.hpp"

namespace pubg_vision::inference {
// The adapter must produce XYXY boxes in input-tensor pixels, not screen pixels.
struct Box { float left{}, top{}, right{}, bottom{}; };
struct Candidate { Box box; float confidence{}; std::int32_t class_id{}; };
struct Detection { Box box; float confidence{}; std::int32_t class_id{}; }; // ROI pixels.
struct ModelSpec {
    core::Size input_size{416, 416};
    std::vector<std::string> classes{"object"};
    std::uint8_t padding_value{114};
    bool output_has_nms{false};
    void validate() const;
};
struct Transform {
    core::Size source_size, input_size, resized_size;
    std::int32_t padding_left{}, padding_top{};
    float scale_x{}, scale_y{}; // Actual rounded resize dimensions / source dimensions.
};
struct PreparedInput {
    std::vector<float> tensor; // Contiguous RGB float32, NCHW, normalized by 255.
    std::array<std::int64_t, 4> shape{};
    Transform transform;
};
class Cancelled final : public std::exception {
public:
    const char* what() const noexcept override { return "inference cancelled"; }
};
inline void throw_if_cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw Cancelled{};
}
class Model {
public:
    virtual ~Model() = default;
    [[nodiscard]] virtual const ModelSpec& spec() const noexcept = 0; // Immutable for this model's lifetime.
    [[nodiscard]] virtual std::string_view backend_name() const noexcept { return "custom"; }
    // Called serially by the inference worker. Own/reuse the runtime session here.
    // Long-running adapters must honour stop, using their runtime cancellation API.
    [[nodiscard]] virtual std::vector<Candidate> run(const PreparedInput& input, std::stop_token stop = {}) = 0;
};
struct PostprocessSettings {
    float confidence_threshold{0.25F};
    float nms_iou{0.45F};
    std::size_t max_candidates{3000}, max_detections{100};
    void validate() const;
};
} // namespace pubg_vision::inference
