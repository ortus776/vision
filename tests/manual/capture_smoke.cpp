#include <windows.h>
#include <dwmapi.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/desktop_capture.hpp"
#include "config/app_config.hpp"
#include "core/logger.hpp"
#include "platform/windows/window_selection.hpp"

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"PubgVisionCaptureSmokeWindow";
constexpr wchar_t kWindowTitle[] = L"PUBG Vision Capture Smoke Target";
constexpr int kClientWidth = 800;
constexpr int kClientHeight = 600;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int half_width = (client.right - client.left) / 2;
        const int half_height = (client.bottom - client.top) / 2;
        const std::array<std::pair<RECT, COLORREF>, 4> quadrants{{
            {RECT{0, 0, half_width, half_height}, RGB(255, 0, 0)},
            {RECT{half_width, 0, client.right, half_height}, RGB(0, 255, 0)},
            {RECT{0, half_height, half_width, client.bottom}, RGB(0, 0, 255)},
            {RECT{half_width, half_height, client.right, client.bottom}, RGB(255, 255, 0)},
        }};
        for (const auto& [rect, color] : quadrants) {
            const HBRUSH brush = CreateSolidBrush(color);
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

[[nodiscard]] std::filesystem::path newest_png(const std::filesystem::path& directory) {
    std::filesystem::path newest;
    std::filesystem::file_time_type newest_time{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".png") {
            continue;
        }
        const auto modified = entry.last_write_time();
        if (newest.empty() || modified > newest_time) {
            newest = entry.path();
            newest_time = modified;
        }
    }
    if (newest.empty()) {
        throw std::runtime_error("capture command did not produce a PNG file");
    }
    return newest;
}

struct Bgra {
    std::uint8_t blue;
    std::uint8_t green;
    std::uint8_t red;
    std::uint8_t alpha;
};

[[nodiscard]] std::vector<std::uint8_t> decode_png(const std::filesystem::path& path,
                                                    pubg_vision::core::Size expected_size) {
    struct ComGuard {
        bool initialized{false};
        ~ComGuard() {
            if (initialized) {
                CoUninitialize();
            }
        }
    } com;

    const HRESULT init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init_result)) {
        throw std::runtime_error("CoInitializeEx failed in smoke-test decoder");
    }
    com.initialized = true;

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        throw std::runtime_error("could not create WIC factory for PNG verification");
    }

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand,
                                                decoder.GetAddressOf());
    if (FAILED(result)) {
        throw std::runtime_error("could not reopen the captured PNG");
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(result)) {
        throw std::runtime_error("could not read the captured PNG frame");
    }

    UINT width{};
    UINT height{};
    result = frame->GetSize(&width, &height);
    if (FAILED(result) || width != static_cast<UINT>(expected_size.width) ||
        height != static_cast<UINT>(expected_size.height)) {
        throw std::runtime_error("captured PNG dimensions do not match the requested ROI");
    }

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(result)) {
        throw std::runtime_error("could not create WIC pixel converter");
    }
    result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        throw std::runtime_error("could not convert captured PNG to BGRA");
    }

    const UINT stride = width * sizeof(Bgra);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
    result = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                   pixels.data());
    if (FAILED(result)) {
        throw std::runtime_error("could not decode captured PNG pixels");
    }
    return pixels;
}

