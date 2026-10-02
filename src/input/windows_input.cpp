#include "input/windows_input.hpp"
#include <stdexcept>

namespace pubg_vision::input {
namespace { constexpr wchar_t input_class[] = L"PubgVisionRawInput"; }
WindowsInput::WindowsInput(std::function<std::int64_t()> clock) : clock_(std::move(clock)) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = procedure; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = input_class;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("could not register input window class");
    window_ = CreateWindowExW(0, input_class, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, wc.hInstance, this);
    if (!window_) throw std::runtime_error("could not create input message window");
    RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_INPUTSINK, window_};
    registered_ = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
    toggle_registered_ = RegisterHotKey(window_, 8, MOD_NOREPEAT, VK_F8) != FALSE;
    stop_registered_ = RegisterHotKey(window_, 9, MOD_NOREPEAT, VK_F9) != FALSE;
    if (!registered_ || !toggle_registered_ || !stop_registered_) {
        cleanup();
        throw std::runtime_error("could not register Raw Input or F8/F9 (another app may own a hotkey)");
    }
}
void WindowsInput::cleanup() noexcept {
    if (toggle_registered_) UnregisterHotKey(window_, 8);
    if (stop_registered_) UnregisterHotKey(window_, 9);
    if (registered_) {
        RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    }
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
}
WindowsInput::~WindowsInput() { cleanup(); }
void WindowsInput::push(Action action) {
    if (events_.size() >= 64) {
        ++dropped_;
        if (action == Action::click) return;
        // Hotkeys remain usable during input overload, especially F9.
        events_.pop_front();
    }
    events_.push_back({action, clock_(), GetForegroundWindow() == target_});
}
LRESULT CALLBACK WindowsInput::procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) noexcept {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<WindowsInput*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self) {
        // Never let a C++ exception cross the Windows callback boundary.
        try {
            if (msg == WM_HOTKEY) {
                if (wp == 8) self->push(Action::toggle);
                if (wp == 9) self->push(Action::stop);
            } else if (msg == WM_INPUT) {
                RAWINPUT raw{};
                UINT size = sizeof(raw);
                const UINT read = GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
                                                  &raw, &size, sizeof(RAWINPUTHEADER));
                if (read == static_cast<UINT>(-1)) self->read_error_ = true;
                else if (read >= sizeof(RAWINPUTHEADER) && raw.header.dwType == RIM_TYPEMOUSE) {
                    const auto flags = raw.data.mouse.usButtonFlags;
                    if (self->left_button_.update((flags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0,
                                                  (flags & RI_MOUSE_LEFT_BUTTON_UP) != 0))
                        self->push(Action::click);
                }
            }
        } catch (...) { self->error_ = std::current_exception(); }
    }
    // DefWindowProc handles the required WM_INPUT foreground cleanup.
    return DefWindowProcW(window, msg, wp, lp);
}
void WindowsInput::pump() {
    MSG message{};
    while (PeekMessageW(&message, window_, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (error_) std::rethrow_exception(error_);
    if (read_error_) throw std::runtime_error("GetRawInputData failed");
}
std::deque<InputEvent> WindowsInput::take() { auto result = std::move(events_); events_.clear(); return result; }
} // namespace pubg_vision::input
