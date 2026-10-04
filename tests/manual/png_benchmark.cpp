#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "capture/desktop_capture.hpp"
#include "collection/capture_scheduler.hpp"
#include "dataset/dataset_writer.hpp"
#include "dataset/png_encoder.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pubg_vision;
using Clock = std::chrono::steady_clock;
double thread_cpu_ms() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetThreadTimes failed");
    const auto ticks = [](FILETIME f) {
        return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
}
template<class F> void bench(const std::string& label, int count, F action,
                            const std::filesystem::path& output = {}) {
    for (int i = 0; i < 3; ++i) action();
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(count));
    const auto cpu = thread_cpu_ms();
    for (int i = 0; i < count; ++i) {
        const auto start = Clock::now();
        action();
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    const auto cpu_per_op = (thread_cpu_ms() - cpu) / count;
    std::sort(times.begin(), times.end());
    const auto percentile = [&](double q) {
        return times[static_cast<std::size_t>(std::ceil(q * count)) - 1];
    };
    std::cout << label << ',' << count << ',' << percentile(.50) << ',' << percentile(.95)
              << ',' << percentile(.99) << ',' << std::accumulate(times.begin(), times.end(), 0.0) / count
              << ',' << cpu_per_op << ',' << (output.empty() ? 0 : std::filesystem::file_size(output)) << '\n';
}
core::Frame image(int side, bool noise) {
    core::Frame f;
    f.size = {side, side}; f.source_size = f.size; f.roi = {0, 0, side, side};
    f.pixels.resize(static_cast<std::size_t>(side) * side * 4);
    std::mt19937 rng(42);
    for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
        const auto offset = (static_cast<std::size_t>(y) * side + x) * 4;
        for (int c = 0; c < 3; ++c)
            f.pixels[offset + c] = static_cast<std::uint8_t>(noise ? rng() & 255U : (x + y + c * 32) & 255);
        f.pixels[offset + 3] = 255;
    }
    return f;
}
core::Frame decode_sample(const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    const auto check = [](HRESULT h) { if (FAILED(h)) throw std::runtime_error("WIC sample decode failed"); };
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    struct Guard { ~Guard() { CoUninitialize(); } } guard;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(factory.GetAddressOf())));
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()));
    ComPtr<IWICBitmapFrameDecode> source;
    check(decoder->GetFrame(0, source.GetAddressOf()));
    UINT width{}, height{};
    check(source->GetSize(&width, &height));
    if (!width || !height || static_cast<std::uint64_t>(width) * height > 16U * 1024U * 1024U)
        throw std::runtime_error("invalid sample dimensions");
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(converter.GetAddressOf()));
    check(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    core::Frame f;
    f.size = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    f.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(f.pixels.size()), f.pixels.data()));
    return f;
}

int main(int argc, char* argv[]) {
    try {
        const auto root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("build/png-variants");
        std::filesystem::create_directories(root);
        std::cout << std::fixed << std::setprecision(6)
                  << "case,samples,p50_ms,p95_ms,p99_ms,mean_ms,thread_cpu_ms_per_op,last_file_bytes\n";
        for (const auto side : {320, 640, 1280}) for (const auto noise : {false, true}) {
            const auto f = image(side, noise);
            // Reverse order for noise to reduce a fixed first/second ordering bias.
            for (const auto no_filter : {noise, !noise}) {
                const auto label = std::string(no_filter ? "png_none_" : "png_default_") +
                    std::to_string(side) + (noise ? "_noise" : "_gradient");
                const auto path = root / (label + ".png");
                const auto encoder = no_filter ? capture::encode_png_no_filter : capture::encode_png;
                bench(label, 20, [&] { encoder(path, f.size, f.pixels); }, path);
                if (decode_sample(path).pixels != f.pixels)
                    throw std::runtime_error("PNG benchmark failed lossless round-trip: " + label);
            }
            const auto label = "png_store_" + std::to_string(side) + (noise ? "_noise" : "_gradient");
            const auto path = root / (label + ".png");
            bench(label, 20, [&] { dataset::encode_png_stored(path, f.size, f.pixels); }, path);
            if (decode_sample(path).pixels != f.pixels)
                throw std::runtime_error("Stored PNG benchmark failed lossless round-trip: " + label);
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
