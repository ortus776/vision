#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
namespace pubg_vision::dataset {
struct TraceStats { std::uint64_t written{}, dropped{}; std::size_t high_water{}; };
// Disk work happens outside the producer mutex. Overload drops diagnostics, never frames.
class TraceWriter {
public:
    explicit TraceWriter(const std::filesystem::path& path, std::size_t capacity = 8192);
    ~TraceWriter();
    bool try_event(std::string json);
    void check_error() const;
    void finish();
    [[nodiscard]] TraceStats stats() const;
private:
    void run() noexcept;
    std::ofstream output_;
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    std::thread worker_;
    bool closing_{};
    std::exception_ptr error_;
    TraceStats stats_;
};
}
