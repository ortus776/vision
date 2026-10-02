#include "inference/model.hpp"
#include <cmath>
#include <stdexcept>

namespace pubg_vision::inference {
void ModelSpec::validate() const {
    if (!input_size.valid() || input_size.width > 2048 || input_size.height > 2048)
        throw std::invalid_argument("model input dimensions must be in 1..2048");
    if (classes.empty() || classes.size() > 4096)
        throw std::invalid_argument("model must declare 1..4096 classes");
    for (const auto& name : classes)
        if (name.empty()) throw std::invalid_argument("model class names must not be empty");
}
void PostprocessSettings::validate() const {
    if (!std::isfinite(confidence_threshold) || confidence_threshold < 0 || confidence_threshold > 1 ||
        !std::isfinite(nms_iou) || nms_iou < 0 || nms_iou > 1 ||
        max_candidates == 0 || max_candidates > 30000 || max_detections == 0 || max_detections > max_candidates)
        throw std::invalid_argument("invalid confidence/NMS limits");
}
} // namespace pubg_vision::inference
