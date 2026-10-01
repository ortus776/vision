#include "app/collect.hpp"
#include <stdexcept>
namespace pubg_vision::app {
void collect(const config::AppConfig&, const core::Logger&) {
    throw std::runtime_error("dataset collection is available only on Windows");
}
}
