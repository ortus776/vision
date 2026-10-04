#include "dataset/png_encoder.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>

namespace pubg_vision::dataset {
namespace {
constexpr auto crc_table = [] {
    std::array<std::array<std::uint32_t, 256>, 8> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        auto value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
        table[0][i] = value;
    }
    for (std::size_t slice = 1; slice < table.size(); ++slice)
        for (std::size_t i = 0; i < 256; ++i) {
            const auto previous = table[slice - 1][i];
            table[slice][i] = table[0][previous & 255U] ^ (previous >> 8);
        }
    return table;
}();

std::uint32_t crc_update(std::uint32_t crc, std::span<const std::uint8_t> bytes) {
    // PNG uses IEEE CRC32, not the CRC32C polynomial in SSE4.2's CRC instruction.
    // Eight independent table lookups avoid a dependency chain for every byte.
    if constexpr (std::endian::native == std::endian::little) {
        while (bytes.size() >= 8) {
            std::uint64_t word{};
            std::memcpy(&word, bytes.data(), sizeof(word));
            word ^= crc;
            crc = crc_table[7][word & 255U] ^ crc_table[6][(word >> 8) & 255U] ^
                  crc_table[5][(word >> 16) & 255U] ^ crc_table[4][(word >> 24) & 255U] ^
                  crc_table[3][(word >> 32) & 255U] ^ crc_table[2][(word >> 40) & 255U] ^
                  crc_table[1][(word >> 48) & 255U] ^ crc_table[0][word >> 56];
            bytes = bytes.subspan(8);
        }
    }
    for (const auto byte : bytes) crc = crc_table[0][(crc ^ byte) & 255U] ^ (crc >> 8);
    return crc;
}

std::uint32_t adler32(std::span<const std::uint8_t> bytes) {
    std::uint32_t a = 1, b = 0;
    // Bound the block length so b cannot overflow before reduction modulo 65521.
    while (!bytes.empty()) {
        const auto length = std::min<std::size_t>(5552, bytes.size());
        for (const auto byte : bytes.first(length)) { a += byte; b += a; }
        a %= 65521; b %= 65521;
        bytes = bytes.subspan(length);
    }
    return (b << 16) | a;
}

std::array<std::uint8_t, 4> big_endian(std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
}

void write_bytes(std::ofstream& out, std::span<const std::uint8_t> bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void chunk(std::ofstream& out, std::array<std::uint8_t, 4> type,
           std::span<const std::uint8_t> bytes) {
    write_bytes(out, big_endian(static_cast<std::uint32_t>(bytes.size())));
    write_bytes(out, type);
    write_bytes(out, bytes);
    write_bytes(out, big_endian(crc_update(crc_update(0xffffffffU, type), bytes) ^ 0xffffffffU));
}
} // namespace

void encode_png_stored(const std::filesystem::path& path, core::Size size,
                       const std::vector<std::uint8_t>& pixels) {
    if (size.width <= 0 || size.height <= 0) throw std::invalid_argument("invalid PNG dimensions");
    const auto row_bytes = static_cast<std::uint64_t>(size.width) * 4;
    const auto raw_size = (row_bytes + 1) * static_cast<std::uint64_t>(size.height);
    if (pixels.size() != row_bytes * static_cast<std::uint64_t>(size.height) ||
        raw_size > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
        raw_size > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("PNG buffer size mismatch or image exceeds encoder limits");
    const auto block_count = (raw_size + 65534) / 65535;
    const auto idat_size = raw_size + block_count * 5 + 6;
    if (idat_size > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::invalid_argument("PNG IDAT exceeds the standard's chunk length limit");

    // Filter byte 0 at the start of every row, then RGBA pixels.
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(raw_size));
    const auto stride = static_cast<std::size_t>(row_bytes);
    for (std::int32_t y = 0; y < size.height; ++y) {
        const auto* input = pixels.data() + static_cast<std::size_t>(y) * stride;
        auto* output = raw.data() + static_cast<std::size_t>(y) * (stride + 1) + 1;
        for (std::size_t x = 0; x < stride; x += 4) {
            output[x] = input[x + 2]; output[x + 1] = input[x + 1];
            output[x + 2] = input[x]; output[x + 3] = input[x + 3];
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("could not open stored PNG output");
    constexpr std::array<std::uint8_t, 8> signature{137,80,78,71,13,10,26,10};
    write_bytes(out, signature);
    std::array<std::uint8_t, 13> ihdr{};
    const auto width = big_endian(static_cast<std::uint32_t>(size.width));
    const auto height = big_endian(static_cast<std::uint32_t>(size.height));
    std::copy(width.begin(), width.end(), ihdr.begin());
    std::copy(height.begin(), height.end(), ihdr.begin() + 4);
    ihdr[8] = 8; ihdr[9] = 6; // 8-bit RGBA, no interlacing.
    chunk(out, {'I','H','D','R'}, ihdr);

    // Assemble IDAT so the file receives one large payload write instead of
    // alternating tiny DEFLATE headers and 64KiB writes for every block.
    std::vector<std::uint8_t> zlib(static_cast<std::size_t>(idat_size));
    auto cursor = zlib.data();
    const auto payload = [&](std::span<const std::uint8_t> bytes) {
        std::memcpy(cursor, bytes.data(), bytes.size());
        cursor += bytes.size();
    };
    constexpr std::array<std::uint8_t, 2> zlib_header{0x78,0x01};
    payload(zlib_header);
    std::span<const std::uint8_t> remaining(raw);
    while (!remaining.empty()) {
        const auto length = static_cast<std::uint16_t>(std::min<std::size_t>(remaining.size(), 65535));
        const auto inverse = static_cast<std::uint16_t>(~length);
        const std::array<std::uint8_t, 5> header{
            static_cast<std::uint8_t>(remaining.size() == length ? 1 : 0), // BFINAL, BTYPE=00.
            static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
            static_cast<std::uint8_t>(inverse), static_cast<std::uint8_t>(inverse >> 8)};
        payload(header);
        payload(remaining.first(length));
        remaining = remaining.subspan(length);
    }
    payload(big_endian(adler32(raw)));
    chunk(out, {'I','D','A','T'}, zlib);
    chunk(out, {'I','E','N','D'}, {});
    out.close();
    if (!out) throw std::runtime_error("could not write stored PNG (disk full or unavailable)");
}
} // namespace pubg_vision::dataset
