#pragma once

#include "config/app_config.hpp"
#include "core/logger.hpp"

namespace pubg_vision::capture {

// Lists visible top-level desktop windows that can be selected for one-shot capture.
void list_windows();

// Captures the configured central client-area ROI from a uniquely selected window and writes
// one PNG. Throws std::runtime_error with context when selection, duplication, or encoding fails.
void capture_once(const config::AppConfig& config, const core::Logger& logger);

} // namespace pubg_vision::capture
