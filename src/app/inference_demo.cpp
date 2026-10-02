#include "app/live.hpp"
#include "app/inference_model.hpp"
#include "capture/desktop_capture.hpp"
#include "core/json.hpp"
#include "core/filesystem.hpp"
#include "inference/pipeline.hpp"
#include "render/raster.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pubg_vision::app {
void inference_demo(const config::AppConfig& config, const core::Logger& logger) {
    using namespace std::chrono;
    auto model = make_model(config);
    config.inference.validate();
    const auto size = config.roi;
    if (!size.valid() || static_cast<std::uint64_t>(size.width) * size.height > 16U * 1024U * 1024U)
        throw std::invalid_argument("demo ROI exceeds the supported BGRA buffer size");
    const auto directory = core::unique_directory(config.output,
        "inference_demo_" + std::to_string(core::utc_milliseconds()));
    std::filesystem::create_directory(directory / "images");
    std::ofstream metadata(directory / "demo.json"), manifest(directory / "detections.jsonl");
    metadata.exceptions(std::ios::badbit | std::ios::failbit);
    manifest.exceptions(std::ios::badbit | std::ios::failbit);
    metadata << "{\"backend\":" << core::json_string(model->backend_name()) << ",\"seed\":";
    if (model->backend_name() == "mock") metadata << config.mock_seed;
    else metadata << "null";
    metadata << ",\"classes\":[";
    bool separator = false;
    for (const auto& name : model->spec().classes) {
        if (separator) metadata << ',';
        metadata << core::json_string(name); separator = true;
    }
    const auto& post = config.inference.postprocess;
    metadata << "],\"confidence_threshold\":" << post.confidence_threshold
        << ",\"nms_iou\":" << post.nms_iou << ",\"max_candidates\":" << post.max_candidates
        << ",\"max_detections\":" << post.max_detections
        << ",\"output_has_nms\":" << (model->spec().output_has_nms ? "true" : "false")
        << ",\"padding_value\":" << static_cast<unsigned>(model->spec().padding_value)
        << ",\"nms_coordinate_space\":\"input_pixels_before_clip\",\"frame_count\":" << config.demo_frames
        << ",\"input_shape\":[1,3," << model->spec().input_size.height << ',' << model->spec().input_size.width
        << "],\"format\":\"RGB float32 NCHW /255\",\"box_coordinates\":\"ROI pixels, XYXY\"}\n";
    metadata.close();
    const auto start = steady_clock::now();
    const inference::Clock now = [&] { return duration_cast<milliseconds>(steady_clock::now() - start).count(); };
    inference::PreparedInput scratch;
    for (std::int32_t i = 1; i <= config.demo_frames; ++i) {
        core::Frame frame;
        frame.size = frame.source_size = size; frame.roi = {0, 0, size.width, size.height};
        frame.generation = 1; frame.captured_ms = now();
        frame.pixels.resize(static_cast<std::size_t>(size.width) * size.height * 4U);
        for (std::int32_t y = 0; y < size.height; ++y) for (std::int32_t x = 0; x < size.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * size.width + x) * 4U;
            frame.pixels[offset] = static_cast<std::uint8_t>(40 + (static_cast<std::int64_t>(x) * 120 / size.width));
            frame.pixels[offset + 1] = 40;
            frame.pixels[offset + 2] = static_cast<std::uint8_t>(40 + (static_cast<std::int64_t>(y) * 120 / size.height));
            frame.pixels[offset + 3] = 255;
        }
        inference::FramePacket packet{static_cast<std::uint64_t>(i), std::move(frame)};
        const auto result = inference::process(packet, *model, config.inference.postprocess, scratch, now);
        render::composite(packet.frame, render::rasterize(size, result.detections, config.overlay_style));
        std::ostringstream filename;
        filename << "frame_" << std::setw(8) << std::setfill('0') << i << ".png";
        const auto relative = "images/" + filename.str();
        const auto temporary = directory / (relative + ".tmp");
        capture::encode_png(temporary, size, packet.frame.pixels);
        std::filesystem::rename(temporary, directory / relative);
        manifest << std::setprecision(9) << "{\"frame_id\":" << result.frame_id << ",\"path\":" << core::json_string(relative)
            << ",\"captured_ms\":" << result.captured_ms << ",\"completed_ms\":" << result.completed_ms << ",\"detections\":[";
        bool comma = false;
        for (const auto& detection : result.detections) {
            if (comma) manifest << ',';
            comma = true;
            const auto& b = detection.box;
            manifest << "{\"class_id\":" << detection.class_id << ",\"confidence\":" << detection.confidence
                << ",\"box\":{\"left\":" << b.left << ",\"top\":" << b.top
                << ",\"right\":" << b.right << ",\"bottom\":" << b.bottom << "}}";
        }
        manifest << "]}\n"; manifest.flush();
    }
    manifest.close();
    logger.write(core::LogLevel::info, "Inference demo (" + std::string(model->backend_name()) + "): " + directory.string() +
        "; rendered PNG frames=" + std::to_string(config.demo_frames));
}
} // namespace pubg_vision::app
