#include "app/live.hpp"
#include "app/inference_model.hpp"
#include "capture/desktop_capture.hpp"
#include "core/json.hpp"
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
    std::filesystem::create_directories(config.output);
    const auto stamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::filesystem::path directory;
    bool created = false;
    for (unsigned suffix = 0; suffix < 1000; ++suffix) {
        directory = config.output / ("inference_demo_" + std::to_string(stamp) + "_" + std::to_string(suffix));
        if (std::filesystem::create_directory(directory)) { created = true; break; }
    }
    if (!created) throw std::runtime_error("could not create a unique demo directory");
    std::filesystem::create_directory(directory / "images");
    std::ofstream metadata(directory / "demo.json"), manifest(directory / "detections.jsonl");
    metadata.exceptions(std::ios::badbit | std::ios::failbit);
    manifest.exceptions(std::ios::badbit | std::ios::failbit);
    metadata << "{\"backend\":\"mock\",\"seed\":" << config.mock_seed << ",\"frame_count\":" << config.demo_frames
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
    logger.write(core::LogLevel::info, "Mock inference demo: " + directory.string() +
        "; rendered PNG frames=" + std::to_string(config.demo_frames));
}
} // namespace pubg_vision::app
