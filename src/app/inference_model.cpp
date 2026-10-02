#include "app/inference_model.hpp"
#include "config/app_config.hpp"
#include "inference/mock_model.hpp"
#include <stdexcept>

namespace pubg_vision::app {
std::unique_ptr<inference::Model> make_model(const config::AppConfig& config) {
    if (config.backend == "mock") return std::make_unique<inference::MockModel>(config.model, config.mock_seed);
    throw std::invalid_argument("unsupported model backend: " + config.backend);
}
} // namespace pubg_vision::app
