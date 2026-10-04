#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "capture/desktop_capture.hpp"
#include "core/geometry.hpp"
#include "core/filesystem.hpp"
#include "core/performance_clock.hpp"
#include "platform/windows/window_selection.hpp"

namespace pubg_vision::capture {
namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::string hresult_text(HRESULT result);

[[nodiscard]] std::string utf8_from_wide(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                              static_cast<int>(text.size()), nullptr, 0,
                                              nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("could not convert a window title to UTF-8");
    }
    std::string converted(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), converted.data(), required,
                            nullptr, nullptr) != required) {
        throw std::runtime_error("could not convert a window title to UTF-8");
    }
    return converted;
}

class ComApartment {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result)) {
            throw std::runtime_error("CoInitializeEx failed (" + hresult_text(result) + ")");
        }
        initialized_ = true;
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{false};
};

class AcquiredFrame {
public:
    explicit AcquiredFrame(IDXGIOutputDuplication* duplication) : duplication_(duplication) {}
    AcquiredFrame(const AcquiredFrame&) = delete;
    AcquiredFrame& operator=(const AcquiredFrame&) = delete;
    ~AcquiredFrame() {
        if (acquired_) {
            (void)duplication_->ReleaseFrame();
        }
    }

    void mark_acquired() noexcept { acquired_ = true; }
    void retain() noexcept { acquired_ = false; }
    HRESULT release() noexcept {
        if (!acquired_) {
            return S_OK;
        }
        acquired_ = false;
        return duplication_->ReleaseFrame();
    }

private:
    IDXGIOutputDuplication* duplication_;
    bool acquired_{false};
};

[[nodiscard]] std::string hresult_text(HRESULT result) {
    std::ostringstream text;
    text << "HRESULT 0x" << std::hex << std::uppercase <<
        static_cast<std::uint32_t>(result);
    return text.str();
}

void check_hr(HRESULT result, const char* operation) {
    if (result == DXGI_ERROR_ACCESS_LOST) {
        throw std::runtime_error(std::string(operation) +
                                 " lost desktop access after a display or session change; retry capture");
    }
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed (" + hresult_text(result) + ")");
    }
}

[[nodiscard]] RECT to_native_rect(core::Rect rect) {
    const auto right = rect.right();
    const auto bottom = rect.bottom();
    if (right < std::numeric_limits<LONG>::min() || right > std::numeric_limits<LONG>::max() ||
        bottom < std::numeric_limits<LONG>::min() || bottom > std::numeric_limits<LONG>::max()) {
        throw std::out_of_range("capture rectangle is outside Windows coordinate limits");
    }
    return RECT{rect.left, rect.top, static_cast<LONG>(right), static_cast<LONG>(bottom)};
}

void save_png(const std::filesystem::path& path,
              core::Size size,
              const std::vector<std::uint8_t>& pixels, bool no_filter = false) {
    const auto stride64 = static_cast<std::uint64_t>(size.width) * 4U;
    const auto buffer_size64 = stride64 * static_cast<std::uint64_t>(size.height);
    if (stride64 > std::numeric_limits<UINT>::max() ||
        buffer_size64 > std::numeric_limits<UINT>::max() ||
        buffer_size64 != pixels.size()) {
        throw std::runtime_error("image buffer size cannot be represented by the WIC encoder");
    }

    ComPtr<IWICImagingFactory> factory;
    check_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(factory.GetAddressOf())),
             "CoCreateInstance(WIC factory)");

    ComPtr<IWICStream> stream;
    check_hr(factory->CreateStream(stream.GetAddressOf()), "IWICImagingFactory::CreateStream");
    check_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
             "IWICStream::InitializeFromFilename");

    ComPtr<IWICBitmapEncoder> encoder;
    check_hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()),
             "IWICImagingFactory::CreateEncoder(PNG)");
    check_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
             "IWICBitmapEncoder::Initialize");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    check_hr(encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf()),
             "IWICBitmapEncoder::CreateNewFrame");
    if (no_filter) {
        PROPBAG2 property{};
        property.pstrName = const_cast<wchar_t*>(L"FilterOption");
        VARIANT value{};
        value.vt = VT_UI1;
        value.bVal = WICPngFilterNone;
        check_hr(options->Write(1, &property, &value), "WIC PNG FilterOption");
    }
    check_hr(frame->Initialize(options.Get()), "IWICBitmapFrameEncode::Initialize");
    check_hr(frame->SetSize(static_cast<UINT>(size.width), static_cast<UINT>(size.height)),
             "IWICBitmapFrameEncode::SetSize");

    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    check_hr(frame->SetPixelFormat(&pixel_format), "IWICBitmapFrameEncode::SetPixelFormat");
    if (!IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA)) {
        throw std::runtime_error("PNG encoder does not support the requested BGRA pixel format");
    }

    check_hr(frame->WritePixels(static_cast<UINT>(size.height), static_cast<UINT>(stride64),
                                static_cast<UINT>(buffer_size64),
                                const_cast<BYTE*>(pixels.data())),
             "IWICBitmapFrameEncode::WritePixels");
    check_hr(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    check_hr(encoder->Commit(), "IWICBitmapEncoder::Commit");
}

