// A real D3D11 swap chain for end-to-end PresentMon tests without launching a game.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace {
LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) { DestroyWindow(window); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, msg, wp, lp);
}
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D11 fixture failed: " + std::to_string(result)); }
}
int main(int argc, char* argv[]) {
    try {
        const int seconds = argc > 1 ? std::stoi(argv[1]) : 30;
        WNDCLASSW wc{}; wc.lpfnWndProc = procedure; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"CollectorPresentMonFixture";
        if (!RegisterClassW(&wc)) throw std::runtime_error("could not register fixture window");
        HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Collector PresentMon Test Target",
            WS_OVERLAPPEDWINDOW, 50, 50, 850, 750, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) throw std::runtime_error("could not create fixture window");
        ShowWindow(window, SW_SHOW); SetForegroundWindow(window);
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferDesc.Width = 800; description.BufferDesc.Height = 700;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1; description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2; description.OutputWindow = window; description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; ComPtr<IDXGISwapChain> chain;
        check(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &description, &chain, &device, nullptr, &context));
        ComPtr<ID3D11Texture2D> texture; check(chain->GetBuffer(0, IID_PPV_ARGS(&texture)));
        ComPtr<ID3D11RenderTargetView> target; check(device->CreateRenderTargetView(texture.Get(), nullptr, &target));
        const auto start = std::chrono::steady_clock::now();
        auto deadline = start;
        unsigned frames{};
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return 0;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            const float color[]{0.1f, static_cast<float>(frames % 100) / 100.0f, 0.4f, 1.0f};
            // Keep this synthetic target active during unattended collector smoke tests.
            if (argc > 2 && frames == 180) {
                ShowWindow(window, SW_MINIMIZE); ShowWindow(window, SW_RESTORE); SetForegroundWindow(window);
            }
            context->ClearRenderTargetView(target.Get(), color);
            check(chain->Present(0, 0));
            ++frames;
            deadline += std::chrono::microseconds(11111);
            std::this_thread::sleep_until(deadline);
        }
        DestroyWindow(window);
        std::cout << "D3D11 target presented " << frames << " frames\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
