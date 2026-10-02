#pragma once
#include <memory>
#include "inference/pipeline.hpp"
#include "render/raster.hpp"

namespace pubg_vision::render {
// UI-thread owned, click-through window. All input remains with the selected app.
class WindowsOverlay {
public:
    WindowsOverlay();
    ~WindowsOverlay();
    WindowsOverlay(const WindowsOverlay&) = delete;
    WindowsOverlay& operator=(const WindowsOverlay&) = delete;
    void show(const inference::DetectionResult& result, Style style);
    void hide() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pubg_vision::render
