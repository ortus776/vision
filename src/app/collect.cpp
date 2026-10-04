#include "app/collect.hpp"
#include "capture/desktop_capture.hpp"
#include "collection/capture_scheduler.hpp"
#include "collection/collection_state.hpp"
#include "core/json.hpp"
#include "core/performance_clock.hpp"
#include "dataset/dataset_writer.hpp"
#include "dataset/png_encoder.hpp"
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
    const auto start_qpc = core::qpc_ticks();
    const auto now = [&] { return duration_cast<milliseconds>(steady_clock::now() - start).count(); };
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    const auto& c = config.collection;
    const auto& variant = config.collect_variant;
    const bool legacy = variant == "legacy";
    const bool input_only = variant == "input-only";
    const bool capture_only = variant == "capture-only";
    const bool held = variant == "held" || variant == "png-none" || variant == "png-store" || capture_only;
    std::ostringstream metadata;
    metadata << "{\"app_version\":\"" PUBGVISION_VERSION "\",\"platform\":\"Windows\","
        << "\"window_title\":" << core::json_string(config.window_title)
        << ",\"window_handle\":" << reinterpret_cast<std::uintptr_t>(selected.handle)
        << ",\"client_width\":" << selected.client_screen.width
        << ",\"client_height\":" << selected.client_screen.height
        << ",\"qpc_frequency\":" << frequency.QuadPart
        << ",\"start_qpc\":" << start_qpc << ",\"trace_frames\":" << (config.trace_frames ? "true" : "false")
        << ",\"roi_width\":" << config.roi.width << ",\"roi_height\":" << config.roi.height
        << ",\"burst_offsets_ms\":[0,100,200],\"periodic_interval_ms\":" << c.periodic_interval
        << ",\"merge_window_ms\":" << c.merge_window << ",\"max_lateness_ms\":" << c.max_lateness
        << ",\"max_pending\":" << c.max_pending << ",\"writer_capacity\":" << config.writer_capacity
        << ",\"start_active\":" << (config.start_active ? "true" : "false")
        << ",\"collect_variant\":" << core::json_string(variant)
        << ",\"diagnostic_only\":" << (input_only || capture_only ? "true" : "false")
        << ",\"collect_seconds\":" << config.collect_seconds << '}';
    dataset::PngEncoder encoder = capture::encode_png;
    if (variant == "png-none") encoder = capture::encode_png_no_filter;
    else if (variant == "png-store") encoder = dataset::encode_png_stored;
    dataset::DatasetWriter writer(config.output, config.writer_capacity, metadata.str(), std::move(encoder), config.trace_frames);
    const auto trace_point = [&](const char* type, const std::string& detail) {
        if (config.trace_frames) writer.trace_event("{\"type\":" + core::json_string(type) +
            ",\"qpc\":" + std::to_string(core::qpc_ticks()) + ",\"at_ms\":" + std::to_string(now()) + detail + '}');
    };
    trace_point("collector_started", ",\"variant\":" + core::json_string(variant));
    collection::CaptureScheduler scheduler(c, [&](const std::string& event) { writer.event(event); });
    input::WindowsInput input(now);
    input.set_target(selected.handle);
    capture::DesktopCapture source(held);
    bool enabled = config.start_active, started = config.start_active, stopping = false, ready = false;
    std::optional<collection::CollectionState> reported_state;
    collection::Millis retry_at = 0;
    collection::Millis probe_at = 0, performance_at = 5000;
    std::uint64_t iterations = 0, prepare_calls = 0;
    double prepare_ms = 0, pump_ms = 0, capture_ms = 0, max_capture_ms = 0;
    const auto timed_prepare = [&] {
        const auto begin = steady_clock::now();
        const auto begin_qpc = core::qpc_ticks();
        ++prepare_calls;
        const auto changed = source.prepare(reinterpret_cast<std::uintptr_t>(selected.handle), config.roi);
        const auto elapsed = duration<double, std::milli>(steady_clock::now() - begin).count();
        prepare_ms += elapsed;
        if (config.trace_frames && (changed || elapsed >= 1)) writer.trace_event(
            "{\"type\":\"span\",\"stage\":\"prepare\",\"start_qpc\":" + std::to_string(begin_qpc) +
            ",\"end_qpc\":" + std::to_string(core::qpc_ticks()) + '}');
        return changed;
    };
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
        "; variant=" + variant + "; F8 starts/pauses, F9 stops. Collection requires selected window foreground.");
    if (input_only || capture_only)
        logger.write(core::LogLevel::warning, "DIAGNOSTIC RUN: PNG images will NOT be saved.");
    // Cumulative counters allow interval deltas without per-frame logging overhead.
    const auto performance = [&] {
        const auto p = source.stats(); const auto w = writer.stats();
        FILETIME created{}, exited{}, kernel{}, user{};
        double cpu_ms = 0;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            const auto ticks = [](FILETIME t) {
                return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
            };
            cpu_ms = static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
        }
        std::ostringstream event;
        event << "{\"type\":\"collection_performance\",\"at_ms\":" << now()
            << ",\"qpc\":" << core::qpc_ticks()
            << ",\"variant\":" << core::json_string(variant) << ",\"iterations\":" << iterations
            << ",\"prepare_calls\":" << prepare_calls << ",\"prepare_ms\":" << prepare_ms
            << ",\"pump_ms\":" << pump_ms << ",\"capture_ms\":" << capture_ms
            << ",\"max_capture_ms\":" << max_capture_ms << ",\"capture_calls\":" << p.calls
            << ",\"captured_frames\":" << p.frames << ",\"timeouts\":" << p.timeouts
            << ",\"acquired_updates\":" << p.acquired << ",\"rejected_updates\":" << p.rejected_updates
            << ",\"acquire_ms\":" << p.acquire_ms << ",\"map_ms\":" << p.map_ms
            << ",\"max_map_ms\":" << p.max_map_ms << ",\"copy_ms\":" << p.copy_ms
            << ",\"png_ms\":" << w.encode_ms << ",\"max_png_ms\":" << w.max_encode_ms
            << ",\"write_ms\":" << w.write_ms << ",\"max_write_ms\":" << w.max_write_ms
            << ",\"journal_ms\":" << w.journal_ms << ",\"saved\":" << w.saved
            << ",\"writer_pending\":" << w.pending << ",\"writer_high_water\":" << w.high_water
            << ",\"process_cpu_ms\":" << cpu_ms << '}';
        writer.event(event.str());
    };
    const auto report_state = [&](bool focused) {
        const auto state = collection::collection_state(started, enabled,
            ready || (!legacy && !focused), focused, stopping);
        if (reported_state && *reported_state == state) return;
        reported_state = state;
        trace_point("collection_status", ",\"state\":" + core::json_string(collection::state_name(state)));
        logger.write(core::LogLevel::info,
            (input_only || capture_only) && state == collection::CollectionState::collecting
                ? "DIAGNOSTIC COLLECTING: measuring input/capture; PNG writing disabled."
                : collection::state_message(state));
        writer.event("{\"type\":\"collection_status\",\"at_ms\":" + std::to_string(now()) +
            ",\"state\":" + core::json_string(collection::state_name(state)) +
            ",\"enabled\":" + std::string(enabled ? "true" : "false") +
            ",\"source_ready\":" + std::string(ready ? "true" : "false") +
            ",\"focused\":" + std::string(focused ? "true" : "false") + '}');
    };
    try {
        report_state(GetForegroundWindow() == selected.handle);
        while (!stopping) {
            ++iterations;
            writer.check_error();
            const auto pump_start = steady_clock::now();
            input.pump();
            pump_ms += duration<double, std::milli>(steady_clock::now() - pump_start).count();
            // Legacy keeps its original polling/resource lifetime for an A/B comparison.
            // Other variants only open DXGI while enabled and focused, and probe at 10Hz.
            if ((legacy || now() >= probe_at) && (ready || now() >= retry_at)) {
                probe_at = now() + 100;
                bool changed = false;
                std::string failure;
                try {
                    if (!IsWindow(selected.handle)) {
                        selected = windows::find_unique_window(title);
                        input.set_target(selected.handle);
                    }
                    if (input_only) {
                        (void)windows::inspect_window(selected.handle);
                        ready = true;
                    } else if (legacy || (enabled && GetForegroundWindow() == selected.handle)) {
                        changed = timed_prepare();
                        ready = true;
                    }
                } catch (const std::exception& ex) { failure = ex.what(); }
                if (!failure.empty()) recover(failure);
                else {
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
                    else { enabled = !enabled; if (enabled) { started = true; probe_at = 0; } }
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
            if (!legacy && !input_only && ready && (!enabled || !focused || stopping)) {
                source.reset();
                ready = false;
                probe_at = 0;
                writer.event("{\"type\":\"source_suspended\",\"at_ms\":" + std::to_string(now()) + '}');
            }
            if (config.collect_seconds > 0 && now() >= static_cast<std::int64_t>(config.collect_seconds) * 1000) {
                stopping = true;
                writer.event("{\"type\":\"control\",\"at_ms\":" + std::to_string(now()) +
                    ",\"action\":\"duration_stop\",\"enabled\":" + std::string(enabled ? "true" : "false") + '}');
            }
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
                    if (input_only) {
                        cancel_flight("diagnostic_input_only", "skipped");
                    } else if (now() - in_flight.front().planned_ms > c.max_lateness) {
                        cancel_flight("capture_timeout_or_late", "skipped");
                    } else {
                        std::optional<core::Frame> frame;
                        std::string failure;
                        bool changed = false;
                        try {
                            if (!legacy) changed = timed_prepare();
                            if (!changed) {
                                const auto capture_start = steady_clock::now();
                                const auto capture_start_qpc = core::qpc_ticks();
                                try { frame = source.next(5); }
                                catch (...) {
                                    if (config.trace_frames) writer.trace_event("{\"type\":\"span\",\"stage\":\"capture\",\"outcome\":\"error\",\"start_qpc\":" +
                                        std::to_string(capture_start_qpc) + ",\"end_qpc\":" + std::to_string(core::qpc_ticks()) +
                                        ",\"requests\":" + collection::requests_json(in_flight) + '}');
                                    throw;
                                }
                                const auto capture_end_qpc = core::qpc_ticks();
                                if (frame) frame->capture_end_qpc = capture_end_qpc;
                                if (config.trace_frames) {
                                    const auto t = source.last_timings();
                                    std::ostringstream trace;
                                    trace << "{\"type\":\"span\",\"stage\":\"capture\",\"start_qpc\":" << capture_start_qpc
                                        << ",\"end_qpc\":" << capture_end_qpc << ",\"outcome\":\"" << (frame ? "frame" : "timeout")
                                        << "\",\"requests\":" << collection::requests_json(in_flight)
                                        << ",\"acquire_start_qpc\":" << t.acquire_start << ",\"acquire_end_qpc\":" << t.acquire_end
                                        << ",\"map_start_qpc\":" << t.map_start << ",\"map_end_qpc\":" << t.map_end
                                        << ",\"copy_start_qpc\":" << t.copy_start << ",\"copy_end_qpc\":" << t.copy_end;
                                    if (frame && frame->source_qpc) trace << ",\"source_qpc\":" << *frame->source_qpc;
                                    trace << '}'; writer.trace_event(trace.str());
                                }
                                const auto elapsed = duration<double, std::milli>(steady_clock::now() - capture_start).count();
                                capture_ms += elapsed;
                                if (elapsed > max_capture_ms) max_capture_ms = elapsed;
                            }
                            if (frame) changed = timed_prepare();
                        } catch (const std::exception& ex) { failure = ex.what(); }
                        if (!failure.empty()) recover(failure);
                        else if (changed || GetForegroundWindow() != selected.handle) {
                            cancel_flight(changed ? "source_changed" : "focus");
                            scheduler.cancel(now(), changed ? "source_changed" : "focus");
                        } else if (frame) {
                            frame->captured_ms = now();
                            if (frame->captured_ms - in_flight.front().planned_ms > c.max_lateness)
                                cancel_flight("capture_late", "skipped");
                            else if (capture_only) cancel_flight("diagnostic_capture_only", "skipped");
                            else {
                                writer.try_enqueue(std::move(*frame), std::move(in_flight));
                                in_flight.clear();
                            }
                        }
                    }
                }
            }
            if (now() >= performance_at) {
                performance();
                performance_at = now() + 5000;
            }
            // Wait wakes for Raw Input/hotkeys and caps polling latency at 5ms.
            MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        scheduler.set_active(false, now(), "stop");
        cancel_flight("stop");
        trace_point("collector_stopped", "");
        source.reset(); // No desktop duplication while draining the PNG queue.
        writer.finish();
        performance();
        const auto w = writer.stats(); const auto s = scheduler.stats();
        const auto trace_stats = writer.trace_stats();
        writer.event("{\"type\":\"session_finished\",\"at_ms\":" + std::to_string(now()) +
            ",\"saved\":" + std::to_string(w.saved) + ",\"writer_rejected\":" + std::to_string(w.rejected) +
            ",\"writer_failed\":" + std::to_string(w.failed) + ",\"writer_high_water\":" + std::to_string(w.high_water) +
            ",\"requested\":" + std::to_string(s.requested) + ",\"expired\":" + std::to_string(s.expired) +
            ",\"pending_overflow\":" + std::to_string(s.overflow) + ",\"cancelled\":" + std::to_string(s.cancelled) +
            ",\"merged\":" + std::to_string(s.merged) + ",\"capture_skips\":" + std::to_string(capture_skips) +
            ",\"input_dropped\":" + std::to_string(input_drops) +
            ",\"trace_written\":" + std::to_string(trace_stats.written) +
            ",\"trace_dropped\":" + std::to_string(trace_stats.dropped) + '}');
        logger.write(core::LogLevel::info, "Session finished: saved=" + std::to_string(w.saved) +
                     ", writer skips=" + std::to_string(w.rejected) + ", errors=" + std::to_string(w.failed));
        if (s.requested == 0) {
            logger.write(core::LogLevel::warning,
                "No capture requests were created. Check the WAITING/PAUSED status, F8 and selected-window focus; "
                "--start-active enables collection without the initial F8 press.");
        }
    } catch (...) {
        const auto error = std::current_exception();
        try { trace_point("collector_failed", ""); } catch (...) {}
        source.reset();
        try { scheduler.set_active(false, now(), "fatal_error"); cancel_flight("fatal_error"); } catch (...) {}
        try { writer.finish(); } catch (...) {}
        const auto w = writer.stats();
        logger.write(core::LogLevel::error, "Session failed: saved=" + std::to_string(w.saved) +
            ", failed=" + std::to_string(w.failed) + "; inspect " + writer.directory().string());
        std::rethrow_exception(error);
    }
}
} // namespace pubg_vision::app
