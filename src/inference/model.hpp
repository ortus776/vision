#pragma once
#include <array>
#include <cstdint>
#include <string>
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
class Model {
public:
    virtual ~Model() = default;
    [[nodiscard]] virtual const ModelSpec& spec() const noexcept = 0; // Immutable for this model's lifetime.
    // Called serially by the inference worker. Own/reuse the runtime session here.
    [[nodiscard]] virtual std::vector<Candidate> run(const PreparedInput& input) = 0;
};
struct PostprocessSettings {
    float confidence_threshold{0.25F};
    float nms_iou{0.45F};
    std::size_t max_candidates{3000}, max_detections{100};
    void validate() const;
};
} // namespace pubg_vision::inference
