#include "app/live.hpp"
#include "app/inference_model.hpp"
#include "capture/desktop_capture.hpp"
#include "input/windows_input.hpp"
#include "platform/windows/window_selection.hpp"
#include "render/windows_overlay.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace pubg_vision::app {
namespace {
bool same(core::Rect a, core::Rect b) {
    return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
struct Target {
    HWND handle{};
    core::Rect client, roi;
    HMONITOR monitor{};
    std::uint64_t generation{};
    bool active{};
};
struct CaptureStatus { std::uint64_t generation{}, failures{}; bool ready{}; std::string error; };
class CaptureWorker {
public:
    CaptureWorker(inference::Pipeline& pipeline, inference::Clock clock, core::Size size, bool synthetic)
        : pipeline_(pipeline), clock_(std::move(clock)), size_(size), synthetic_(synthetic) {
        if (!size.valid() || static_cast<std::uint64_t>(size.width) * size.height > 16U * 1024U * 1024U)
            throw std::invalid_argument("live ROI exceeds the supported BGRA buffer size");
        worker_ = std::thread([this] { run(); });
    }
    ~CaptureWorker() { stop(); }
    void set(Target target) {
        std::lock_guard lock(mutex_);
        if (target_.generation == target.generation && target_.active == target.active) return;
        target_ = target; wake_.notify_one();
    }
    CaptureStatus status() const { std::lock_guard lock(mutex_); return status_; }
    void stop() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        wake_.notify_one(); if (worker_.joinable()) worker_.join();
    }
private:
    void run() noexcept {
        // DesktopCapture and all D3D11/DXGI resources stay on this thread.
        try {
            std::unique_ptr<capture::DesktopCapture> source;
            if (!synthetic_) source = std::make_unique<capture::DesktopCapture>();
            std::uint64_t local_generation = 0, frame_id = 0;
            for (;;) {
                Target target;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock, [&] { return stopping_ || target_.active; });
                    if (stopping_) return;
                    target = target_;
                }
                try {
                    std::optional<core::Frame> frame;
                    if (synthetic_) {
                        frame.emplace(); frame->size = frame->source_size = size_; frame->roi = target.roi;
                        frame->pixels.assign(static_cast<std::size_t>(size_.width) * size_.height * 4U, 48);
                        for (std::size_t i = 3; i < frame->pixels.size(); i += 4U) frame->pixels[i] = 255;
                    } else {
                        if (target.generation != local_generation) { source->reset(); local_generation = target.generation; }
                        source->prepare(reinterpret_cast<std::uintptr_t>(target.handle), size_);
                        frame = source->next(10);
                    }
                    { std::lock_guard lock(mutex_); status_.generation = target.generation; status_.ready = true; status_.error.clear(); }
                    if (frame && same(frame->roi, target.roi) && GetForegroundWindow() == target.handle) {
                        // Reject a geometry change that occurred during the copy.
                        const auto current = platform::windows::inspect_window(target.handle);
                        if (same(current.client_screen, target.client) && current.monitor == target.monitor) {
                            frame->captured_ms = clock_(); frame->generation = target.generation;
                            pipeline_.submit({++frame_id, std::move(*frame)});
                        }
                    }
                } catch (const std::exception& error) {
                    if (source) source->reset();
                    local_generation = 0;
                    std::unique_lock lock(mutex_);
                    status_.generation = target.generation; status_.ready = false;
                    status_.error = error.what(); ++status_.failures;
                    const auto failed_generation = target.generation;
                    // Recovery wait is interruptible by stop or a new source state.
                    wake_.wait_for(lock, std::chrono::seconds(1), [&] {
                        return stopping_ || !target_.active || target_.generation != failed_generation;
                    });
                    if (stopping_) return;
                }
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(synthetic_ ? 16 : 2), [&] {
                    return stopping_ || !target_.active || target_.generation != target.generation;
                });
                if (stopping_) return;
            }
        } catch (...) {
            std::lock_guard lock(mutex_); status_.ready = false; ++status_.failures; error_ = std::current_exception();
        }
    }
public:
    void check_error() const {
        std::exception_ptr error;
        { std::lock_guard lock(mutex_); error = error_; }
        if (error) std::rethrow_exception(error);
    }
