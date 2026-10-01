#pragma once

#include "config/app_config.hpp"
#include "core/logger.hpp"
#include "core/frame.hpp"
#include <memory>
#include <optional>

namespace pubg_vision::capture {

// Lists visible top-level desktop windows that can be selected for one-shot capture.
void list_windows();

// Captures the configured central client-area ROI from a uniquely selected window and writes
// one PNG. Throws std::runtime_error with context when selection, duplication, or encoding fails.
void capture_once(const config::AppConfig& config, const core::Logger& logger);

// Thread-affine persistent source. prepare refreshes geometry and reports generation changes.
class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();
    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;
    bool prepare(std::uintptr_t window, core::Size roi);
    void reset();
    [[nodiscard]] std::optional<core::Frame> next(std::uint32_t timeout_ms = 5);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void encode_png(const std::filesystem::path& path, core::Size size,
                const std::vector<std::uint8_t>& pixels);

} // namespace pubg_vision::capture