void expect_color(const std::vector<std::uint8_t>& pixels, int width, int x, int y,
                  std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                  const char* name) {
    const auto offset = (static_cast<std::size_t>(y) * width + x) * sizeof(Bgra);
    const Bgra actual{pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
    const auto close = [](std::uint8_t actual_value, std::uint8_t expected) {
        const int difference = static_cast<int>(actual_value) - expected;
        return difference >= -4 && difference <= 4;
    };
    if (!close(actual.red, red) || !close(actual.green, green) ||
        !close(actual.blue, blue) || actual.alpha < 250) {
        throw std::runtime_error(std::string("captured pixel check failed for ") + name +
                                 " (BGRA " + std::to_string(actual.blue) + "," +
                                 std::to_string(actual.green) + "," +
                                 std::to_string(actual.red) + "," +
                                 std::to_string(actual.alpha) + ")");
    }
}

[[nodiscard]] std::filesystem::path run_capture_case(
    HWND window, pubg_vision::core::Size roi, const std::filesystem::path& output,
    const char* label) {
    MSG pending{};
    while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&pending);
        DispatchMessageW(&pending);
    }
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window, nullptr, TRUE);
    UpdateWindow(window);
    (void)DwmFlush();
    Sleep(100);

    const auto selected_window = pubg_vision::platform::windows::find_unique_window(
        kWindowTitle);
    const auto requested_roi = pubg_vision::core::centered_rect(selected_window.client_screen,
                                                                  roi);
    POINT sample{selected_window.client_screen.left + selected_window.client_screen.width / 4,
                 selected_window.client_screen.top + selected_window.client_screen.height / 4};
    const auto visible = GetAncestor(WindowFromPoint(sample), GA_ROOT);
    if (visible != window) {
        throw std::runtime_error("test window is occluded; move other windows away before running GUI smoke");
    }
    // Verify visibility through the decoded DXGI PNG below. A screen GDI DC may
    // not expose pixels of an output on another adapter, so GetPixel is not a
    // valid precondition for that capture path.
    std::cout << label << ": client (" << selected_window.client_screen.left << ","
              << selected_window.client_screen.top << ") "
              << selected_window.client_screen.width << "x"
              << selected_window.client_screen.height << "; ROI ("
              << requested_roi.left << "," << requested_roi.top << ") "
              << roi.width << "x" << roi.height << '\n';

    pubg_vision::config::AppConfig config;
    config.roi = roi;
    config.window_title = "PUBG Vision Capture Smoke Target";
    config.output = output;

    const pubg_vision::core::Logger logger;
    std::atomic<bool> capture_finished{false};
    std::exception_ptr capture_error;
    std::thread capture_thread([&] {
        try {
            pubg_vision::capture::capture_once(config, logger);
        } catch (...) {
            capture_error = std::current_exception();
        }
        capture_finished.store(true);
    });
    const auto test_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!capture_finished.load()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, FALSE);
        UpdateWindow(window);
        Sleep(30);
        if (std::chrono::steady_clock::now() >= test_deadline) {
            std::cerr << "Capture smoke test timed out\n" << std::flush;
            std::quick_exit(3);
        }
    }
    capture_thread.join();
    if (capture_error) {
        std::rethrow_exception(capture_error);
    }
    const auto path = newest_png(config.output);
    const auto pixels = decode_png(path, roi);

    // The center crop crosses all four color quadrants of the test window.
    expect_color(pixels, roi.width, 20, 20, 255, 0, 0, "top-left quadrant");
    expect_color(pixels, roi.width, roi.width - 20, 20, 0, 255, 0,
                 "top-right quadrant");
    expect_color(pixels, roi.width, 20, roi.height - 20, 0, 0, 255,
                 "bottom-left quadrant");
    expect_color(pixels, roi.width, roi.width - 20, roi.height - 20, 255, 255, 0,
                 "bottom-right quadrant");
    return path;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        pubg_vision::platform::windows::set_per_monitor_dpi_awareness();
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = kWindowClass;
        if (RegisterClassExW(&window_class) == 0) {
            throw std::runtime_error("RegisterClassExW failed");
        }

        RECT outer{100, 100, 100 + kClientWidth, 100 + kClientHeight};
        if (!AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE)) {
            throw std::runtime_error("AdjustWindowRect failed");
        }
        HWND window = CreateWindowExW(WS_EX_TOPMOST, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                      outer.left, outer.top, outer.right - outer.left,
                                      outer.bottom - outer.top, nullptr, nullptr,
                                      window_class.hInstance, nullptr);
        if (window == nullptr) {
            throw std::runtime_error("CreateWindowExW failed");
        }
        SetWindowPos(window, HWND_TOPMOST, outer.left, outer.top,
                     outer.right - outer.left, outer.bottom - outer.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(window, SW_SHOWNORMAL);
        BringWindowToTop(window);
        (void)SetForegroundWindow(window);
        InvalidateRect(window, nullptr, TRUE);
        UpdateWindow(window);
        (void)DwmFlush();
        Sleep(500);

        const auto output_root = argc > 1 ? std::filesystem::path(argv[1])
                                          : std::filesystem::path("build/capture-smoke");
        const auto primary_path = run_capture_case(window, {320, 240},
                                                    output_root / "primary", "Primary monitor");

        std::vector<HMONITOR> monitors;
        if (!EnumDisplayMonitors(nullptr, nullptr,
                [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
                    auto& found = *reinterpret_cast<std::vector<HMONITOR>*>(parameter);
                    found.push_back(monitor);
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&monitors))) {
            throw std::runtime_error("EnumDisplayMonitors failed");
        }
        const HMONITOR primary_monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        std::filesystem::path secondary_path;
        for (const HMONITOR monitor : monitors) {
            if (monitor == primary_monitor) {
                continue;
            }
            MONITORINFO monitor_info{sizeof(MONITORINFO)};
            if (!GetMonitorInfoW(monitor, &monitor_info)) {
                throw std::runtime_error("GetMonitorInfoW failed in smoke test");
            }
            const int monitor_width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
            const int monitor_height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
            if (monitor_width < kClientWidth + 200 || monitor_height < kClientHeight + 200) {
                continue;
            }
            RECT secondary_outer{
                monitor_info.rcMonitor.left + 100,
                monitor_info.rcMonitor.top + 100,
                monitor_info.rcMonitor.left + 100 + kClientWidth,
                monitor_info.rcMonitor.top + 100 + kClientHeight,
            };
            if (!AdjustWindowRect(&secondary_outer, WS_OVERLAPPEDWINDOW, FALSE) ||
                !SetWindowPos(window, HWND_TOPMOST, secondary_outer.left, secondary_outer.top,
                              secondary_outer.right - secondary_outer.left,
                              secondary_outer.bottom - secondary_outer.top, SWP_SHOWWINDOW)) {
                throw std::runtime_error("could not move smoke-test window to second monitor");
            }
            secondary_path = run_capture_case(window, {640, 480},
                                              output_root / "secondary", "Second monitor");
            break;
        }

        DestroyWindow(window);
        UnregisterClassW(kWindowClass, window_class.hInstance);
        std::cout << "Capture smoke test passed: " << primary_path.string();
        if (!secondary_path.empty()) {
            std::cout << " and " << secondary_path.string();
        }
        std::cout << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Capture smoke test failed: " << exception.what() << '\n';
        return 1;
    }
}
