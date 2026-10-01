#include "collection/capture_scheduler.hpp"
#include "dataset/dataset_writer.hpp"
#include "core/json.hpp"
#include "input/button_edge.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace pubg_vision;
int failures{};
void expect(bool ok, const char* name) { if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; } }
void scheduler_tests() {
    input::LeftButtonEdge button;
    expect(button.update(true, false), "button down creates an edge");
    expect(!button.update(true, false) && !button.update(false, false), "holding button creates one burst");
    expect(!button.update(false, true) && button.update(true, false), "release rearms the next click");
    collection::CaptureScheduler s;
    s.click(0); expect(s.pending() == 0, "ready ignores clicks");
    s.set_active(true, 1000, "start"); s.click(5990);
    auto due = s.take_due(6000);
    expect(due.size() == 2 && due[0].reason == "click" && due[1].reason == "timer",
           "timer and click merge without losing origins");
    expect(s.take_due(6089).empty(), "future deadlines are not captured early");
    due = s.take_due(6090); expect(due.size() == 1 && due[0].burst_index == 1, "second burst frame at 100ms");
    due = s.take_due(6190); expect(due.size() == 1 && due[0].burst_index == 2, "third burst frame at 200ms");
    expect(s.take_due(10999).empty(), "click never moves timer phase");
    due = s.take_due(11000); expect(due.size() == 1 && due[0].reason == "timer", "timer remains anchored");
    s.click(11100); s.set_active(false, 11101, "focus");
    expect(s.pending() == 0 && s.stats().cancelled == 3, "focus pause cancels queued burst");
    s.set_active(true, 20000, "focus");
    expect(s.take_due(24999).empty(), "resume starts a new timer interval");
    expect(s.take_due(25000).size() == 1, "no catch-up after pause");
    s.click(25100); expect(s.take_due(25201).size() == 1 && s.stats().expired == 1,
                            "lateness drops expired request but preserves next frame");
    s.cancel(25201, "resize"); expect(s.pending() == 0, "resize cancels old geometry requests");
    collection::Settings small; small.max_pending = 2;
    collection::CaptureScheduler bounded(small); bounded.set_active(true, 0, "start"); bounded.click(0);
    expect(bounded.pending() == 2 && bounded.stats().overflow == 1, "pending queue is bounded");
    collection::Settings exact; exact.merge_window = 0;
    collection::CaptureScheduler boundary(exact); boundary.set_active(true, 0, "start"); boundary.click(0);
    expect(boundary.take_due(100).size() == 1 && boundary.stats().expired == 0, "100ms late still permitted");
    expect(boundary.take_due(101).size() == 1, "due requests outside merge window stay separate");
    bool invalid = false;
    try { small.burst_offsets = {0, 0}; collection::CaptureScheduler bad(small); }
    catch (const std::invalid_argument&) { invalid = true; }
    expect(invalid, "duplicate burst offsets rejected");
    expect(core::json_string("a\n\"\\") == "\"a\\u000a\\\"\\\\\"", "JSON escapes strings");
}

