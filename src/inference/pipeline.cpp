#include "inference/pipeline.hpp"
#include "preprocess/preprocessor.hpp"
#include "postprocess/postprocessor.hpp"
#include <chrono>
#include <stdexcept>

namespace pubg_vision::inference {
void PipelineSettings::validate() const {
    postprocess.validate();
    if (fps < 1 || fps > 120 || ttl_ms < 1 || ttl_ms > 60000)
        throw std::invalid_argument("inference FPS must be 1..120 and TTL 1..60000 ms");
}
DetectionResult process(const FramePacket& packet, Model& model,
    const PostprocessSettings& settings, PreparedInput& scratch, const Clock& clock) {
    if (!packet.frame.roi.valid() || packet.frame.roi.width != packet.frame.size.width ||
        packet.frame.roi.height != packet.frame.size.height)
        throw std::invalid_argument("frame dimensions must agree with its ROI");
    preprocess::prepare(packet.frame, model.spec(), scratch);
    const auto candidates = model.run(scratch);
    auto detections = postprocess::decode(candidates, scratch.transform, model.spec(), settings);
    return {packet.frame_id, packet.frame.generation, packet.frame.roi,
        packet.frame.captured_ms, clock(), std::move(detections)};
}
bool visible(const DetectionResult& r, std::uint64_t generation, core::Rect roi,
    std::int64_t now_ms, std::int32_t ttl_ms) noexcept {
    return ttl_ms > 0 && r.generation == generation && r.roi.left == roi.left && r.roi.top == roi.top &&
        r.roi.width == roi.width && r.roi.height == roi.height && now_ms >= r.captured_ms &&
        static_cast<std::uint64_t>(now_ms) - static_cast<std::uint64_t>(r.captured_ms) <= static_cast<std::uint64_t>(ttl_ms);
}
Pipeline::Pipeline(std::unique_ptr<Model> model, PipelineSettings settings, Clock clock)
    : model_(std::move(model)), settings_(settings), clock_(std::move(clock)) {
    if (!model_ || !clock_) throw std::invalid_argument("pipeline requires model and clock");
    model_->spec().validate(); settings_.validate();
    worker_ = std::thread([this] { run(); });
}
Pipeline::~Pipeline() { stop(); }
bool Pipeline::submit(FramePacket packet) {
    std::lock_guard lock(mutex_);
    if (stopping_ || error_) return false;
    if (!active_ || packet.frame.generation != generation_) { ++stats_.stale_discarded; return false; }
    ++stats_.submitted;
    if (pending_) ++stats_.replaced;
    pending_ = std::move(packet);
    wake_.notify_one();
    return true;
}
void Pipeline::invalidate(std::uint64_t generation, bool active) {
    std::lock_guard lock(mutex_);
    if (generation_ == generation && active_ == active) return;
    generation_ = generation; active_ = active; ++revision_;
    if (pending_) ++stats_.stale_discarded;
    pending_.reset(); result_.reset();
    wake_.notify_one();
}
std::optional<DetectionResult> Pipeline::latest() const { std::lock_guard lock(mutex_); return result_; }
PipelineStats Pipeline::stats() const { std::lock_guard lock(mutex_); return stats_; }
void Pipeline::check_error() const {
    std::exception_ptr error;
    { std::lock_guard lock(mutex_); error = error_; }
    if (error) std::rethrow_exception(error);
}
void Pipeline::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; pending_.reset(); result_.reset(); }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}
void Pipeline::run() noexcept {
    try {
        PreparedInput scratch;
        auto next = std::chrono::steady_clock::now();
        const auto period = std::chrono::microseconds(1000000 / settings_.fps);
        for (;;) {
            FramePacket packet;
            std::uint64_t revision{};
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
                if (stopping_) return;
                if (std::chrono::steady_clock::now() < next) {
                    wake_.wait_until(lock, next, [&] { return stopping_ || !pending_; });
                    if (stopping_) return;
                    if (!pending_) continue;
                }
                packet = std::move(*pending_); pending_.reset();
                revision = revision_;
            }
            const auto began = std::chrono::steady_clock::now();
            next = began + period;
            if (!visible({packet.frame_id, packet.frame.generation, packet.frame.roi,
                          packet.frame.captured_ms, 0, {}}, packet.frame.generation,
                          packet.frame.roi, clock_(), settings_.ttl_ms)) {
                std::lock_guard lock(mutex_); ++stats_.stale_discarded; continue;
            }
            auto result = process(packet, *model_, settings_.postprocess, scratch, clock_);
            std::lock_guard lock(mutex_);
            ++stats_.processed;
            if (!stopping_ && active_ && result.generation == generation_ && revision == revision_ &&
                visible(result, generation_, result.roi, clock_(), settings_.ttl_ms)) result_ = std::move(result);
            else ++stats_.stale_discarded;
        }
    } catch (...) {
        std::lock_guard lock(mutex_);
        error_ = std::current_exception(); stopping_ = true; pending_.reset(); result_.reset();
    }
}
} // namespace pubg_vision::inference
