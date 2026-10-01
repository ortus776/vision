#include "collection/capture_scheduler.hpp"
#include "core/json.hpp"

#include <algorithm>
#include <stdexcept>
#include <sstream>

namespace pubg_vision::collection {
std::string requests_json(const std::vector<Request>& requests) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& r : requests) {
        if (!first) out << ',';
        first = false;
        out << "{\"event_id\":" << r.event_id << ",\"burst_id\":" << r.burst_id
            << ",\"burst_index\":" << r.burst_index << ",\"planned_ms\":" << r.planned_ms
            << ",\"reason\":" << core::json_string(r.reason) << '}';
    }
    return out.str() + ']';
}
CaptureScheduler::CaptureScheduler(Settings settings, EventSink sink)
    : settings_(std::move(settings)), sink_(std::move(sink)) {
    if (settings_.periodic_interval <= 0 || settings_.merge_window < 0 ||
        settings_.max_lateness < 0 || settings_.max_pending == 0 ||
        settings_.burst_offsets.empty() || settings_.burst_offsets.front() != 0 ||
        !std::is_sorted(settings_.burst_offsets.begin(), settings_.burst_offsets.end()) ||
        std::adjacent_find(settings_.burst_offsets.begin(), settings_.burst_offsets.end()) !=
            settings_.burst_offsets.end() || settings_.max_pending > 4096 ||
        settings_.burst_offsets.back() > 60000 || settings_.periodic_interval > 3600000) {
        throw std::invalid_argument("invalid collection scheduler settings");
    }
}
void CaptureScheduler::log(const char* type, const Request& r, Millis now, const std::string& reason) {
    if (sink_) sink_("{\"type\":" + core::json_string(type) + ",\"at_ms\":" +
        std::to_string(now) + ",\"reason\":" + core::json_string(reason) +
        ",\"requests\":" + requests_json({r}) + '}');
}
void CaptureScheduler::add(Request r, Millis now) {
    ++stats_.requested;
    log("requested", r, now, r.reason);
    if (pending_.size() >= settings_.max_pending) {
        ++stats_.overflow;
        log("skipped", r, now, "pending_queue_full");
        return;
    }
    pending_.push_back(std::move(r));
    std::stable_sort(pending_.begin(), pending_.end(), [](const auto& a, const auto& b) {
        return a.planned_ms < b.planned_ms;
    });
}
void CaptureScheduler::set_active(bool active, Millis now, const std::string& reason) {
    if (active == active_) return;
    active_ = active;
    if (active) next_timer_ = now + settings_.periodic_interval;
    else cancel(now, reason);
    if (sink_) sink_("{\"type\":\"state\",\"active\":" + std::string(active ? "true" : "false") +
        ",\"at_ms\":" + std::to_string(now) + ",\"reason\":" + core::json_string(reason) + '}');
}
void CaptureScheduler::cancel(Millis now, const std::string& reason) {
    for (const auto& r : pending_) {
        ++stats_.cancelled;
        log("cancelled", r, now, reason);
    }
    pending_.clear();
    next_timer_ = now + settings_.periodic_interval;
}
void CaptureScheduler::click(Millis now) {
    if (!active_) return;
    const auto burst = next_burst_++;
    for (std::size_t i = 0; i < settings_.burst_offsets.size(); ++i)
        add({next_event_++, burst, i, now + settings_.burst_offsets[i], "click"}, now);
}
std::vector<Request> CaptureScheduler::take_due(Millis now) {
    if (!active_) return {};
    // Account for every missed deadline; never enqueue a backlog of stale timer images.
    while (next_timer_ <= now) {
        Request r{next_event_++, 0, 0, next_timer_, "timer"};
        next_timer_ += settings_.periodic_interval;
        if (now - r.planned_ms > settings_.max_lateness) {
            ++stats_.requested;
            log("requested", r, now, r.reason);
            ++stats_.expired;
            log("skipped", r, now, "late");
        } else add(std::move(r), now);
    }
    while (!pending_.empty() && now - pending_.front().planned_ms > settings_.max_lateness) {
        ++stats_.expired;
        log("skipped", pending_.front(), now, "late");
        pending_.erase(pending_.begin());
    }
    if (pending_.empty() || pending_.front().planned_ms > now) return {};
    const auto merge_end = pending_.front().planned_ms + settings_.merge_window;
    std::vector<Request> due;
    // Future deadlines are never captured early, even within the merge window.
    while (!pending_.empty() && pending_.front().planned_ms <= now &&
           pending_.front().planned_ms <= merge_end) {
        due.push_back(std::move(pending_.front()));
        pending_.erase(pending_.begin());
    }
    if (due.size() > 1) {
        stats_.merged += due.size() - 1;
        if (sink_) sink_("{\"type\":\"merged\",\"at_ms\":" + std::to_string(now) +
            ",\"requests\":" + requests_json(due) + '}');
    }
    return due;
}
} // namespace pubg_vision::collection
