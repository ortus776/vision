#pragma once
#include <string_view>

namespace pubg_vision::collection {
enum class CollectionState { waiting, user_paused, focus_paused, recovering, collecting, stopping };

inline CollectionState collection_state(bool started, bool enabled, bool ready,
                                        bool focused, bool stopping) noexcept {
    if (stopping) return CollectionState::stopping;
    if (!enabled) return started ? CollectionState::user_paused : CollectionState::waiting;
    if (!ready) return CollectionState::recovering;
    if (!focused) return CollectionState::focus_paused;
    return CollectionState::collecting;
}
inline std::string_view state_name(CollectionState state) noexcept {
    switch (state) {
    case CollectionState::waiting: return "waiting_for_start";
    case CollectionState::user_paused: return "user_paused";
    case CollectionState::focus_paused: return "focus_paused";
    case CollectionState::recovering: return "recovering";
    case CollectionState::collecting: return "collecting";
    case CollectionState::stopping: return "stopping";
    }
    return "unknown";
}
inline std::string_view state_message(CollectionState state) noexcept {
    switch (state) {
    case CollectionState::waiting: return "WAITING: collection is OFF. Press F8 (or Fn+F8 on your keyboard), then focus the selected window.";
    case CollectionState::user_paused: return "PAUSED by F8. Press F8 to resume.";
    case CollectionState::focus_paused: return "PAUSED: selected window is not in the foreground. Switch to the selected window.";
    case CollectionState::recovering: return "PAUSED: capture source unavailable; retrying automatically.";
    case CollectionState::collecting: return "COLLECTING: clicks and the independent timer can save PNG files.";
    case CollectionState::stopping: return "STOPPING: finishing accepted PNG writes.";
    }
    return "Unknown collection state";
}
} // namespace pubg_vision::collection