struct SelectedOutput {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC description{};
};

[[nodiscard]] SelectedOutput find_output(IDXGIFactory1* factory, HMONITOR target_monitor) {
    SelectedOutput selected;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapter_result = factory->EnumAdapters1(adapter_index, adapter.GetAddressOf());
        if (adapter_result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        check_hr(adapter_result, "IDXGIFactory1::EnumAdapters1");

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_result = adapter->EnumOutputs(output_index, output.GetAddressOf());
            if (output_result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            check_hr(output_result, "IDXGIAdapter::EnumOutputs");

            DXGI_OUTPUT_DESC description{};
            check_hr(output->GetDesc(&description), "IDXGIOutput::GetDesc");
            if (description.Monitor != target_monitor) {
                continue;
            }

            check_hr(output.As(&selected.output), "QueryInterface(IDXGIOutput1)");
            selected.adapter = std::move(adapter);
            selected.description = description;
            return selected;
        }
    }

    throw std::runtime_error("could not find the DXGI output associated with the selected window");
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> copy_desktop_crop(
    IDXGIOutputDuplication* duplication,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    core::Rect crop_on_output,
    core::Size roi,
    ComPtr<ID3D11Texture2D>& staging_texture,
    std::uint32_t timeout_ms,
    std::int64_t& source_qpc,
    std::int64_t minimum_present_qpc,
    bool hold_frame, bool& held, CaptureStats& stats, CaptureTimings& timings) {
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point begin) {
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    };
    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = static_cast<UINT>(roi.width);
    staging_desc.Height = static_cast<UINT>(roi.height);
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (!staging_texture) {
        check_hr(device->CreateTexture2D(&staging_desc, nullptr, staging_texture.GetAddressOf()),
                 "ID3D11Device::CreateTexture2D(staging)");
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    ComPtr<IDXGIResource> desktop_resource;
    AcquiredFrame frame(duplication);
    if (held) {
        held = false;
        check_hr(duplication->ReleaseFrame(), "IDXGIOutputDuplication::ReleaseFrame(before acquire)");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return std::nullopt;
        }
        const auto acquire_start = Clock::now();
        if (!timings.acquire_start) timings.acquire_start = core::qpc_ticks();
        const HRESULT result = duplication->AcquireNextFrame(
            static_cast<UINT>(remaining.count()), &frame_info,
            desktop_resource.GetAddressOf());
        timings.acquire_end = core::qpc_ticks();
        stats.acquire_ms += elapsed(acquire_start);
        if (result == DXGI_ERROR_WAIT_TIMEOUT) {
            return std::nullopt;
        }
        check_hr(result, "IDXGIOutputDuplication::AcquireNextFrame");
        frame.mark_acquired();
        ++stats.acquired;
        if (frame_info.LastPresentTime.QuadPart != 0 &&
            frame_info.LastPresentTime.QuadPart >= minimum_present_qpc) {
            break;
        }
        // Reject pointer-only updates and images predating the current geometry.
        ++stats.rejected_updates;
        check_hr(frame.release(), "IDXGIOutputDuplication::ReleaseFrame");
        desktop_resource.Reset();
    }

    ComPtr<ID3D11Texture2D> desktop_texture;
    check_hr(desktop_resource.As(&desktop_texture), "QueryInterface(ID3D11Texture2D)");

    D3D11_TEXTURE2D_DESC desktop_desc{};
    desktop_texture->GetDesc(&desktop_desc);
    if (desktop_desc.Format != staging_desc.Format) {
        throw std::runtime_error("the acquired desktop surface has an unsupported pixel format");
    }
    if (crop_on_output.left < 0 || crop_on_output.top < 0 ||
        crop_on_output.right() > desktop_desc.Width ||
        crop_on_output.bottom() > desktop_desc.Height) {
        throw std::runtime_error("the selected ROI is outside the acquired desktop surface");
    }

    const D3D11_BOX source_box{
        static_cast<UINT>(crop_on_output.left),
        static_cast<UINT>(crop_on_output.top),
        0,
        static_cast<UINT>(crop_on_output.right()),
        static_cast<UINT>(crop_on_output.bottom()),
        1,
    };
    context->CopySubresourceRegion(staging_texture.Get(), 0, 0, 0, 0,
                                   desktop_texture.Get(), 0, &source_box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto map_start = Clock::now();
    timings.map_start = core::qpc_ticks();
    check_hr(context->Map(staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped),
             "ID3D11DeviceContext::Map(staging)");
    timings.map_end = core::qpc_ticks();
    const auto map_ms = elapsed(map_start);
    stats.map_ms += map_ms;
    stats.max_map_ms = (std::max)(stats.max_map_ms, map_ms);
    struct UnmapGuard {
        ID3D11DeviceContext* context;
        ID3D11Resource* resource;
        ~UnmapGuard() { context->Unmap(resource, 0); }
    } unmap{context, staging_texture.Get()};

    const auto copy_start = Clock::now();
    timings.copy_start = core::qpc_ticks();
    const auto stride = static_cast<std::size_t>(roi.width) * 4U;
    if (mapped.RowPitch < stride) throw std::runtime_error("DXGI RowPitch is smaller than the ROI row");
    source_qpc = frame_info.LastPresentTime.QuadPart;
    const auto buffer_size = stride * static_cast<std::size_t>(roi.height);
    std::vector<std::uint8_t> pixels(buffer_size);
    const auto* source_bytes = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::int32_t row = 0; row < roi.height; ++row) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * stride,
                    source_bytes + static_cast<std::size_t>(row) * mapped.RowPitch,
                    stride);
    }
    // Desktop pixels are opaque. The duplication surface does not guarantee a usable
    // alpha channel, while the PNG encoder interprets zero alpha as transparency.
    for (std::size_t offset = 3; offset < pixels.size(); offset += 4) {
        pixels[offset] = 255;
    }

    stats.copy_ms += elapsed(copy_start);
    timings.copy_end = core::qpc_ticks();
    if (hold_frame) {
        // Keep ownership between sparse samples; release immediately before the next acquire.
        // The local guard still releases on every exception before this ownership transfer.
        frame.retain();
        held = true;
    } else check_hr(frame.release(), "IDXGIOutputDuplication::ReleaseFrame");
    return pixels;
}

} // namespace

