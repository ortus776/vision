#include "inference/mock_model.hpp"
#include "inference/pipeline.hpp"
#include "postprocess/postprocessor.hpp"
#include "preprocess/preprocessor.hpp"
#include "render/raster.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace pubg_vision;
using namespace std::chrono_literals;
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
bool near(float a, float b) { return std::abs(a - b) < 0.00001F; }
template<class F> void invalid(F function, const char* message) {
    try { function(); expect(false, message); } catch (const std::invalid_argument&) {} catch (...) { expect(false, message); }
}
template<class F> bool wait(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}
inference::FramePacket packet(std::uint64_t id, std::uint64_t generation = 1, std::int64_t at = 0) {
    core::Frame frame;
    frame.size = frame.source_size = {4, 4}; frame.roi = {-100, 200, 4, 4};
    frame.generation = generation; frame.captured_ms = at; frame.pixels.assign(4U * 4U * 4U, 255);
    return {id, std::move(frame)};
}
void preprocessing() {
    core::Frame frame;
    frame.size = {2, 1}; frame.pixels = {10, 20, 255, 255, 200, 40, 0, 255};
    inference::ModelSpec spec; spec.input_size = {4, 4};
    inference::PreparedInput input;
    preprocess::prepare(frame, spec, input);
    expect(input.shape == std::array<std::int64_t, 4>{1,3,4,4}, "tensor shape is NCHW");
    expect(input.transform.resized_size.width == 4 && input.transform.resized_size.height == 2 &&
        input.transform.padding_top == 1, "letterbox centers a wide source");
    expect(near(input.tensor[0], 114.F / 255.F), "padding value is normalized");
    expect(near(input.tensor[4], 1.F) && near(input.tensor[5], .75F) &&
        near(input.tensor[6], .25F) && near(input.tensor[7], 0.F), "half-pixel bilinear red channel has known values");
    expect(near(input.tensor[16 + 4], 20.F / 255.F) && near(input.tensor[32 + 4], 10.F / 255.F),
        "BGRA is reordered to RGB planes");
    const std::vector<inference::Candidate> full_wide{{{0,1,4,3}, 1, 0}};
    const auto wide = postprocess::decode(full_wide, input.transform, spec, {});
    expect(wide.size() == 1 && near(wide[0].box.left, 0) && near(wide[0].box.top, 0) &&
        near(wide[0].box.right, 2) && near(wide[0].box.bottom, 1), "inverse transform removes vertical letterbox padding");
    const std::vector<inference::Candidate> padding_only{{{0,0,4,.5F}, 1, 0}};
    expect(postprocess::decode(padding_only, input.transform, spec, {}).empty(), "boxes inside padding cannot become visible detections");
    const auto capacity = input.tensor.capacity();
    preprocess::prepare(frame, spec, input);
    expect(input.tensor.capacity() == capacity, "same-shape preprocessing reuses tensor allocation");
    frame.pixels.pop_back();
    invalid([&] { preprocess::prepare(frame, spec, input); }, "truncated frame is rejected");
    frame.size = {3, 2}; frame.pixels.assign(24, 255);
    preprocess::prepare(frame, spec, input);
    expect(near(input.transform.scale_x, 4.F / 3.F) && near(input.transform.scale_y, 1.5F),
        "rounded letterbox stores separate actual scales");
    const std::vector<inference::Candidate> boxes{{{4.F/3.F, 1.5F, 4.F, 3.F}, 1.F, 0}};
    const auto result = postprocess::decode(boxes, input.transform, spec, {});
    expect(result.size() == 1 && near(result[0].box.left, 1) && near(result[0].box.top, 1) &&
        near(result[0].box.right, 3) && near(result[0].box.bottom, 2), "inverse mapping uses actual scales");
    spec.input_size.width = 0;
    invalid([&] { preprocess::prepare(frame, spec, input); }, "invalid model input rejected");
}
void postprocessing() {
    inference::ModelSpec spec; spec.input_size = {100,100}; spec.classes = {"a", "b"};
    inference::Transform transform{{100,100}, {100,100}, {100,100}, 0, 0, 1.F, 1.F};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<inference::Candidate> candidates{
        {{10,10,50,50}, .9F, 0}, {{10,10,50,50}, .8F, 0}, {{10,10,50,50}, .7F, 1},
        {{60,60,90,90}, .1F, 0}, {{60,60,90,90}, nan, 0}, {{nan,0,20,20}, .9F, 0},
        {{-20,-20,-5,-5}, .9F, 0}, {{-5,-5,5,5}, .6F, 0}, {{1,1,1,5}, .9F, 0},
        {{60,60,90,90}, 2.F, 0}, {{60,60,90,90}, .9F, 2}};
    auto detections = postprocess::decode(candidates, transform, spec, {});
    expect(detections.size() == 3, "NMS filters duplicates within the same class and invalid proposals");
    expect(near(detections.back().box.left, 0) && near(detections.back().box.top, 0), "crossing boxes clip to ROI");
    spec.output_has_nms = true;
    expect(postprocess::decode(candidates, transform, spec, {}).size() == 4, "embedded NMS is not applied twice");
    inference::PostprocessSettings settings; settings.max_detections = 1;
    expect(postprocess::decode(candidates, transform, spec, settings).size() == 1, "output limit is enforced");
    transform.scale_y = 0;
    invalid([&] { (void)postprocess::decode(candidates, transform, spec, {}); }, "invalid transform rejected");
    transform.scale_y = 1;
    spec.output_has_nms = false;
    const std::vector<inference::Candidate> edge{{{-100,0,100,100}, .9F, 0}, {{0,0,100,100}, .8F, 0}};
    settings.max_detections = 100; settings.nms_iou = .6F;
    const auto before_clip = postprocess::decode(edge, transform, spec, settings);
    expect(before_clip.size() == 2 && near(before_clip[0].box.left, 0), "NMS uses original boxes before clipping at ROI edge");
    settings.nms_iou = .5F;
    expect(postprocess::decode(edge, transform, spec, settings).size() == 2, "IoU equal to the threshold is retained");
    settings.nms_iou = .49F;
    expect(postprocess::decode(edge, transform, spec, settings).size() == 1, "original overlapping boxes suppress above IoU threshold");
    spec.output_has_nms = true;
    expect(postprocess::decode(edge, transform, spec, settings).size() == 2, "embedded/NMS-free output bypasses external NMS");
}
void rendering_and_mock() {
    const std::vector<inference::Detection> boxes{{{2,2,8,8}, 1, 0}};
    const auto raster = render::rasterize({10,10}, boxes);
    const auto alpha = [&](std::int32_t x, std::int32_t y) { return raster[(static_cast<std::size_t>(y)*10+x)*4+3]; };
    expect(alpha(2,2) == 255 && alpha(5,5) == 0 && alpha(0,0) == 0, "box stroke is opaque and interior/background transparent");
    const auto point = render::rasterize({10,10}, boxes, render::Style::point);
    expect(point[(5U*10+5)*4+3] == 255 && point[3] == 0, "point mode marks only the center neighborhood");
    core::Frame bad; bad.size = {1,1}; bad.pixels.resize(3);
    invalid([&] { render::composite(bad, std::vector<std::uint8_t>(3)); }, "odd raster buffer cannot overrun composite");
    inference::ModelSpec spec; spec.input_size = {4,4};
    inference::MockModel first(spec, 42), second(spec, 42);
    inference::PreparedInput input;
    const auto frame = packet(1);
    preprocess::prepare(frame.frame, spec, input);
    const auto a = first.run(input), b = second.run(input), c = first.run(input);
    expect(a.size() == 1 && near(a[0].box.left, b[0].box.left), "mock seed makes tests reproducible");
    expect(!near(a[0].box.left, c[0].box.left), "mock produces a new random box for each processed frame");
    const auto result = inference::process(frame, first, {}, input, [] { return 1; });
    expect(result.frame_id == 1 && result.roi.left == -100 && result.generation == 1, "frame identity and screen origin survive the pipeline");
    expect(inference::visible(result, 1, result.roi, 500, 500), "TTL includes its boundary");
    expect(!inference::visible(result, 1, result.roi, 501, 500), "TTL hides stale results using capture time");
    expect(!inference::visible(result, 2, result.roi, 1, 500), "wrong generation cannot be rendered");
    auto moved = result.roi; ++moved.left;
    expect(!inference::visible(result, 1, moved, 1, 500), "wrong ROI cannot be rendered after a move");
}
struct Gate {
    std::mutex mutex;
    std::condition_variable wake;
    std::size_t calls{};
    bool release{};
};
class GatedModel : public inference::Model {
public:
    explicit GatedModel(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) { spec_.input_size = {4,4}; }
    const inference::ModelSpec& spec() const noexcept override { return spec_; }
    std::vector<inference::Candidate> run(const inference::PreparedInput&, std::stop_token = {}) override {
        std::unique_lock lock(gate_->mutex);
        if (++gate_->calls == 1) gate_->wake.wait(lock, [&] { return gate_->release; });
        return {{{1,1,3,3}, 1, 0}};
    }
private:
    std::shared_ptr<Gate> gate_;
    inference::ModelSpec spec_;
};
void release(const std::shared_ptr<Gate>& gate) { std::lock_guard lock(gate->mutex); gate->release = true; gate->wake.notify_all(); }
bool entered(const std::shared_ptr<Gate>& gate) { std::lock_guard lock(gate->mutex); return gate->calls > 0; }
struct GateRelease {
    std::shared_ptr<Gate> gate;
    ~GateRelease() { release(gate); }
};
void asynchronous_pipeline() {
    inference::PipelineSettings settings; settings.fps = 120;
    {
        auto gate = std::make_shared<Gate>();
        inference::Pipeline pipeline(std::make_unique<GatedModel>(gate), settings, [] { return 0; });
        GateRelease cleanup{gate}; // Unblocks the model before pipeline destruction during unwinding.
        pipeline.submit(packet(1)); expect(wait([&] { return entered(gate); }), "inference call started");
        for (std::uint64_t i = 2; i <= 100; ++i) pipeline.submit(packet(i));
        expect(pipeline.stats().replaced == 98, "overload keeps one latest pending frame");
        release(gate);
        expect(wait([&] { auto r = pipeline.latest(); return r && r->frame_id == 100; }), "newest frame processed after slow call");
        expect(pipeline.stats().processed == 2, "obsolete pending frames are not processed");
        pipeline.stop(); expect(!pipeline.submit(packet(101)), "stop rejects further submissions");
    }
    for (bool same_generation : {false, true}) {
        auto gate = std::make_shared<Gate>();
        inference::Pipeline pipeline(std::make_unique<GatedModel>(gate), settings, [] { return 0; });
        GateRelease cleanup{gate};
        pipeline.submit(packet(1)); expect(wait([&] { return entered(gate); }), "blocked model started before invalidation");
        const std::uint64_t generation = same_generation ? 1 : 2;
        pipeline.invalidate(generation, false); pipeline.invalidate(generation, true);
        pipeline.submit(packet(2, generation)); release(gate);
        expect(wait([&] { auto r = pipeline.latest(); return r && r->frame_id == 2; }), "new valid frame processed after invalidation");
        expect(pipeline.stats().stale_discarded == 1, "running result is discarded across geometry change or same-generation pause/resume");
    }
    {
        std::atomic<std::int64_t> now{0};
        auto gate = std::make_shared<Gate>();
        inference::Pipeline pipeline(std::make_unique<GatedModel>(gate), settings, [&] { return now.load(); });
        GateRelease cleanup{gate};
        pipeline.submit(packet(1)); expect(wait([&] { return entered(gate); }), "TTL test started");
        now = 1000; release(gate);
        expect(wait([&] { return pipeline.stats().processed == 1; }), "slow result finished");
        expect(!pipeline.latest() && pipeline.stats().stale_discarded == 1, "result expiring during model execution is discarded");
        pipeline.submit(packet(2, 1, 1000));
        expect(wait([&] { return pipeline.latest().has_value(); }), "fresh result remains available after an expiry");
    }
}
class FailingModel : public inference::Model {
public:
    const inference::ModelSpec& spec() const noexcept override { return spec_; }
    std::vector<inference::Candidate> run(const inference::PreparedInput&, std::stop_token = {}) override { throw std::runtime_error("model test failure"); }
private:
    inference::ModelSpec spec_{{4,4}};
};
void failure_propagation() {
    inference::Pipeline pipeline(std::make_unique<FailingModel>(), {}, [] { return 0; });
    pipeline.submit(packet(1));
    expect(wait([&] {
        try { pipeline.check_error(); return false; }
        catch (const std::runtime_error& e) { return std::string(e.what()) == "model test failure"; }
    }), "model exception reaches owner thread");
    expect(!pipeline.latest() && !pipeline.submit(packet(2)), "failed worker publishes no results and rejects frames");
}
struct CancellationProbe { std::atomic<int> entered{}, cancelled{}; };
class CancellableModel : public inference::Model {
public:
    explicit CancellableModel(std::shared_ptr<CancellationProbe> probe) : probe_(std::move(probe)) { spec_.input_size = {4,4}; }
    const inference::ModelSpec& spec() const noexcept override { return spec_; }
    std::vector<inference::Candidate> run(const inference::PreparedInput&, std::stop_token stop) override {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        ++probe_->entered;
        wake.wait(lock, stop, [] { return false; });
        ++probe_->cancelled;
        inference::throw_if_cancelled(stop);
        return {};
    }
private:
    std::shared_ptr<CancellationProbe> probe_;
    inference::ModelSpec spec_;
};
void cooperative_cancellation() {
    auto probe = std::make_shared<CancellationProbe>();
    inference::PipelineSettings settings; settings.fps = 120;
    inference::Pipeline pipeline(std::make_unique<CancellableModel>(probe), settings, [] { return 0; });
    pipeline.submit(packet(1));
    expect(wait([&] { return probe->entered == 1; }), "cancellable model entered Run");
    pipeline.invalidate(1, false);
    expect(wait([&] { return probe->cancelled == 1 && pipeline.stats().stale_discarded == 1; }), "pause cancels Run without a model failure");
    pipeline.check_error();
    pipeline.invalidate(1, true); pipeline.submit(packet(2));
    expect(wait([&] { return probe->entered == 2; }), "new Run uses a fresh token after resume");
    pipeline.stop();
    expect(probe->cancelled == 2 && !pipeline.latest(), "stop cancels blocked Run and joins the worker");
    pipeline.check_error();
}
}
int main() {
    try { preprocessing(); postprocessing(); rendering_and_mock(); asynchronous_pipeline(); failure_propagation(); cooperative_cancellation(); }
    catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    if (failures) return 1;
    std::cout << "Inference, coordinates, rendering and latest-frame pipeline tests passed\n";
    return 0;
}
