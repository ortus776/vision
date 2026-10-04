#include "dataset/trace_writer.hpp"
#include <stdexcept>
namespace pubg_vision::dataset {
TraceWriter::TraceWriter(const std::filesystem::path& path, std::size_t capacity)
    : output_(path, std::ios::binary), capacity_(capacity) {
    if (!capacity || !output_) throw std::runtime_error("could not open collector trace");
    worker_ = std::thread([this] { run(); });
}
TraceWriter::~TraceWriter() { try { finish(); } catch (...) {} }
bool TraceWriter::try_event(std::string json) {
    std::scoped_lock lock(mutex_);
    if (closing_ || error_ || queue_.size() >= capacity_) { ++stats_.dropped; return false; }
    queue_.push_back(std::move(json));
    if (queue_.size() > stats_.high_water) stats_.high_water = queue_.size();
    wake_.notify_one(); return true;
}
TraceStats TraceWriter::stats() const { std::scoped_lock lock(mutex_); return stats_; }
void TraceWriter::check_error() const {
    std::scoped_lock lock(mutex_); if (error_) std::rethrow_exception(error_);
}
void TraceWriter::finish() {
    { std::scoped_lock lock(mutex_); closing_ = true; }
    wake_.notify_all(); if (worker_.joinable()) worker_.join(); check_error();
}
void TraceWriter::run() noexcept {
    try {
        for (;;) {
            std::deque<std::string> batch;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return closing_ || !queue_.empty(); });
                if (queue_.empty()) break;
                batch.swap(queue_);
            }
            for (const auto& line : batch) output_ << line << '\n';
            if (!output_) throw std::runtime_error("could not append collector trace");
            { std::scoped_lock lock(mutex_); stats_.written += batch.size(); }
        }
        output_.flush();
        if (!output_) throw std::runtime_error("could not flush collector trace");
    } catch (...) {
        std::scoped_lock lock(mutex_);
        error_ = std::current_exception(); closing_ = true;
        stats_.dropped += queue_.size(); queue_.clear();
    }
}
}
