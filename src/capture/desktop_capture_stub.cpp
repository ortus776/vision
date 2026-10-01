#include "capture/desktop_capture.hpp"

#include <stdexcept>

namespace pubg_vision::capture {

void list_windows() {
    throw std::runtime_error("window selection is available only on Windows");
}

void capture_once(const config::AppConfig&, const core::Logger&) {
    throw std::runtime_error("desktop capture is available only on Windows");
}

struct DesktopCapture::Impl {};
DesktopCapture::DesktopCapture() : impl_(std::make_unique<Impl>()) {}
DesktopCapture::~DesktopCapture() = default;
bool DesktopCapture::prepare(std::uintptr_t, core::Size) {
    throw std::runtime_error("desktop capture is available only on Windows");
}
void DesktopCapture::reset() {}
std::optional<core::Frame> DesktopCapture::next(std::uint32_t) {
    throw std::runtime_error("desktop capture is available only on Windows");
}
void encode_png(const std::filesystem::path&, core::Size, const std::vector<std::uint8_t>&) {
    throw std::runtime_error("PNG encoding is available only on Windows");
}

} // namespace pubg_vision::capture
