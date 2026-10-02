#pragma once
#include "inference/model.hpp"
#include <memory>

namespace pubg_vision::config { struct AppConfig; }

namespace pubg_vision::app {
// Integration point: replace/add a Model adapter here when the trained model is ready.
// Keep its runtime session inside the adapter; live and demo share this factory.
std::unique_ptr<inference::Model> make_model(const config::AppConfig& config);
} // namespace pubg_vision::app
