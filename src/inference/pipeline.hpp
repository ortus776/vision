#pragma once
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include "core/frame.hpp"
#include "inference/model.hpp"

namespace pubg_vision::inference {
struct FramePacket { std::uint64_t frame_id{}; core::Frame frame; };
struct DetectionResult {
    std::uint64_t frame_id{}, generation{};
    core::Rect roi;
    std::int64_t captured_ms{}, completed_ms{};
    std::vector<Detection> detections;
};
struct PipelineSettings {
    std::int32_t fps{10}, ttl_ms{500};
    PostprocessSettings postprocess;
    void validate() const;
};
struct PipelineStats { std::uint64_t submitted{}, replaced{}, processed{}, stale_discarded{}; };
using Clock = std::function<std::int64_t()>; // Same monotonic origin as Frame::captured_ms.
[[nodiscard]] DetectionResult process(const FramePacket& packet, Model& model,
    const PostprocessSettings& settings, PreparedInput& scratch, const Clock& clock);
[[nodiscard]] bool visible(const DetectionResult& result, std::uint64_t generation,
    core::Rect roi, std::int64_t now_ms, std::int32_t ttl_ms) noexcept;

// One replaceable pending frame and one result. No FIFO of obsolete images.
class Pipeline {
public:
    Pipeline(std::unique_ptr<Model> model, PipelineSettings settings, Clock clock);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    bool submit(FramePacket packet);
    void invalidate(std::uint64_t generation, bool active);
    [[nodiscard]] std::optional<DetectionResult> latest() const;
    [[nodiscard]] PipelineStats stats() const;
    void check_error() const;
    void stop(); // Owner thread; joins a running model call before returning.
private:
    void run() noexcept;
    std::unique_ptr<Model> model_;
    PipelineSettings settings_;
    Clock clock_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<FramePacket> pending_;
    std::optional<DetectionResult> result_;
    PipelineStats stats_;
    std::uint64_t generation_{1};
    std::uint64_t revision_{}; // Also invalidates a running call across pause/resume at the same geometry.
    bool active_{true}, stopping_{};
    std::exception_ptr error_;
    std::thread worker_;
};
} // namespace pubg_vision::inference
