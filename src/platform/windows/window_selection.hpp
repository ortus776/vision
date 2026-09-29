#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/geometry.hpp"

namespace pubg_vision::platform::windows {

struct WindowInfo {
    HWND handle{};
    std::wstring title;
    core::Rect client_screen;
    HMONITOR monitor{};
};

void set_per_monitor_dpi_awareness();
[[nodiscard]] std::vector<WindowInfo> visible_windows();
[[nodiscard]] WindowInfo find_unique_window(std::wstring_view title_substring);

} // namespace pubg_vision::platform::windows
