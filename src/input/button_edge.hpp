#pragma once
namespace pubg_vision::input {
class LeftButtonEdge {
public:
    bool update(bool down, bool up) noexcept {
        const bool clicked = down && !held_;
        if (down) held_ = true;
        if (up) held_ = false;
        return clicked;
    }
private:
    bool held_{};
};
}