// Synthetic elapsed time, not a claim of a wall-clock hour on a graphics device.
void simulated_scenarios(collection::Millis duration, bool pauses) {
    std::set<std::uint64_t> accounted;
    std::uint64_t terminal_count{};
    collection::CaptureScheduler s({}, [&](const std::string& line) {
        if (line.find("\"type\":\"cancelled\"") != std::string::npos ||
            line.find("\"type\":\"skipped\"") != std::string::npos) {
            const auto start = line.find("\"event_id\":") + 11;
            const auto id = std::stoull(line.substr(start));
            expect(accounted.insert(id).second, "request has one terminal outcome");
            ++terminal_count;
        }
    });
    std::size_t high_water{};
    std::uint64_t delivered{}, timer_count{};
    s.set_active(true, 0, "start");
    for (collection::Millis now = 0; now <= duration; now += 10) {
        if (pauses && now % 30000 == 10000) s.set_active(false, now, "focus");
        if (pauses && now % 30000 == 11000) s.set_active(true, now, "focus");
        if (now % 70 == 0) s.click(now);
        if (pauses && now % 60000 == 20000) s.cancel(now, "resize");
        if (pauses && now % 10000 < 300) continue; // Deliberate scheduler stall.
        const auto due = s.take_due(now);
        for (const auto& r : due) {
            expect(accounted.insert(r.event_id).second, "delivered request is unique");
            ++delivered; if (r.reason == "timer") ++timer_count;
        }
        high_water = (std::max)(high_water, s.pending());
    }
    s.set_active(false, duration, "stop");
    expect(s.stats().requested == terminal_count + delivered, "all scenario requests are explained");
    expect(high_water <= 32, "synthetic queue stays bounded");
    if (!pauses) expect(timer_count == static_cast<std::uint64_t>(duration / 5000), "hour timer has exact count independent of clicks");
    std::cout << "Synthetic " << duration / 60000 << "min: requested=" << s.stats().requested
              << ", delivered=" << delivered << ", terminal_skips=" << terminal_count
              << ", pending_high_water=" << high_water << '\n';
}
core::Frame frame() {
    core::Frame f; f.size = {2, 2}; f.source_size = {800, 600}; f.roi = {10, 20, 2, 2};
    f.pixels.assign(16, 255); return f;
}
void writer_tests() {
    const auto root = std::filesystem::path("build/collection-unit") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::mutex gate; std::condition_variable signal; bool entered = false, release = false;
    dataset::DatasetWriter w(root, 1, "{}", [&](const auto& path, auto, const auto&) {
        { std::unique_lock lock(gate); entered = true; signal.notify_all();
          signal.wait(lock, [&] { return release; }); }
        std::ofstream out(path, std::ios::binary); out << "encoded"; out.close();
        if (!out) throw std::runtime_error("test file write failed");
    });
    std::vector<collection::Request> request{{1, 1, 0, 0, "click"}};
    expect(w.try_enqueue(frame(), request), "first frame accepted");
    { std::unique_lock lock(gate); signal.wait(lock, [&] { return entered; }); }
    request[0].event_id = 2; expect(w.try_enqueue(frame(), request), "one waiting frame accepted");
    request[0].event_id = 3; expect(!w.try_enqueue(frame(), request), "slow encoder causes recorded queue rejection");
    { std::scoped_lock lock(gate); release = true; } signal.notify_all(); w.finish();
    expect(w.stats().saved == 2 && w.stats().rejected == 1 && w.stats().high_water == 1,
           "stop drains exactly the accepted bounded queue");
    expect(std::filesystem::exists(w.directory() / "images/frame_00000001.png") &&
           !std::filesystem::exists(w.directory() / "images/frame_00000001.png.tmp"),
           "atomic rename removes temporary name");
    std::ifstream manifest(w.directory() / "frames.jsonl"); std::string line; int lines{};
    while (std::getline(manifest, line)) { ++lines; expect(line.find("\"source_qpc\":null") != std::string::npos,
                                                        "missing source time is explicit null"); }
    expect(lines == 2, "manifest only references successfully written frames");
    dataset::DatasetWriter failed(root, 2, "{}", [](const auto&, auto, const auto&) {
        throw std::runtime_error("simulated disk full");
    });
    failed.try_enqueue(frame(), request);
    bool propagated = false;
    try { failed.finish(); } catch (const std::runtime_error&) { propagated = true; }
    expect(propagated && failed.stats().failed == 1 && failed.stats().saved == 0,
           "disk failure stops worker and propagates to caller");
    expect(std::filesystem::file_size(failed.directory() / "frames.jsonl") == 0,
           "failed PNG creates no manifest entry");
    const auto blocker = root / "file-instead-of-directory";
    { std::ofstream out(blocker); out << 'x'; }
    propagated = false;
    try { dataset::DatasetWriter invalid(blocker, 1, "{}", [](const auto&, auto, const auto&) {}); }
    catch (const std::filesystem::filesystem_error&) { propagated = true; }
    expect(propagated, "invalid disk destination is rejected at startup");
}
} // namespace
int main() {
    try { scheduler_tests(); simulated_scenarios(600000, true); simulated_scenarios(3600000, false); writer_tests(); }
    catch (const std::exception& ex) { ++failures; std::cerr << ex.what() << '\n'; }
    return failures ? 1 : 0;
}
