#include "dataset/dataset_writer.hpp"
#include "core/json.hpp"
#include "core/filesystem.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pubg_vision::dataset {
DatasetWriter::DatasetWriter(const std::filesystem::path& root, std::size_t capacity,
                             const std::string& metadata, PngEncoder encoder)
    : capacity_(capacity), encoder_(std::move(encoder)) {
    if (capacity == 0 || capacity > 4096 || !encoder_)
        throw std::invalid_argument("invalid writer settings");
    const auto stamp = core::utc_milliseconds();
    directory_ = core::unique_directory(root, "session_" + std::to_string(stamp));
    session_id_ = directory_.filename().string();
    std::filesystem::create_directory(directory_ / "images");
    std::ofstream session(directory_ / "session.json", std::ios::binary);
    session << "{\"schema_version\":2,\"session_id\":" << core::json_string(session_id_)
            << ",\"started_utc_ms\":" << stamp << ",\"metadata\":" << metadata << "}\n";
    session.close();
    if (!session) throw std::runtime_error("could not write session.json");
    frames_.open(directory_ / "frames.jsonl", std::ios::binary);
    events_.open(directory_ / "events.jsonl", std::ios::binary);
    if (!frames_ || !events_) throw std::runtime_error("could not open dataset manifests");
    worker_ = std::thread([this] { run(); });
}
DatasetWriter::~DatasetWriter() {
    try { finish(); } catch (...) { /* Explicit finish is responsible for reporting errors. */ }
}
void DatasetWriter::event_unlocked(const std::string& json) {
    events_ << json << '\n';
    events_.flush();
    if (!events_) throw std::runtime_error("could not append events.jsonl (disk full or unavailable)");
}
void DatasetWriter::event(const std::string& json) {
    std::scoped_lock lock(mutex_);
    if (error_) std::rethrow_exception(error_);
    try { event_unlocked(json); }
    catch (...) { error_ = std::current_exception(); closing_ = true; wake_.notify_all(); throw; }
}
void DatasetWriter::check_error() const {
    std::scoped_lock lock(mutex_);
    if (error_) std::rethrow_exception(error_);
}
WriterStats DatasetWriter::stats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}
bool DatasetWriter::try_enqueue(core::Frame frame, std::vector<collection::Request> requests) {
    std::scoped_lock lock(mutex_);
    if (error_) std::rethrow_exception(error_);
    if (closing_) throw std::logic_error("dataset writer is closed");
    if (requests.empty()) throw std::invalid_argument("frame must reference at least one request");
    if (queue_.size() >= capacity_) {
        ++stats_.rejected;
        event_unlocked("{\"type\":\"skipped\",\"reason\":\"writer_queue_full\",\"requests\":" +
                       collection::requests_json(requests) + '}');
        return false;
    }
    const auto id = next_id_++;
    event_unlocked("{\"type\":\"writer_accepted\",\"frame_id\":" + std::to_string(id) +
                   ",\"requests\":" + collection::requests_json(requests) + '}');
    queue_.push_back({std::move(frame), std::move(requests), id});
    if (queue_.size() > stats_.high_water) stats_.high_water = queue_.size();
    wake_.notify_one();
    return true;
}
void DatasetWriter::finish() {
    { std::scoped_lock lock(mutex_); closing_ = true; }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    check_error();
}
void DatasetWriter::write(const Job& job) {
    std::ostringstream name;
    name << "frame_" << std::setw(8) << std::setfill('0') << job.id << ".png";
    const std::string relative = "images/" + name.str();
    const auto final_path = directory_ / relative;
    auto temp = final_path;
    temp += ".tmp";
    encoder_(temp, job.frame.size, job.frame.pixels);
    std::filesystem::rename(temp, final_path);
    const auto& f = job.frame;
    std::ostringstream line;
    line << "{\"session_id\":" << core::json_string(session_id_) << ",\"frame_id\":" << job.id
         << ",\"path\":" << core::json_string(relative) << ",\"captured_ms\":" << f.captured_ms
         << ",\"captured_utc_ms\":" << f.captured_utc_ms << ",\"source_qpc\":";
    if (f.source_qpc) line << *f.source_qpc; else line << "null";
    line << ",\"generation\":" << f.generation << ",\"roi\":{\"left\":" << f.roi.left
         << ",\"top\":" << f.roi.top << ",\"width\":" << f.size.width
         << ",\"height\":" << f.size.height << "},\"source_size\":{\"width\":"
         << f.source_size.width << ",\"height\":" << f.source_size.height
         << "},\"requests\":" << collection::requests_json(job.requests) << '}';
    frames_ << line.str() << '\n';
    frames_.flush();
    if (!frames_) throw std::runtime_error("could not append frames.jsonl (PNG may be orphaned)");
}
void DatasetWriter::run() noexcept {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return closing_ || !queue_.empty(); });
            if (error_) {
                stats_.failed += queue_.size();
                queue_.clear();
                return;
            }
            if (queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            write(job);
            std::scoped_lock lock(mutex_);
            ++stats_.saved;
        } catch (...) {
            std::scoped_lock lock(mutex_);
            error_ = std::current_exception();
            closing_ = true;
            ++stats_.failed;
            // Best effort diagnostics: the failed disk itself may prevent journaling.
            try {
                event_unlocked("{\"type\":\"write_failed\",\"frame_id\":" + std::to_string(job.id) +
                               ",\"requests\":" + collection::requests_json(job.requests) + '}');
                for (const auto& pending : queue_)
                    event_unlocked("{\"type\":\"cancelled\",\"reason\":\"writer_error\",\"requests\":" +
                                   collection::requests_json(pending.requests) + '}');
            } catch (...) {}
            stats_.failed += queue_.size();
            queue_.clear();
            return;
        }
    }
}
} // namespace pubg_vision::dataset
