#include <windows.h>

#include "platform/windows/window_selection.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace pubg_vision::platform::windows {
namespace {

BOOL CALLBACK collect_window(HWND handle, LPARAM parameter) {
    auto& windows = *reinterpret_cast<std::vector<WindowInfo>*>(parameter);
    if (!IsWindowVisible(handle) || IsIconic(handle) || GetWindow(handle, GW_OWNER) != nullptr) {
        return TRUE;
    }

    const int title_length = GetWindowTextLengthW(handle);
    if (title_length <= 0) {
        return TRUE;
    }

    std::wstring title(static_cast<std::size_t>(title_length) + 1U, L'\0');
    const int copied = GetWindowTextW(handle, title.data(), title_length + 1);
    if (copied <= 0) {
        return TRUE;
    }
    title.resize(static_cast<std::size_t>(copied));

    RECT client{};
    if (!GetClientRect(handle, &client) || client.right <= client.left || client.bottom <= client.top) {
        return TRUE;
    }

    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    if (!ClientToScreen(handle, &top_left) || !ClientToScreen(handle, &bottom_right)) {
        return TRUE;
    }

    const core::Rect client_screen{
        top_left.x,
        top_left.y,
        bottom_right.x - top_left.x,
        bottom_right.y - top_left.y,
    };
    if (!client_screen.valid()) {
        return TRUE;
    }

    windows.push_back(WindowInfo{
        handle,
        std::move(title),
        client_screen,
        MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST),
    });
    return TRUE;
}

} // namespace

void set_per_monitor_dpi_awareness() {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        const auto error = GetLastError();
        // An executable manifest or an already-created window may have set the mode first.
        const auto current = GetThreadDpiAwarenessContext();
        if (!AreDpiAwarenessContextsEqual(current, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            throw std::runtime_error("could not enable per-monitor DPI awareness (Win32 error " +
                                     std::to_string(error) + ")");
        }
    }
}

std::vector<WindowInfo> visible_windows() {
    std::vector<WindowInfo> windows;
    if (!EnumWindows(collect_window, reinterpret_cast<LPARAM>(&windows))) {
        throw std::runtime_error("EnumWindows failed");
    }
    std::sort(windows.begin(), windows.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.title < rhs.title;
    });
    return windows;
}

WindowInfo find_unique_window(std::wstring_view title_substring) {
    if (title_substring.empty()) {
        throw std::invalid_argument("window title substring must not be empty");
    }

    auto windows = visible_windows();
    std::vector<WindowInfo> matches;
    for (auto& window : windows) {
        if (window.title.find(title_substring) != std::wstring::npos) {
            matches.push_back(std::move(window));
        }
    }

    if (matches.empty()) {
        throw std::runtime_error("no visible window title contains the requested text; run --list-windows");
    }
    if (matches.size() > 1U) {
        throw std::runtime_error("window title matches more than one window; use --list-windows and a more specific title");
    }
    return std::move(matches.front());
}

} // namespace pubg_vision::platform::windows
