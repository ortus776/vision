#pragma once
#include "config/app_config.hpp"
#include "core/logger.hpp"

namespace pubg_vision::app {
void live(const config::AppConfig& config, const core::Logger& logger);
void inference_demo(const config::AppConfig& config, const core::Logger& logger);
} // namespace pubg_vision::app
