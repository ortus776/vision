#include "render/windows_overlay.hpp"
#include <windows.h>
#include <cstring>
#include <stdexcept>

namespace pubg_vision::render {
namespace {
constexpr wchar_t overlay_class[] = L"PubgVisionDetectionOverlay";
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_CLOSE) { ShowWindow(window, SW_HIDE); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
void check(bool success, const char* action) {
    if (!success) throw std::runtime_error(std::string(action) + " (Win32 error " + std::to_string(GetLastError()) + ")");
}
}
struct WindowsOverlay::Impl {
    HWND window{};
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    void* bits{};
    core::Size size;
    ~Impl() {
        if (window) DestroyWindow(window);
        release_bitmap();
        if (dc) DeleteDC(dc);
    }
    void release_bitmap() noexcept {
        if (previous && dc) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        previous = nullptr; bitmap = nullptr; bits = nullptr; size = {};
    }
    void resize(core::Size next) {
        if (size.width == next.width && size.height == next.height) return;
        release_bitmap();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = next.width; info.bmiHeader.biHeight = -next.height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        check(bitmap != nullptr && bits != nullptr, "CreateDIBSection");
        previous = SelectObject(dc, bitmap);
        check(previous && previous != HGDI_ERROR, "SelectObject");
        size = next;
    }
};
WindowsOverlay::WindowsOverlay() : impl_(std::make_unique<Impl>()) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = procedure; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = overlay_class;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) check(false, "RegisterClassW");
    impl_->window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        overlay_class, L"PUBG Vision mock overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    check(impl_->window != nullptr, "CreateWindowExW overlay");
    // Require capture exclusion instead of silently feeding the drawn boxes back to the model.
    check(SetWindowDisplayAffinity(impl_->window, WDA_EXCLUDEFROMCAPTURE) != FALSE,
          "SetWindowDisplayAffinity; live overlay requires Windows 10 2004+ with DWM");
    DWORD affinity{};
    check(GetWindowDisplayAffinity(impl_->window, &affinity) != FALSE && affinity == WDA_EXCLUDEFROMCAPTURE,
          "GetWindowDisplayAffinity");
    impl_->dc = CreateCompatibleDC(nullptr);
    check(impl_->dc != nullptr, "CreateCompatibleDC");
}
WindowsOverlay::~WindowsOverlay() = default;
void WindowsOverlay::show(const inference::DetectionResult& result, Style style) {
    if (result.detections.empty()) { hide(); return; }
    const core::Size size{result.roi.width, result.roi.height};
    const auto surface = rasterize(size, result.detections, style);
    impl_->resize(size);
    std::memcpy(impl_->bits, surface.data(), surface.size());
    POINT position{result.roi.left, result.roi.top}, origin{};
    SIZE dimensions{size.width, size.height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    check(UpdateLayeredWindow(impl_->window, nullptr, &position, &dimensions, impl_->dc,
        &origin, 0, &blend, ULW_ALPHA) != FALSE, "UpdateLayeredWindow");
    check(SetWindowPos(impl_->window, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE, "SetWindowPos overlay");
}
void WindowsOverlay::hide() noexcept { ShowWindow(impl_->window, SW_HIDE); }
} // namespace pubg_vision::render