private:
    inference::Pipeline& pipeline_;
    inference::Clock clock_;
    core::Size size_;
    bool synthetic_{};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Target target_;
    CaptureStatus status_;
    bool stopping_{};
    std::exception_ptr error_;
    std::thread worker_;
};
}
void live(const config::AppConfig& config, const core::Logger& logger) {
    using namespace std::chrono;
    namespace windows = platform::windows;
    windows::set_per_monitor_dpi_awareness();
    const auto title = windows::wide_from_utf8(config.window_title);
    auto selected = windows::find_unique_window(title);
    const auto start = steady_clock::now();
    const inference::Clock now = [&] { return duration_cast<milliseconds>(steady_clock::now() - start).count(); };
    auto model = make_model(config);
    const std::string backend(model->backend_name());
    inference::Pipeline pipeline(std::move(model), config.inference, now);
    pipeline.invalidate(1, false);
    render::WindowsOverlay overlay;
    input::WindowsInput input(now);
    input.set_target(selected.handle);
    CaptureWorker capture(pipeline, now, config.roi, config.synthetic_source);
    Target previous;
    std::uint64_t generation = 1, seen_failures = 0, drawn_frame = 0;
    bool enabled = true, stopping = false, drawn = false;
    std::string reported_state;
    auto retry_at = steady_clock::now();
    logger.write(core::LogLevel::info, "LIVE backend=" + backend + "; F8 pauses/resumes; F9 stops. Focus selected window.");
    if (backend == "mock") logger.write(core::LogLevel::info, "Mock model produces random detections.");
    logger.write(core::LogLevel::info, config.synthetic_source ? "Source: synthetic frames for overlay testing" : "Source: DXGI desktop capture");
    while (!stopping) {
        input.pump();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) stopping = true;
            else { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        for (const auto& event : input.take()) {
            if (event.action == input::Action::stop) stopping = true;
            else if (event.action == input::Action::toggle) enabled = !enabled;
        }
        pipeline.check_error(); capture.check_error();
        Target target;
        std::string state;
        try {
            if (!IsWindow(selected.handle)) {
                if (steady_clock::now() < retry_at) throw std::runtime_error("waiting for selected window");
                retry_at = steady_clock::now() + seconds(1);
                selected = windows::find_unique_window(title); input.set_target(selected.handle);
            }
            const auto current = windows::inspect_window(selected.handle);
            target.handle = current.handle; target.client = current.client_screen; target.monitor = current.monitor;
            target.roi = core::centered_rect(current.client_screen, config.roi);
            target.active = enabled && !stopping && GetForegroundWindow() == current.handle;
            state = !enabled ? "PAUSED by F8" : (!target.active ? "PAUSED: selected window is not foreground" : "LIVE (" + backend + "): processing selected window");
        } catch (const std::exception& error) { state = "PAUSED: " + std::string(error.what()); }
        const auto status = capture.status();
        const bool failed = status.failures != seen_failures;
        if (failed) {
            seen_failures = status.failures;
            logger.write(core::LogLevel::warning, "Live capture recovering: " + status.error);
        }
        if (target.handle != previous.handle || target.monitor != previous.monitor ||
            !same(target.client, previous.client) || !same(target.roi, previous.roi) || target.active != previous.active) {
            ++generation; overlay.hide(); drawn = false;
        }
        // Capture recovery uses the same generation: its one-second retry must not be reset each UI tick.
        target.generation = generation;
        pipeline.invalidate(generation, target.active && (!failed && (status.ready || status.failures == 0)));
        capture.set(target); previous = target;
        if (state != reported_state) { logger.write(core::LogLevel::info, state); reported_state = state; }
        const auto result = pipeline.latest();
        if (!stopping && target.active && status.ready && status.generation == generation && result &&
            inference::visible(*result, generation, target.roi, now(), config.inference.ttl_ms)) {
            if (!drawn || drawn_frame != result->frame_id) {
                overlay.show(*result, config.overlay_style); drawn_frame = result->frame_id; drawn = true;
            }
        } else { overlay.hide(); drawn = false; }
        MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    overlay.hide(); capture.stop(); pipeline.stop();
    const auto stats = pipeline.stats();
    logger.write(core::LogLevel::info, "Live finished: submitted=" + std::to_string(stats.submitted) +
        ", processed=" + std::to_string(stats.processed) + ", replaced=" + std::to_string(stats.replaced) +
        ", stale discarded=" + std::to_string(stats.stale_discarded));
}
} // namespace pubg_vision::app
