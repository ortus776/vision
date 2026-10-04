#include "dataset/dataset_writer.hpp"
#include "core/json.hpp"
#include "core/filesystem.hpp"
#include "core/performance_clock.hpp"
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace pubg_vision::dataset {
DatasetWriter::DatasetWriter(const std::filesystem::path& root, std::size_t capacity,
                             const std::string& metadata, PngEncoder encoder, bool trace_frames)
    : capacity_(capacity), encoder_(std::move(encoder)) {
    if (capacity == 0 || capacity > 4096 || !encoder_)
        throw std::invalid_argument("invalid writer settings");
    const auto stamp = core::utc_milliseconds();
    // A random 128-bit component also separates sessions in different output roots
    // or after old session directories have been moved away for annotation.
    std::random_device random;
    std::uniform_int_distribution<std::uint32_t> word;
    std::ostringstream prefix;
    prefix << "session_" << stamp << '_' << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) prefix << std::setw(8) << word(random);
    directory_ = core::unique_directory(core::native_output_path(root), prefix.str());
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
    if (trace_frames) trace_ = std::make_unique<TraceWriter>(directory_ / "collector_trace.jsonl");
    worker_ = std::thread([this] { run(); });
}
DatasetWriter::~DatasetWriter() {
    try { finish(); } catch (...) { /* Explicit finish is responsible for reporting errors. */ }
}
void DatasetWriter::event_unlocked(const std::string& json) {
    const auto begin = std::chrono::steady_clock::now();
    events_ << json << '\n';
    events_.flush();
    if (!events_) throw std::runtime_error("could not append events.jsonl (disk full or unavailable)");
    stats_.journal_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}
void DatasetWriter::event(const std::string& json) {
    std::scoped_lock lock(mutex_);
    if (error_) std::rethrow_exception(error_);
    try { event_unlocked(json); }
    catch (...) { error_ = std::current_exception(); closing_ = true; wake_.notify_all(); throw; }
}
void DatasetWriter::check_error() const {
    if (trace_) trace_->check_error();
    std::scoped_lock lock(mutex_);
    if (error_) std::rethrow_exception(error_);
}
void DatasetWriter::trace_event(std::string json) { if (trace_) trace_->try_event(std::move(json)); }
TraceStats DatasetWriter::trace_stats() const { return trace_ ? trace_->stats() : TraceStats{}; }
WriterStats DatasetWriter::stats() const {
    std::scoped_lock lock(mutex_);
    auto result = stats_;
    result.pending = queue_.size();
    return result;
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
    if (trace_) trace_->finish();
    check_error();
}
double DatasetWriter::write(const Job& job) {
    std::ostringstream name;
    name << session_id_ << "_frame_" << std::setw(8) << std::setfill('0') << job.id << ".png";
    const std::string relative = "images/" + name.str();
    auto final_path = directory_ / relative;
    final_path.make_preferred(); // Extended Win32 paths do not normalize '/' separators.
    auto temp = final_path;
    temp += ".tmp";
    const auto encode_start = std::chrono::steady_clock::now();
    const auto encode_qpc = core::qpc_ticks();
    encoder_(temp, job.frame.size, job.frame.pixels);
    const auto encode_end_qpc = core::qpc_ticks();
    const auto encode_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - encode_start).count();
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
    if (trace_) {
        const auto publish_end_qpc = core::qpc_ticks();
        const auto span = [&](const char* stage, std::int64_t begin, std::int64_t end) {
            trace_event("{\"type\":\"span\",\"stage\":" + core::json_string(stage) +
                ",\"start_qpc\":" + std::to_string(begin) + ",\"end_qpc\":" + std::to_string(end) +
                ",\"frame_id\":" + std::to_string(job.id) + ",\"requests\":" +
                collection::requests_json(job.requests) + '}');
        };
        if (job.frame.capture_end_qpc) span("writer_queue", *job.frame.capture_end_qpc, encode_qpc);
        span("png", encode_qpc, encode_end_qpc);
        span("publish", encode_end_qpc, publish_end_qpc);
    }
    return encode_ms;
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
            const auto begin = std::chrono::steady_clock::now();
            const auto encode_ms = write(job);
            const auto write_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();
            std::scoped_lock lock(mutex_);
            ++stats_.saved;
            stats_.encode_ms += encode_ms;
            if (encode_ms > stats_.max_encode_ms) stats_.max_encode_ms = encode_ms;
            stats_.write_ms += write_ms;
            if (write_ms > stats_.max_write_ms) stats_.max_write_ms = write_ms;
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