void list_windows() {
    platform::windows::set_per_monitor_dpi_awareness();
    const auto windows = platform::windows::visible_windows();
    if (windows.empty()) {
        std::cout << "No visible top-level windows found.\n";
        return;
    }

    std::cout << "Visible windows (title and client-area size):\n";
    for (const auto& window : windows) {
        std::cout << "  " << utf8_from_wide(window.title) << " - "
                  << window.client_screen.width << "x" << window.client_screen.height
                  << " at (" << window.client_screen.left << ","
                  << window.client_screen.top << ")";
        MONITORINFO monitor_info{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(window.monitor, &monitor_info)) {
            std::cout << "; nearest monitor (" << monitor_info.rcMonitor.left << ","
                      << monitor_info.rcMonitor.top << ") "
                      << monitor_info.rcMonitor.right - monitor_info.rcMonitor.left
                      << "x" << monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
        }
        std::cout << '\n';
    }
}

struct DesktopCapture::Impl {
    ComApartment com;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    core::Rect absolute{}, local{}, client{};
    core::Size size{};
    HMONITOR monitor{};
    HWND handle{};
    std::uint64_t generation{};
    std::int64_t minimum_present_qpc{};
    bool hold_frame{}, held{};
    CaptureStats stats;
    CaptureTimings timings;
    explicit Impl(bool hold) : hold_frame(hold) {}
    ~Impl() { release_held(); }
    void release_held() noexcept {
        if (held && duplication) (void)duplication->ReleaseFrame();
        held = false;
    }
    bool prepare(HWND target, core::Size roi) {
        const auto window = platform::windows::inspect_window(target);
        const auto absolute_roi = core::centered_rect(window.client_screen, roi);
        const auto native_roi = to_native_rect(absolute_roi);
        const POINT roi_center{
            native_roi.left + (native_roi.right - native_roi.left) / 2,
            native_roi.top + (native_roi.bottom - native_roi.top) / 2,
        };
        const HMONITOR roi_monitor = MonitorFromPoint(roi_center, MONITOR_DEFAULTTONULL);
        if (roi_monitor == nullptr) {
            throw std::runtime_error("the center of the requested ROI is not on an active monitor");
        }

        MONITORINFO monitor_info{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(roi_monitor, &monitor_info)) {
            throw std::runtime_error("GetMonitorInfoW failed (Win32 error " +
                                     std::to_string(GetLastError()) + ")");
        }
        const core::Rect monitor_rect{
            monitor_info.rcMonitor.left,
            monitor_info.rcMonitor.top,
            monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
            monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
        };
        const auto local_crop = core::relative_rect(absolute_roi, monitor_rect);
        const bool changed = !duplication || handle != target || monitor != roi_monitor ||
            absolute.left != absolute_roi.left || absolute.top != absolute_roi.top ||
            size.width != roi.width || size.height != roi.height ||
            client.width != window.client_screen.width || client.height != window.client_screen.height;
        if (!changed) return false;
        release_held();
        LARGE_INTEGER changed_at{};
        QueryPerformanceCounter(&changed_at);
        minimum_present_qpc = changed_at.QuadPart;
        if (!duplication || handle != target || monitor != roi_monitor) {
            duplication.Reset(); context.Reset(); device.Reset(); staging.Reset();
            ComPtr<IDXGIFactory1> factory;
            check_hr(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),
                     "CreateDXGIFactory1");
            auto selected_output = find_output(factory.Get(), roi_monitor);
            if (selected_output.description.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                selected_output.description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                throw std::runtime_error("rotated display modes are not supported in this milestone");
            }

            check_hr(D3D11CreateDevice(selected_output.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                       nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                                       context.GetAddressOf()),
                     "D3D11CreateDevice");

            check_hr(selected_output.output->DuplicateOutput(device.Get(), duplication.GetAddressOf()),
                     "IDXGIOutput1::DuplicateOutput");

        }
        if (size.width != roi.width || size.height != roi.height) staging.Reset();
        handle = target; monitor = roi_monitor; absolute = absolute_roi; local = local_crop;
        client = window.client_screen; size = roi; ++generation;
        return true;
    }
};
DesktopCapture::DesktopCapture(bool hold_frame) : impl_(std::make_unique<Impl>(hold_frame)) {}
DesktopCapture::~DesktopCapture() = default;
bool DesktopCapture::prepare(std::uintptr_t window, core::Size roi) {
    return impl_->prepare(reinterpret_cast<HWND>(window), roi);
}
void DesktopCapture::reset() {
    impl_->release_held();
    impl_->duplication.Reset(); impl_->staging.Reset();
    impl_->context.Reset(); impl_->device.Reset();
}
CaptureStats DesktopCapture::stats() const { return impl_->stats; }
CaptureTimings DesktopCapture::last_timings() const { return impl_->timings; }
std::optional<core::Frame> DesktopCapture::next(std::uint32_t timeout_ms) {
    if (!impl_->duplication) throw std::logic_error("capture source is not prepared");
    std::int64_t source_qpc{};
    ++impl_->stats.calls;
    impl_->timings = {};
    auto pixels = copy_desktop_crop(impl_->duplication.Get(), impl_->device.Get(),
        impl_->context.Get(), impl_->local, impl_->size, impl_->staging, timeout_ms, source_qpc,
        impl_->minimum_present_qpc, impl_->hold_frame, impl_->held, impl_->stats, impl_->timings);
    if (!pixels) { ++impl_->stats.timeouts; return std::nullopt; }
    ++impl_->stats.frames;
    core::Frame frame;
    frame.size = impl_->size; frame.source_size = {impl_->client.width, impl_->client.height};
    frame.roi = impl_->absolute; frame.generation = impl_->generation;
    frame.pixels = std::move(*pixels); frame.source_qpc = source_qpc;
    frame.captured_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return frame;
}
void encode_png(const std::filesystem::path& path, core::Size size,
                const std::vector<std::uint8_t>& pixels) {
    ComApartment com;
    save_png(path, size, pixels);
}
void encode_png_no_filter(const std::filesystem::path& path, core::Size size,
                          const std::vector<std::uint8_t>& pixels) {
    ComApartment com;
    save_png(path, size, pixels, true);
}
void capture_once(const config::AppConfig& config, const core::Logger& logger) {
    platform::windows::set_per_monitor_dpi_awareness();
    const auto window = platform::windows::find_unique_window(
        platform::windows::wide_from_utf8(config.window_title));
    DesktopCapture source;
    source.prepare(reinterpret_cast<std::uintptr_t>(window.handle), config.roi);
    auto frame = source.next(5000);
    if (!frame) throw std::runtime_error("timed out waiting for a desktop image update");
    const auto directory = core::unique_directory(config.output,
        "capture_" + std::to_string(core::utc_milliseconds()));
    const auto path = directory / "frame.png";
    auto temp = path; temp += ".tmp";
    encode_png(temp, config.roi, frame->pixels);
    std::filesystem::rename(temp, path);
    logger.write(core::LogLevel::info, "Captured ROI to " + path.string());
}
} // namespace pubg_vision::capture
