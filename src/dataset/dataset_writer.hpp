#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include "collection/capture_scheduler.hpp"
#include "core/frame.hpp"

namespace pubg_vision::dataset {
using PngEncoder = std::function<void(const std::filesystem::path&, core::Size,
                                    const std::vector<std::uint8_t>&)>;
struct WriterStats { std::uint64_t saved{}, rejected{}, failed{}; std::size_t high_water{}; };
class DatasetWriter {
public:
    DatasetWriter(const std::filesystem::path& root, std::size_t capacity,
                  const std::string& session_metadata, PngEncoder encoder);
    ~DatasetWriter();
    DatasetWriter(const DatasetWriter&) = delete;
    DatasetWriter& operator=(const DatasetWriter&) = delete;
    // Does not wait for PNG encoding. Queue full is a recorded skip.
    bool try_enqueue(core::Frame frame, std::vector<collection::Request> requests);
    void event(const std::string& json);
    void check_error() const;
    void finish(); // Drain accepted frames, join, then propagate any disk error.
    [[nodiscard]] WriterStats stats() const;
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] const std::string& session_id() const noexcept { return session_id_; }
private:
    struct Job { core::Frame frame; std::vector<collection::Request> requests; std::uint64_t id; };
    void run() noexcept;
    void write(const Job& job);
    void event_unlocked(const std::string& json);
    std::filesystem::path directory_;
    std::string session_id_;
    std::size_t capacity_;
    PngEncoder encoder_;
    std::ofstream frames_, events_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::thread worker_;
    std::exception_ptr error_;
    bool closing_{};
    WriterStats stats_;
    std::uint64_t next_id_{1};
};
} // namespace pubg_vision::dataset
