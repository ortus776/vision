#include "app/collect.hpp"
#include "capture/desktop_capture.hpp"
#include "collection/capture_scheduler.hpp"
#include "collection/collection_state.hpp"
#include "core/json.hpp"
#include "dataset/dataset_writer.hpp"
#include "input/windows_input.hpp"
#include "platform/windows/window_selection.hpp"
#include <chrono>
#include <sstream>
#include <thread>

#ifndef PUBGVISION_VERSION
#define PUBGVISION_VERSION "0.0.0"
#endif

namespace pubg_vision::app {
void collect(const config::AppConfig& config, const core::Logger& logger) {
    using namespace std::chrono;
    namespace windows = platform::windows;
    windows::set_per_monitor_dpi_awareness();
    const auto title = windows::wide_from_utf8(config.window_title);
    auto selected = windows::find_unique_window(title);
    const auto start = steady_clock::now();
    const auto now = [&] { return duration_cast<milliseconds>(steady_clock::now() - start).count(); };
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    const auto& c = config.collection;
    std::ostringstream metadata;
    metadata << "{\"app_version\":\"" PUBGVISION_VERSION "\",\"platform\":\"Windows\","
        << "\"window_title\":" << core::json_string(config.window_title)
        << ",\"window_handle\":" << reinterpret_cast<std::uintptr_t>(selected.handle)
        << ",\"client_width\":" << selected.client_screen.width
        << ",\"client_height\":" << selected.client_screen.height
        << ",\"qpc_frequency\":" << frequency.QuadPart
        << ",\"roi_width\":" << config.roi.width << ",\"roi_height\":" << config.roi.height
        << ",\"burst_offsets_ms\":[0,100,200],\"periodic_interval_ms\":" << c.periodic_interval
        << ",\"merge_window_ms\":" << c.merge_window << ",\"max_lateness_ms\":" << c.max_lateness
        << ",\"max_pending\":" << c.max_pending << ",\"writer_capacity\":" << config.writer_capacity
        << ",\"start_active\":" << (config.start_active ? "true" : "false") << '}';
    dataset::DatasetWriter writer(config.output, config.writer_capacity, metadata.str(), capture::encode_png);
    collection::CaptureScheduler scheduler(c, [&](const std::string& event) { writer.event(event); });
    input::WindowsInput input(now);
    input.set_target(selected.handle);
    capture::DesktopCapture source;
    bool enabled = config.start_active, started = config.start_active, stopping = false, ready = false;
    std::optional<collection::CollectionState> reported_state;
    collection::Millis retry_at = 0;
    std::vector<collection::Request> in_flight;
    std::uint64_t capture_skips = 0, input_drops = 0;
    const auto cancel_flight = [&](const std::string& reason, const char* type = "cancelled") {
        if (in_flight.empty()) return;
        writer.event("{\"type\":" + core::json_string(type) + ",\"at_ms\":" + std::to_string(now()) +
                     ",\"reason\":" + core::json_string(reason) + ",\"requests\":" +
                     collection::requests_json(in_flight) + '}');
        capture_skips += in_flight.size();
        in_flight.clear();
    };
    const auto recover = [&](const std::string& message) {
        ready = false; source.reset(); retry_at = now() + 1000;
        scheduler.set_active(false, now(), "source_unavailable");
        cancel_flight("source_unavailable");
        writer.event("{\"type\":\"capture_error\",\"at_ms\":" + std::to_string(now()) +
                     ",\"message\":" + core::json_string(message) + '}');
        logger.write(core::LogLevel::warning, "Capture paused; retry in 1s: " + message);
    };
    logger.write(core::LogLevel::info, "Session: " + writer.directory().string() +
        "; F8 starts/pauses, F9 stops. Collection requires selected window foreground.");
    const auto report_state = [&](bool focused) {
        const auto state = collection::collection_state(started, enabled, ready, focused, stopping);
        if (reported_state && *reported_state == state) return;
        reported_state = state;
        logger.write(core::LogLevel::info, collection::state_message(state));
        writer.event("{\"type\":\"collection_status\",\"at_ms\":" + std::to_string(now()) +
            ",\"state\":" + core::json_string(collection::state_name(state)) +
            ",\"enabled\":" + std::string(enabled ? "true" : "false") +
            ",\"source_ready\":" + std::string(ready ? "true" : "false") +
            ",\"focused\":" + std::string(focused ? "true" : "false") + '}');
    };
    try {
        report_state(GetForegroundWindow() == selected.handle);
        while (!stopping) {
            writer.check_error();
            input.pump();
            // Probe geometry even while paused; recovery never overrides the user's pause.
            if (ready || now() >= retry_at) {
                bool changed = false;
                std::string failure;
                try {
                    if (!IsWindow(selected.handle)) {
                        selected = windows::find_unique_window(title);
                        input.set_target(selected.handle);
                    }
                    changed = source.prepare(reinterpret_cast<std::uintptr_t>(selected.handle), config.roi);
                } catch (const std::exception& ex) { failure = ex.what(); }
                if (!failure.empty()) recover(failure);
                else {
                    ready = true;
                    if (changed) {
                        scheduler.cancel(now(), "source_changed");
                        cancel_flight("source_changed");
                        writer.event("{\"type\":\"source_changed\",\"at_ms\":" + std::to_string(now()) + '}');
                    }
                }
            }
            for (const auto& event : input.take()) {
                if (event.action == input::Action::stop || event.action == input::Action::toggle) {
                    if (event.action == input::Action::stop) stopping = true;
                    else { enabled = !enabled; if (enabled) started = true; }
                    writer.event("{\"type\":\"control\",\"at_ms\":" + std::to_string(event.at_ms) +
                        ",\"action\":" + core::json_string(stopping ? "stop" : "toggle") +
                        ",\"enabled\":" + std::string(enabled ? "true" : "false") +
                        ",\"focused\":" + std::string(event.focused ? "true" : "false") + '}');
                    if (stopping) break;
                }
                scheduler.set_active(enabled && ready && event.focused, event.at_ms,
                                     !enabled ? "user_pause" : (!ready ? "source_unavailable" : "focus"));
                if (event.action == input::Action::click) scheduler.click(event.at_ms);
            }
            const bool focused = GetForegroundWindow() == selected.handle;
            report_state(focused);
            scheduler.set_active(enabled && ready && focused && !stopping, now(),
                                 stopping ? "stop" : (!enabled ? "user_pause" : (!ready ? "source_unavailable" : "focus")));
            if (!scheduler.active()) cancel_flight(stopping ? "stop" : "pause");
            if (input.dropped() != input_drops) {
                input_drops = input.dropped();
                writer.event("{\"type\":\"input_overflow\",\"dropped_total\":" + std::to_string(input_drops) + '}');
            }
            if (stopping) break;
            if (scheduler.active()) {
                if (in_flight.empty()) in_flight = scheduler.take_due(now());
                if (!in_flight.empty()) {
                    if (now() - in_flight.front().planned_ms > c.max_lateness) {
                        cancel_flight("capture_timeout_or_late", "skipped");
                    } else {
                        std::optional<core::Frame> frame;
                        std::string failure;
                        bool changed = false;
                        try {
                            frame = source.next(5);
                            if (frame) changed = source.prepare(reinterpret_cast<std::uintptr_t>(selected.handle), config.roi);
                        } catch (const std::exception& ex) { failure = ex.what(); }
                        if (!failure.empty()) recover(failure);
                        else if (changed || GetForegroundWindow() != selected.handle) {
                            cancel_flight(changed ? "source_changed" : "focus");
                            scheduler.cancel(now(), changed ? "source_changed" : "focus");
                        } else if (frame) {
                            frame->captured_ms = now();
                            if (frame->captured_ms - in_flight.front().planned_ms > c.max_lateness)
                                cancel_flight("capture_late", "skipped");
                            else {
                                writer.try_enqueue(std::move(*frame), std::move(in_flight));
                                in_flight.clear();
                            }
                        }
                    }
                }
            }
            // Wait wakes for Raw Input/hotkeys and caps polling latency at 5ms.
            MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        scheduler.set_active(false, now(), "stop");
        cancel_flight("stop");
        writer.finish();
        const auto w = writer.stats(); const auto s = scheduler.stats();
        writer.event("{\"type\":\"session_finished\",\"at_ms\":" + std::to_string(now()) +
            ",\"saved\":" + std::to_string(w.saved) + ",\"writer_rejected\":" + std::to_string(w.rejected) +
            ",\"writer_failed\":" + std::to_string(w.failed) + ",\"writer_high_water\":" + std::to_string(w.high_water) +
            ",\"requested\":" + std::to_string(s.requested) + ",\"expired\":" + std::to_string(s.expired) +
            ",\"pending_overflow\":" + std::to_string(s.overflow) + ",\"cancelled\":" + std::to_string(s.cancelled) +
            ",\"merged\":" + std::to_string(s.merged) + ",\"capture_skips\":" + std::to_string(capture_skips) +
            ",\"input_dropped\":" + std::to_string(input_drops) + '}');
        logger.write(core::LogLevel::info, "Session finished: saved=" + std::to_string(w.saved) +
                     ", writer skips=" + std::to_string(w.rejected) + ", errors=" + std::to_string(w.failed));
        if (s.requested == 0) {
            logger.write(core::LogLevel::warning,
                "No capture requests were created. Check the WAITING/PAUSED status, F8 and selected-window focus; "
                "--start-active enables collection without the initial F8 press.");
        }
    } catch (...) {
        const auto error = std::current_exception();
        try { scheduler.set_active(false, now(), "fatal_error"); cancel_flight("fatal_error"); } catch (...) {}
        try { writer.finish(); } catch (...) {}
        const auto w = writer.stats();
        logger.write(core::LogLevel::error, "Session failed: saved=" + std::to_string(w.saved) +
            ", failed=" + std::to_string(w.failed) + "; inspect " + writer.directory().string());
        std::rethrow_exception(error);
    }
}
} // namespace pubg_vision::app
