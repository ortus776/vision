#include "postprocess/postprocessor.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pubg_vision::postprocess {
namespace {
bool valid(inference::Box b) {
    return std::isfinite(b.left) && std::isfinite(b.top) && std::isfinite(b.right) &&
        std::isfinite(b.bottom) && b.right > b.left && b.bottom > b.top;
}
double iou(inference::Box a, inference::Box b) {
    const double overlap = std::max(0.0, static_cast<double>(std::min(a.right, b.right) - std::max(a.left, b.left))) *
        std::max(0.0, static_cast<double>(std::min(a.bottom, b.bottom) - std::max(a.top, b.top)));
    const double area_a = static_cast<double>(a.right - a.left) * (a.bottom - a.top);
    const double area_b = static_cast<double>(b.right - b.left) * (b.bottom - b.top);
    return overlap / (area_a + area_b - overlap);
}
}
std::vector<inference::Detection> decode(std::span<const inference::Candidate> candidates,
    const inference::Transform& t, const inference::ModelSpec& spec,
    const inference::PostprocessSettings& settings) {
    spec.validate(); settings.validate();
    if (!t.source_size.valid() || !std::isfinite(t.scale_x) || !std::isfinite(t.scale_y) ||
        t.scale_x <= 0 || t.scale_y <= 0)
        throw std::invalid_argument("invalid inverse letterbox transform");
    std::vector<inference::Detection> filtered;
    for (const auto& c : candidates) {
        if (!valid(c.box) || !std::isfinite(c.confidence) || c.confidence < settings.confidence_threshold ||
            c.confidence > 1 || c.class_id < 0 || static_cast<std::size_t>(c.class_id) >= spec.classes.size()) continue;
        inference::Box box{
            (c.box.left - static_cast<float>(t.padding_left)) / t.scale_x,
            (c.box.top - static_cast<float>(t.padding_top)) / t.scale_y,
            (c.box.right - static_cast<float>(t.padding_left)) / t.scale_x,
            (c.box.bottom - static_cast<float>(t.padding_top)) / t.scale_y};
        box.left = std::clamp(box.left, 0.F, static_cast<float>(t.source_size.width));
        box.right = std::clamp(box.right, 0.F, static_cast<float>(t.source_size.width));
        box.top = std::clamp(box.top, 0.F, static_cast<float>(t.source_size.height));
        box.bottom = std::clamp(box.bottom, 0.F, static_cast<float>(t.source_size.height));
        if (valid(box)) filtered.push_back({box, c.confidence, c.class_id});
    }
    std::stable_sort(filtered.begin(), filtered.end(), [](const auto& a, const auto& b) {
        return a.confidence > b.confidence;
    });
    if (filtered.size() > settings.max_candidates) filtered.resize(settings.max_candidates);
    std::vector<inference::Detection> result;
    for (const auto& candidate : filtered) {
        const bool suppressed = !spec.output_has_nms && std::any_of(result.begin(), result.end(), [&](const auto& kept) {
            return kept.class_id == candidate.class_id && iou(kept.box, candidate.box) > settings.nms_iou;
        });
        if (!suppressed) result.push_back(candidate);
        if (result.size() == settings.max_detections) break;
    }
    return result;
}
} // namespace pubg_vision::postprocess
