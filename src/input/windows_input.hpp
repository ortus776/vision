#pragma once
#include <windows.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include "input/button_edge.hpp"

namespace pubg_vision::input {
enum class Action { click, toggle, stop };
struct InputEvent { Action action; std::int64_t at_ms; bool focused; };
class WindowsInput {
public:
    explicit WindowsInput(std::function<std::int64_t()> clock);
    ~WindowsInput();
    WindowsInput(const WindowsInput&) = delete;
    WindowsInput& operator=(const WindowsInput&) = delete;
    void set_target(HWND target) noexcept { target_ = target; }
    void pump();
    [[nodiscard]] std::deque<InputEvent> take();
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
private:
    static LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp);
    void push(Action action);
    void cleanup() noexcept;
    std::function<std::int64_t()> clock_;
    HWND window_{}, target_{};
    bool registered_{}, toggle_registered_{}, stop_registered_{};
    LeftButtonEdge left_button_;
    std::uint64_t dropped_{};
    std::string error_;
    std::deque<InputEvent> events_;
};
} // namespace pubg_vision::input
