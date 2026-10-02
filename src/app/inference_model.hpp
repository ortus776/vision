#pragma once
#include "config/app_config.hpp"
#include "inference/mock_model.hpp"
#include <memory>

namespace pubg_vision::app {
// Integration point: replace/add a Model adapter here when the trained model is ready.
// Keep its runtime session inside the adapter; live and demo share this factory.
inline std::unique_ptr<inference::Model> make_model(const config::AppConfig& config) {
    return std::make_unique<inference::MockModel>(config.model, config.mock_seed);
}
} // namespace pubg_vision::app
