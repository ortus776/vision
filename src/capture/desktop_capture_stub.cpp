#include "capture/desktop_capture.hpp"

#include <stdexcept>

namespace pubg_vision::capture {

void list_windows() {
    throw std::runtime_error("window selection is available only on Windows");
}

void capture_once(const config::AppConfig&, const core::Logger&) {
    throw std::runtime_error("desktop capture is available only on Windows");
}

} // namespace pubg_vision::capture
