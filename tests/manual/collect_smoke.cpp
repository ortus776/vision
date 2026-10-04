#include <windows.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstdlib>
#include "app/collect.hpp"
#include "platform/windows/window_selection.hpp"

namespace {
constexpr wchar_t target_class[] = L"PubgVisionCollectorSmokeTarget";
constexpr wchar_t target_title[] = L"PUBG Vision сборщик Smoke Target";
LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{}; auto dc = BeginPaint(window, &paint);
        RECT r{}; GetClientRect(window, &r);
        const auto brush = CreateSolidBrush(RGB(20, 100, 200));
        FillRect(dc, &r, brush); DeleteObject(brush); EndPaint(window, &paint); return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
HWND make_window() {
    auto w = CreateWindowExW(WS_EX_TOPMOST, target_class, target_title, WS_OVERLAPPEDWINDOW,
        100, 100, 820, 640, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w) throw std::runtime_error("could not create collector test window");
    ShowWindow(w, SW_SHOW); SetForegroundWindow(w); return w;
}
void hotkey(int id) {
    auto input = FindWindowExW(HWND_MESSAGE, nullptr, L"PubgVisionRawInput", nullptr);
    if (!input || !PostMessageW(input, WM_HOTKEY, static_cast<WPARAM>(id), 0))
        throw std::runtime_error("collector input message window was not found");
}
}
int main(int argc, char* argv[]) {
    std::thread worker;
    std::atomic<bool> finished{};
    std::exception_ptr failure;
    HWND window{};
    try {
        pubg_vision::platform::windows::set_per_monitor_dpi_awareness();
        WNDCLASSW wc{}; wc.lpfnWndProc = procedure; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = target_class;
        if (!RegisterClassW(&wc)) throw std::runtime_error("could not register smoke class");
        window = make_window();
        pubg_vision::config::AppConfig config;
        config.collect = true; config.window_title = "PUBG Vision сборщик Smoke Target";
        config.roi = {320, 240}; config.collection.periodic_interval = 150;
        config.trace_frames = true;
        config.output = argc > 1 ? argv[1] : "build/collect-smoke";
        if (argc > 2) config.collect_variant = argv[2];
        if (argc > 3) config.collect_seconds = std::stoi(argv[3]);
        worker = std::thread([&] {
            try { pubg_vision::app::collect(config, pubg_vision::core::Logger{}); }
            catch (...) { failure = std::current_exception(); }
            finished.store(true);
        });
        const auto start = std::chrono::steady_clock::now();
        int step{};
        while (!finished.load()) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= 500 && step == 0) { hotkey(8); ++step; }
            if (elapsed >= 1500 && step == 1) { hotkey(8); ++step; }
            if (elapsed >= 1800 && step == 2) { SetWindowPos(window, nullptr, 120, 120, 1000, 760, SWP_NOZORDER); ++step; }
            if (elapsed >= 2200 && step == 3) { hotkey(8); ++step; }
            if (elapsed >= 3000 && step == 4) { ShowWindow(window, SW_MINIMIZE); ++step; }
            if (elapsed >= 3600 && step == 5) { ShowWindow(window, SW_RESTORE); SetForegroundWindow(window); ++step; }
            if (elapsed >= 4800 && step == 6) { DestroyWindow(window); window = nullptr; ++step; }
            if (elapsed >= 5500 && step == 7) { window = make_window(); ++step; }
            if (elapsed >= 6500 && step == 8) {
                const auto monitors = pubg_vision::platform::windows::display_monitors();
                for (auto monitor : monitors) {
                    if (monitor == MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)) continue;
                    MONITORINFO info{sizeof(info)};
                    if (GetMonitorInfoW(monitor, &info)) {
                        SetWindowPos(window, nullptr, info.rcMonitor.left + 100,
                                     info.rcMonitor.top + 100, 820, 640, SWP_NOZORDER);
                        SetForegroundWindow(window);
                        break;
                    }
                }
                ++step;
            }
            if (elapsed >= 7500 && step == 9) { hotkey(9); ++step; }
            if (elapsed > 15000) { std::cerr << "collector smoke timeout\n"; std::quick_exit(3); }
            if (window) { InvalidateRect(window, nullptr, FALSE); UpdateWindow(window); }
            Sleep(10);
        }
        worker.join(); if (failure) std::rethrow_exception(failure);
        if (window) DestroyWindow(window);
        std::cout << "Collector smoke passed (pause, focus, resize, source recovery, stop)\n";
        return 0;
    } catch (const std::exception& ex) {
        if (worker.joinable()) {
            auto input = FindWindowExW(HWND_MESSAGE, nullptr, L"PubgVisionRawInput", nullptr);
            if (input) PostMessageW(input, WM_HOTKEY, 9, 0);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!finished.load() && std::chrono::steady_clock::now() < deadline) Sleep(10);
            if (!finished.load()) std::quick_exit(3);
            worker.join();
        }
        if (window) DestroyWindow(window);
        std::cerr << "Collector smoke failed: " << ex.what() << '\n'; return 1;
    }
}
