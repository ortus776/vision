#include "app/live.hpp"
#include <stdexcept>
namespace pubg_vision::app {
void live(const config::AppConfig&, const core::Logger&) {
    throw std::runtime_error("live desktop capture/overlay is supported only on Windows");
}
} // namespace pubg_vision::app
