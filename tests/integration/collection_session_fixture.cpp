#include "capture/desktop_capture.hpp"
#include "collection/capture_scheduler.hpp"
#include "core/filesystem.hpp"
#include "dataset/dataset_writer.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

int wmain(int argc, wchar_t* argv[]) {
    using namespace pubg_vision;
    try {
        if (argc != 3) throw std::invalid_argument("expected output root and late/overflow scenario");
        const bool overflow = std::wstring_view(argv[2]) == L"overflow";
        collection::Settings settings;
        settings.periodic_interval = 1;
        settings.max_lateness = overflow ? 60000 : 0;
        settings.max_pending = overflow ? 2 : 32;
        const std::int64_t now = overflow ? 60000 : 3600000;
        dataset::DatasetWriter writer(argv[1], 16,
            "{\"periodic_interval_ms\":1,\"max_lateness_ms\":" + std::to_string(settings.max_lateness) + '}',
            capture::encode_png);
        collection::CaptureScheduler scheduler(settings, [&](const std::string& event) { writer.event(event); });
        scheduler.set_active(true, 0, "start");
        auto requests = scheduler.take_due(now);
        if (requests.empty()) throw std::runtime_error("fresh fixture request was lost");
        core::Frame frame;
        frame.size = {2,2}; frame.source_size = {8,8}; frame.roi = {0,0,2,2};
        frame.pixels.assign(16, 255); frame.generation = 1;
        frame.captured_ms = now; frame.captured_utc_ms = core::utc_milliseconds();
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
