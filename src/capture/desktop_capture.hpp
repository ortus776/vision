#pragma once

#include "config/app_config.hpp"
#include "core/logger.hpp"
#include "core/frame.hpp"
#include <memory>
#include <optional>

namespace pubg_vision::capture {

struct CaptureStats {
    std::uint64_t calls{}, frames{}, timeouts{}, acquired{}, rejected_updates{};
    double acquire_ms{}, map_ms{}, copy_ms{}, max_map_ms{};
};
struct CaptureTimings {
    std::int64_t acquire_start{}, acquire_end{}, map_start{}, map_end{}, copy_start{}, copy_end{};
};

// Lists visible top-level desktop windows that can be selected for one-shot capture.
void list_windows();

// Captures the configured central client-area ROI from a uniquely selected window and writes
// one PNG. Throws std::runtime_error with context when selection, duplication, or encoding fails.
void capture_once(const config::AppConfig& config, const core::Logger& logger);

// Thread-affine persistent source. prepare refreshes geometry and reports generation changes.
class DesktopCapture {
public:
    explicit DesktopCapture(bool hold_frame = false);
    ~DesktopCapture();
    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;
    bool prepare(std::uintptr_t window, core::Size roi);
    void reset();
    [[nodiscard]] CaptureStats stats() const;
    [[nodiscard]] CaptureTimings last_timings() const;
    [[nodiscard]] std::optional<core::Frame> next(std::uint32_t timeout_ms = 5);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void encode_png(const std::filesystem::path& path, core::Size size,
                const std::vector<std::uint8_t>& pixels);
void encode_png_no_filter(const std::filesystem::path& path, core::Size size,
                          const std::vector<std::uint8_t>& pixels);

} // namespace pubg_vision::capture
