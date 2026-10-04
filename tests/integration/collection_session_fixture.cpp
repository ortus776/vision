#include "capture/desktop_capture.hpp"
#include "collection/capture_scheduler.hpp"
#include "core/filesystem.hpp"
#include "core/performance_clock.hpp"
#include "dataset/dataset_writer.hpp"
#include "dataset/png_encoder.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

int wmain(int argc, wchar_t* argv[]) {
    using namespace pubg_vision;
    try {
        if (argc != 3) throw std::invalid_argument("expected output root and late/overflow/store scenario");
        if (std::wstring_view(argv[2]) == L"store-boundaries") {
            const std::filesystem::path root(argv[1]);
            std::filesystem::create_directories(root);
            // Filtered row length is 5: these straddle and exactly fill 65535-byte blocks.
            for (int height : {1, 13106, 13107, 13108, 26214}) {
                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(height) * 4);
                for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<std::uint8_t>(i % 256);
                dataset::encode_png_stored(root / (std::to_string(height) + ".png"), {1, height}, pixels);
            }
            const auto rejects = [&](core::Size size, const std::vector<std::uint8_t>& pixels) {
                try { dataset::encode_png_stored(root / "invalid.png", size, pixels); }
                catch (const std::invalid_argument&) { return; }
                throw std::runtime_error("stored PNG encoder accepted invalid dimensions/buffer");
            };
            rejects({0, 1}, {}); rejects({1, -1}, {}); rejects({1, 1}, {1, 2, 3});
            bool failed = false;
            try { dataset::encode_png_stored(root / "missing-parent/frame.png", {1,1}, {0,0,0,255}); }
            catch (const std::runtime_error&) { failed = true; }
            if (!failed) throw std::runtime_error("stored PNG failed to report a missing output directory");
            return 0;
        }
        const bool stored = std::wstring_view(argv[2]) == L"store";
        const bool overflow = std::wstring_view(argv[2]) == L"overflow";
        collection::Settings settings;
        settings.periodic_interval = 1;
        settings.max_lateness = overflow ? 60000 : 0;
        settings.max_pending = overflow ? 2 : 32;
        const std::int64_t now = overflow ? 60000 : 3600000;
        dataset::DatasetWriter writer(argv[1], 16,
            "{\"periodic_interval_ms\":1,\"max_lateness_ms\":" + std::to_string(settings.max_lateness) + '}',
            stored ? dataset::encode_png_stored : capture::encode_png, stored);
        collection::CaptureScheduler scheduler(settings, [&](const std::string& event) { writer.event(event); });
        scheduler.set_active(true, 0, "start");
        auto requests = scheduler.take_due(now);
        if (requests.empty()) throw std::runtime_error("fresh fixture request was lost");
        core::Frame frame;
        frame.size = {2,2}; frame.source_size = {8,8}; frame.roi = {0,0,2,2};
        frame.pixels.assign(16, 255); frame.generation = 1;
        if (stored) frame.pixels = {0,0,255,255, 0,255,0,128, 255,0,0,64, 33,22,11,0};
        frame.captured_ms = now; frame.captured_utc_ms = core::utc_milliseconds();
        frame.capture_end_qpc = core::qpc_ticks();
        if (!writer.try_enqueue(std::move(frame), std::move(requests))) throw std::runtime_error("fixture PNG rejected");
        scheduler.set_active(false, now, "stop");
        writer.finish();
        const auto w = writer.stats(); const auto s = scheduler.stats();
        writer.event("{\"type\":\"session_finished\",\"saved\":" + std::to_string(w.saved) +
            ",\"requested\":" + std::to_string(s.requested) + ",\"expired\":" + std::to_string(s.expired) +
            ",\"pending_overflow\":" + std::to_string(s.overflow) + ",\"merged\":" + std::to_string(s.merged) +
            ",\"writer_rejected\":" + std::to_string(w.rejected) + '}');
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
