#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pubg_vision::collection {

using Millis = std::int64_t; // Monotonic milliseconds relative to session start.
struct Settings {
    std::vector<Millis> burst_offsets{0, 100, 200};
    Millis periodic_interval{5000};
    Millis merge_window{20};
    Millis max_lateness{100};
    std::size_t max_pending{32};
};
struct Request {
    std::uint64_t event_id{};
    std::uint64_t burst_id{}; // Zero for timer requests.
    std::size_t burst_index{};
    Millis planned_ms{};
    std::string reason;
};
struct SchedulerStats {
    std::uint64_t requested{}, expired{}, overflow{}, cancelled{}, merged{};
};
using EventSink = std::function<void(const std::string&)>;
[[nodiscard]] std::string requests_json(const std::vector<Request>& requests);

// This class owns deadlines only. No Win32, disk, sleeps, or wall-clock time.
class CaptureScheduler {
public:
    explicit CaptureScheduler(Settings settings = {}, EventSink sink = {});
    void set_active(bool active, Millis now, const std::string& reason);
    void cancel(Millis now, const std::string& reason);
    void click(Millis now);
    [[nodiscard]] std::vector<Request> take_due(Millis now);
    [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const SchedulerStats& stats() const noexcept { return stats_; }
private:
    void add(Request request, Millis now);
    void log(const char* type, const Request& request, Millis now, const std::string& reason);
    void skip_timers(std::uint64_t count, Millis now, const char* reason);
    Settings settings_;
    EventSink sink_;
    std::vector<Request> pending_;
    SchedulerStats stats_;
    bool active_{};
    Millis next_timer_{};
    std::uint64_t next_event_{1}, next_burst_{1};
};
} // namespace pubg_vision::collection
