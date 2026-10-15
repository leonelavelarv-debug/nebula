// SPDX-License-Identifier: GPL-3.0-only
// texture_pack.cpp: the texture-pack index, loaders, resident replacements and dump writer.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "galaxy/gx/texture_pack.h"

#include "galaxy/gx/texture_cache.h"
#include "galaxy/windows_power.h"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <system_error>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace galaxy::gx {

// ---------------------------------------------------------------------------
// Hashes
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
constexpr std::uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;
constexpr std::uint64_t kPrime3 = 0x165667B19E3779F9ull;
constexpr std::uint64_t kPrime4 = 0x85EBCA77C2B2AE63ull;
constexpr std::uint64_t kPrime5 = 0x27D4EB2F165667C5ull;

std::uint64_t read64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 8u);
    return v;  // little-endian host
}

std::uint32_t read32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4u);
    return v;
}

std::uint64_t xxh_round(std::uint64_t acc, std::uint64_t lane) noexcept {
    acc += lane * kPrime2;
    acc = std::rotl(acc, 31);
    return acc * kPrime1;
}

std::uint64_t xxh_merge(std::uint64_t acc, std::uint64_t value) noexcept {
    acc ^= xxh_round(0u, value);
    return acc * kPrime1 + kPrime4;
}

}  // namespace

std::uint64_t xxh64(const void* data, std::size_t size, std::uint64_t seed) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::uint8_t* const end = p + size;
    std::uint64_t acc = 0;
    if (size >= 32u) {
        std::uint64_t a1 = seed + kPrime1 + kPrime2;
        std::uint64_t a2 = seed + kPrime2;
        std::uint64_t a3 = seed;
        std::uint64_t a4 = seed - kPrime1;
        const std::uint8_t* const limit = end - 32u;
        do {
            a1 = xxh_round(a1, read64(p));
            a2 = xxh_round(a2, read64(p + 8u));
            a3 = xxh_round(a3, read64(p + 16u));
            a4 = xxh_round(a4, read64(p + 24u));
            p += 32u;
        } while (p <= limit);
        acc = std::rotl(a1, 1) + std::rotl(a2, 7) + std::rotl(a3, 12) + std::rotl(a4, 18);
        acc = xxh_merge(acc, a1);
        acc = xxh_merge(acc, a2);
        acc = xxh_merge(acc, a3);
        acc = xxh_merge(acc, a4);
    } else {
        acc = seed + kPrime5;
    }
    acc += static_cast<std::uint64_t>(size);
    while (p + 8u <= end) {
        acc ^= xxh_round(0u, read64(p));
        acc = std::rotl(acc, 27) * kPrime1 + kPrime4;
        p += 8u;
    }
    if (p + 4u <= end) {
        acc ^= static_cast<std::uint64_t>(read32(p)) * kPrime1;
        acc = std::rotl(acc, 23) * kPrime2 + kPrime3;
        p += 4u;
    }
    while (p < end) {
        acc ^= static_cast<std::uint64_t>(*p) * kPrime5;
        acc = std::rotl(acc, 11) * kPrime1;
        ++p;
    }
    acc ^= acc >> 33;
    acc *= kPrime2;
    acc ^= acc >> 29;
    acc *= kPrime3;
    acc ^= acc >> 32;
    return acc;
}

std::uint64_t texture_pack_hash(const std::uint8_t* base_level, std::size_t bytes) noexcept {
    return xxh64(base_level, bytes, 0u);
}

bool texture_format_palettized(TexFormat format) noexcept {
    return format == TexFormat::C4 || format == TexFormat::C8 || format == TexFormat::C14X2;
}

std::uint64_t texture_pack_tlut_hash(TexFormat format, const std::uint8_t* base_level, std::uint32_t width,
                                     std::uint32_t height, const std::uint8_t* palette) noexcept {
    if (!texture_format_palettized(format) || base_level == nullptr || palette == nullptr) {
        return 0u;
    }
    const std::size_t bytes = texture_guest_byte_size(format, width, height, 1u);
    std::uint32_t lo = 0xFFFFu;
    std::uint32_t hi = 0u;
    if (format == TexFormat::C4) {
        for (std::size_t i = 0; i < bytes; ++i) {
            const std::uint32_t a = base_level[i] >> 4u;
            const std::uint32_t b = base_level[i] & 0xFu;
            lo = std::min({lo, a, b});
            hi = std::max({hi, a, b});
        }
    } else if (format == TexFormat::C8) {
        for (std::size_t i = 0; i < bytes; ++i) {
            lo = std::min<std::uint32_t>(lo, base_level[i]);
            hi = std::max<std::uint32_t>(hi, base_level[i]);
        }
    } else {
        for (std::size_t i = 0; i + 1u < bytes; i += 2u) {
            const std::uint32_t v = ((static_cast<std::uint32_t>(base_level[i]) << 8u) | base_level[i + 1u]) &
                                    0x3FFFu;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (lo > hi) {
        return 0u;
    }
    return xxh64(palette + lo * 2u, static_cast<std::size_t>(hi - lo + 1u) * 2u, 0u);
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

namespace {

bool parse_hex64(std::string_view text, std::uint64_t& out) noexcept {
    if (text.size() != 16u) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        value <<= 4u;
        if (c >= '0' && c <= '9') value |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
    }
    out = value;
    return true;
}

bool parse_uint(std::string_view text, std::uint32_t limit, std::uint32_t& out) noexcept {
    if (text.empty() || text.size() > 5u) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10u + static_cast<std::uint32_t>(c - '0');
    }
    if (value > limit) {
        return false;
    }
    out = value;
    return true;
}

bool known_format(std::uint32_t format) noexcept {
    return format <= 6u || format == 8u || format == 9u || format == 10u || format == 14u;
}

}  // namespace

std::optional<TexturePackName> parse_texture_pack_name(std::string_view stem) noexcept {
    constexpr std::string_view kPrefix = "tex1_";
    if (stem.size() < kPrefix.size() || stem.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    std::string_view rest = stem.substr(kPrefix.size());
    std::array<std::string_view, 8> parts{};
    std::size_t count = 0;
    while (!rest.empty()) {
        if (count == parts.size()) return std::nullopt;
        const std::size_t cut = rest.find('_');
        parts[count++] = rest.substr(0, cut);
        if (cut == std::string_view::npos) break;
        rest = rest.substr(cut + 1u);
    }
    TexturePackName name{};
    std::size_t at = 0;
    // <w>x<h>
    {
        const std::string_view size = parts[at++];
        const std::size_t x = size.find('x');
        std::uint32_t w = 0, h = 0;
        if (x == std::string_view::npos || !parse_uint(size.substr(0, x), 1024u, w) ||
            !parse_uint(size.substr(x + 1u), 1024u, h) || w == 0u || h == 0u) {
            return std::nullopt;
        }
        name.width = static_cast<std::uint16_t>(w);
        name.height = static_cast<std::uint16_t>(h);
    }
    if (at < count && parts[at] == "m") {
        name.mipmapped = true;
        ++at;
    }
    if (at >= count || !parse_hex64(parts[at++], name.hash)) {
        return std::nullopt;
    }
    if (at < count && parts[at] == "$") {
        name.any_tlut = true;
        ++at;
    } else if (at < count && parts[at].size() == 16u) {
        if (!parse_hex64(parts[at++], name.tlut)) return std::nullopt;
    }
    std::uint32_t format = 0;
    if (at >= count || !parse_uint(parts[at++], 255u, format) || !known_format(format)) {
        return std::nullopt;
    }
    name.format = static_cast<std::uint8_t>(format);
    const bool palettized = texture_format_palettized(static_cast<TexFormat>(format));
    if (!palettized && (name.any_tlut || name.tlut != 0u)) {
        return std::nullopt;
    }
    if (at < count) {
        const std::string_view tail = parts[at++];
        std::uint32_t level = 0;
        if (tail == "arb") {
            name.arbitrary = true;
        } else if (tail.size() > 3u && tail.substr(0, 3u) == "mip" && parse_uint(tail.substr(3u), 15u, level) &&
                   level != 0u) {
            name.level = static_cast<std::uint8_t>(level);
        } else {
            return std::nullopt;
        }
    }
    if (at != count) {
        return std::nullopt;
    }
    return name;
}

std::string texture_pack_name(const TexturePackName& name) {
    char text[96]{};
    int length = std::snprintf(text, sizeof(text), "tex1_%ux%u%s_%016llx", name.width, name.height,
                               name.mipmapped ? "_m" : "", static_cast<unsigned long long>(name.hash));
    if (texture_format_palettized(static_cast<TexFormat>(name.format))) {
        length += name.any_tlut ? std::snprintf(text + length, sizeof(text) - length, "_$")
                                : std::snprintf(text + length, sizeof(text) - length, "_%016llx",
                                                static_cast<unsigned long long>(name.tlut));
    }
    length += std::snprintf(text + length, sizeof(text) - length, "_%u", name.format);
    if (name.level != 0u) {
        std::snprintf(text + length, sizeof(text) - length, "_mip%u", name.level);
    } else if (name.arbitrary) {
        std::snprintf(text + length, sizeof(text) - length, "_arb");
    }
    return text;
}

// ---------------------------------------------------------------------------
// DDS
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8u) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16u) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24u);
}

// Bytes of one 4x4 block, 0 for uncompressed formats (4 bytes a pixel), ~0 for unsupported.
std::uint32_t dds_block_bytes(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
        return 8u;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_UNORM:
        return 16u;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return 0u;
    default:
        return ~0u;
    }
}

DXGI_FORMAT dds_unorm(std::uint32_t dxgi) noexcept {
    switch (dxgi) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS:
        return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC2_TYPELESS:
        return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
        return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_TYPELESS:
        return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_TYPELESS:
        return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: case DXGI_FORMAT_BC7_TYPELESS:
        return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

}  // namespace

bool parse_dds(std::span<const std::uint8_t> file, DdsLayout& out) noexcept {
    // "DDS " magic, DDS_HEADER (124 bytes): size, flags, height, width, pitch, depth, mip count,
    // reserved[11], pixel format (size, flags, fourCC, bit count, R/G/B/A masks), caps[4], reserved.
    if (file.size() < 128u || read32(file.data()) != fourcc('D', 'D', 'S', ' ') || read32(file.data() + 4u) != 124u) {
        return false;
    }
    const std::uint8_t* h = file.data() + 4u;
    const std::uint32_t height = read32(h + 8u);
    const std::uint32_t width = read32(h + 12u);
    const std::uint32_t depth = read32(h + 20u);
    std::uint32_t levels = read32(h + 24u);
    const std::uint8_t* pf = h + 72u;
    const std::uint32_t pf_flags = read32(pf + 4u);
    const std::uint32_t code = read32(pf + 8u);
    const std::uint32_t bits = read32(pf + 12u);
    const std::uint32_t r_mask = read32(pf + 16u);
    const std::uint32_t a_mask = read32(pf + 28u);
    constexpr std::uint32_t kFourCC = 0x4u;
    constexpr std::uint32_t kRgb = 0x40u;
    std::size_t offset = 128u;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if ((pf_flags & kFourCC) != 0u) {
        if (code == fourcc('D', 'X', '1', '0')) {
            // DDS_HEADER_DXT10: dxgi format, dimension (3 = 2D), misc, array size, misc2.
            if (file.size() < 148u || read32(file.data() + 132u) != 3u || read32(file.data() + 140u) > 1u) {
                return false;
            }
            format = dds_unorm(read32(file.data() + 128u));
            offset = 148u;
        } else if (code == fourcc('D', 'X', 'T', '1')) {
            format = DXGI_FORMAT_BC1_UNORM;
        } else if (code == fourcc('D', 'X', 'T', '2') || code == fourcc('D', 'X', 'T', '3')) {
            format = DXGI_FORMAT_BC2_UNORM;
        } else if (code == fourcc('D', 'X', 'T', '4') || code == fourcc('D', 'X', 'T', '5')) {
            format = DXGI_FORMAT_BC3_UNORM;
        } else if (code == fourcc('A', 'T', 'I', '1') || code == fourcc('B', 'C', '4', 'U')) {
            format = DXGI_FORMAT_BC4_UNORM;
        } else if (code == fourcc('A', 'T', 'I', '2') || code == fourcc('B', 'C', '5', 'U')) {
            format = DXGI_FORMAT_BC5_UNORM;
        }
    } else if ((pf_flags & kRgb) != 0u && bits == 32u && a_mask == 0xFF000000u) {
        format = r_mask == 0x00FF0000u ? DXGI_FORMAT_B8G8R8A8_UNORM
               : r_mask == 0x000000FFu ? DXGI_FORMAT_R8G8B8A8_UNORM
                                       : DXGI_FORMAT_UNKNOWN;
    }
    const std::uint32_t block = dds_block_bytes(format);
    if (format == DXGI_FORMAT_UNKNOWN || block == ~0u || width == 0u || height == 0u || width > 16384u ||
        height > 16384u || depth > 1u) {
        return false;
    }
    levels = std::clamp<std::uint32_t>(levels, 1u, 16u);
    const std::uint32_t full = static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));
    levels = std::min(levels, full);
    out = DdsLayout{};
    out.format = format;
    out.width = width;
    out.height = height;
    out.data_offset = offset;
    std::size_t at = offset;
    std::uint32_t kept = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::uint32_t lw = std::max(1u, width >> level);
        const std::uint32_t lh = std::max(1u, height >> level);
        const std::uint32_t row_bytes = block != 0u ? ((lw + 3u) / 4u) * block : lw * 4u;
        const std::uint32_t rows = block != 0u ? (lh + 3u) / 4u : lh;
        const std::size_t bytes = static_cast<std::size_t>(row_bytes) * rows;
        if (at + bytes > file.size()) {
            break;
        }
        out.level_offset[level] = at;
        out.row_bytes[level] = row_bytes;
        out.rows[level] = rows;
        at += bytes;
        ++kept;
    }
    out.levels = kept;
    return kept != 0u;
}

// ---------------------------------------------------------------------------
// TexturePacks
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kUploadBytesPerFrame = 48ull << 20;
constexpr std::uint32_t kPreloadOutstanding = 32u;

// A file or folder name in lower case if it is ASCII (pack names are), else empty.
std::string ascii_lower(const std::filesystem::path& name) {
    const std::wstring wide = name.native();
    std::string text;
    text.reserve(wide.size());
    for (const wchar_t c : wide) {
        if (c == 0 || c > 0x7F) return {};
        text.push_back(static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c));
    }
    return text;
}

std::string ascii_name(const std::filesystem::path& name) {
    const std::wstring wide = name.native();
    std::string text;
    text.reserve(wide.size());
    for (const wchar_t c : wide) {
        if (c == 0 || c > 0x7F) return {};
        text.push_back(static_cast<char>(c));
    }
    return text;
}

std::string display(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > (std::streamoff{1} << 31)) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(bytes.data()), size));
}

// PNG (or any WIC-readable image) -> straight-alpha RGBA8. The calling thread has COM initialized.
bool decode_image(IWICImagingFactory* wic, const std::filesystem::path& path, std::vector<RGBA8>& pixels,
                  std::uint32_t& width, std::uint32_t& height) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (wic == nullptr ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || width == 0u ||
        height == 0u || width > 16384u || height > 16384u || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }
    pixels.resize(static_cast<std::size_t>(width) * height);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4u, width * height * 4u,
                                           reinterpret_cast<BYTE*>(pixels.data())));
}

// RGBA8 pixels (swizzled to BGRA in place: the PNG encoder's 32-bit input) -> a PNG file.
bool encode_png(IWICImagingFactory* wic, const std::filesystem::path& path, std::uint32_t width,
                std::uint32_t height, std::vector<std::uint8_t>& pixels) {
    for (std::size_t i = 0; i + 3u < pixels.size(); i += 4u) {
        std::swap(pixels[i], pixels[i + 2u]);
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool written =
        wic != nullptr && SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        format == GUID_WICPixelFormat32bppBGRA &&
        SUCCEEDED(frame->WritePixels(height, width * 4u, width * height * 4u, pixels.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (!written) {
        frame.Reset();
        encoder.Reset();
        stream.Reset();
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    return written;
}

// The loader thread's WIC factory (COM initialized once per thread).
IWICImagingFactory* thread_wic() {
    thread_local ComPtr<IWICImagingFactory> factory;
    thread_local bool tried = false;
    if (!tried) {
        tried = true;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(init) || init == RPC_E_CHANGED_MODE) {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        }
    }
    return factory.Get();
}

}  // namespace

std::uint64_t TexturePacks::size_key(std::uint32_t width, std::uint32_t height, std::uint32_t format) noexcept {
    return (static_cast<std::uint64_t>(width) << 32u) | (static_cast<std::uint64_t>(height) << 8u) | format;
}

std::uint64_t TexturePacks::name_key(std::uint64_t size, std::uint64_t hash, std::uint64_t tlut) noexcept {
    std::uint64_t key = hash;
    key ^= (tlut + 0x9E3779B97F4A7C15ull + (key << 6u) + (key >> 2u));
    key ^= (size + 0x9E3779B97F4A7C15ull + (key << 6u) + (key >> 2u));
    return key;
}

void TexturePacks::initialize(ID3D12Device* device) {
    device_ = device;
    if (workers_.empty()) {
        stopping_ = false;
        const unsigned count = std::clamp(std::thread::hardware_concurrency() / 8u, 2u, 4u);
        for (unsigned i = 0; i < count; ++i) {
            workers_.emplace_back([this] { worker_main(); });
        }
    }
    if (configured_) {
        // A renderer started again keeps its settings: the index is built anew.
        configured_ = false;
        const TexturePackSettings settings = settings_;
        configure(settings);
    }
}

void TexturePacks::shutdown() noexcept {
    stop_workers();
    clear_slots();
    index_.reset();
    retired_uploads_.clear();
    dumped_.clear();
    device_.Reset();
}

void TexturePacks::stop_workers() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    finished_.clear();
    built_.reset();
}

void TexturePacks::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || workers_.empty()) return;
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void TexturePacks::worker_main() {
    galaxy::host::name_current_thread(L"Nebula Texture Pack");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            job();
        } catch (const std::exception& error) {
            std::cerr << "[texture-pack] loader error: " << error.what() << '\n';
        }
    }
}

void TexturePacks::configure(const TexturePackSettings& settings) {
    const bool enable = settings.enabled && !settings.packs.empty();
    const bool was_enabled = configured_ && settings_.enabled && !settings_.packs.empty();
    const bool reindex = enable && (!was_enabled || settings.packs != settings_.packs);
    const bool disable = !enable && (was_enabled || !configured_);
    // Dumping covers textures decoded from now on: the cache decodes everything again.
    changed_ = changed_ || (configured_ && settings.dump != settings_.dump);
    configured_ = true;
    settings_ = settings;
    if (disable) {
        ++generation_;
        changed_ = changed_ || index_ != nullptr;
        index_.reset();
        clear_slots();
        return;
    }
    if (!reindex) {
        evict_to_budget(0u);
        return;
    }
    // The new index replaces the old one at a later pump; the old slots serve until then.
    const std::filesystem::path root = settings_.packs;
    const std::uint64_t generation = ++generation_;
    submit([this, root, generation] {
        std::shared_ptr<Index> index = build_index(root, generation);
        std::lock_guard<std::mutex> lock(mutex_);
        built_ = std::move(index);
    });
}

void TexturePacks::clear_slots() noexcept {
    slots_.clear();
    ready_queue_.clear();
    outstanding_ = 0;
    preload_cursor_ = 0;
    resident_bytes_ = 0;
    resident_count_ = 0;
}

std::shared_ptr<TexturePacks::Index> TexturePacks::build_index(const std::filesystem::path& root,
                                                               std::uint64_t generation) {
    const auto start = std::chrono::steady_clock::now();
    auto index = std::make_shared<Index>();
    index->generation = generation;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return index;
    }
    // Game folders: <root>/<GameID>, its 3-letter region-free prefix, or the same one or two levels
    // down (a pack unpacked into its own folder); or the root itself when it is one.
    const std::string id = ascii_lower(kGameId);
    const std::string region_free = id.substr(0, 3);
    std::vector<std::filesystem::path> games;
    const std::string root_name = ascii_lower(root.filename());
    if (root_name == id || root_name == region_free) {
        games.push_back(root);
    } else {
        for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it.depth() > 2) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_directory(error)) continue;
            const std::string name = ascii_lower(it->path().filename());
            if (name == id || name == region_free) {
                games.push_back(it->path());
                it.disable_recursion_pending();
            }
        }
    }
    std::sort(games.begin(), games.end());
    struct Found {
        std::filesystem::path path;
        TexturePackName name;
        bool dds;
    };
    std::vector<Found> found;
    for (const auto& game : games) {
        for (auto it = std::filesystem::recursive_directory_iterator(
                 game, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (!it->is_regular_file(error)) continue;
            const std::string ext = ascii_lower(it->path().extension());
            const bool dds = ext == ".dds";
            if (!dds && ext != ".png") continue;
            ++index->files;
            const auto name = parse_texture_pack_name(ascii_name(it->path().stem()));
            if (name) found.push_back({it->path(), *name, dds});
        }
    }
    // Deterministic: by path; a DDS wins over a PNG of the same name, else the first path.
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.dds != b.dds) return a.dds;
        return a.path < b.path;
    });
    for (const Found& f : found) {
        if (f.name.level != 0u) continue;
        const std::uint64_t size = size_key(f.name.width, f.name.height, f.name.format);
        auto& map = f.name.any_tlut ? index->any_tlut : index->exact;
        const std::uint64_t key = name_key(size, f.name.hash, f.name.any_tlut ? 0u : f.name.tlut);
        if (map.find(key) != map.end()) continue;
        map.emplace(key, static_cast<std::uint32_t>(index->records.size()));
        index->sizes.insert(size);
        Record record{};
        record.name = f.name;
        record.file = f.path;
        record.dds = f.dds;
        index->records.push_back(std::move(record));
    }
    // Custom lower levels (PNG): _mip<n> beside a PNG base.
    for (const Found& f : found) {
        if (f.name.level == 0u || f.dds) continue;
        const std::uint64_t size = size_key(f.name.width, f.name.height, f.name.format);
        auto& map = f.name.any_tlut ? index->any_tlut : index->exact;
        const auto it = map.find(name_key(size, f.name.hash, f.name.any_tlut ? 0u : f.name.tlut));
        if (it == map.end()) continue;
        Record& record = index->records[it->second];
        if (record.dds) continue;
        if (record.mips.size() <= f.name.level) record.mips.resize(f.name.level + 1u);
        if (record.mips[f.name.level].empty()) record.mips[f.name.level] = f.path;
    }
    index->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return index;
}

bool TexturePacks::may_match(std::uint32_t width, std::uint32_t height, TexFormat format) const noexcept {
    return index_ != nullptr && settings_.enabled &&
           index_->sizes.count(size_key(width, height, static_cast<std::uint32_t>(format))) != 0u;
}

std::uint32_t TexturePacks::find(std::uint32_t width, std::uint32_t height, TexFormat format, std::uint64_t hash,
                                 std::uint64_t tlut) const {
    if (!may_match(width, height, format)) {
        return kNone;
    }
    const std::uint64_t size = size_key(width, height, static_cast<std::uint32_t>(format));
    const auto matches = [&](std::uint32_t slot, bool any) {
        const TexturePackName& name = index_->records[slot].name;
        return name.width == width && name.height == height && name.format == static_cast<std::uint8_t>(format) &&
               name.hash == hash && (any || name.tlut == tlut);
    };
    if (const auto it = index_->exact.find(name_key(size, hash, tlut));
        it != index_->exact.end() && matches(it->second, false)) {
        return it->second;
    }
    if (texture_format_palettized(format)) {
        if (const auto it = index_->any_tlut.find(name_key(size, hash, 0u));
            it != index_->any_tlut.end() && matches(it->second, true)) {
            return it->second;
        }
    }
    return kNone;
}

const TextureReplacement* TexturePacks::resident(std::uint32_t slot) const noexcept {
    return slot < slots_.size() && slots_[slot].state == SlotState::Resident ? &slots_[slot].replacement : nullptr;
}

std::filesystem::path TexturePacks::file(std::uint32_t slot) const {
    return index_ && slot < index_->records.size() ? index_->records[slot].file : std::filesystem::path{};
}

void TexturePacks::add_user(std::uint32_t slot) {
    if (slot >= slots_.size()) return;
    Slot& s = slots_[slot];
    ++s.users;
    s.last_use = frame_;
    if (s.state == SlotState::Idle) {
        request(slot);
    }
}

void TexturePacks::remove_user(std::uint32_t slot) noexcept {
    if (slot >= slots_.size() || slots_[slot].users == 0u) return;
    Slot& s = slots_[slot];
    --s.users;
    s.last_use = frame_;
}

void TexturePacks::request(std::uint32_t slot) {
    if (!index_ || slot >= slots_.size() || slots_[slot].state != SlotState::Idle) return;
    slots_[slot].state = SlotState::Loading;
    ++outstanding_;
    const std::shared_ptr<const Index> index = index_;
    const std::uint64_t generation = generation_;
    submit([this, index, slot, generation] {
        std::unique_ptr<Load> result = load(index->records[slot], slot, generation);
        std::lock_guard<std::mutex> lock(mutex_);
        finished_.push_back(std::move(result));
    });
}

std::unique_ptr<TexturePacks::Load> TexturePacks::load(const Record& record, std::uint32_t slot,
                                                       std::uint64_t generation) const {
    const auto start = std::chrono::steady_clock::now();
    auto result = std::make_unique<Load>();
    result->slot = slot;
    result->generation = generation;
    result->failed = true;
    const auto fail = [&](const char* why) {
        std::cerr << "[texture-pack] skipped " << display(record.file.filename()) << ": " << why << '\n';
        result->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return std::move(result);
    };
    // Source levels: rows of `row_bytes` bytes, `rows` of them, per level.
    std::vector<std::uint8_t> file;
    std::vector<std::vector<RGBA8>> rgba;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::uint32_t width = 0, height = 0, levels = 0;
    DdsLayout dds{};
    if (record.dds) {
        if (!read_file(record.file, file) || !parse_dds(file, dds)) return fail("not a supported DDS");
        width = dds.width;
        height = dds.height;
        format = dds.format;
        levels = dds.levels;
        const bool block = format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM;
        if (block && (width % 4u != 0u || height % 4u != 0u)) return fail("block-compressed size not a multiple of 4");
        if (!block && levels == 1u) {
            // One uncompressed level: as a PNG, with generated levels.
            rgba.resize(1);
            rgba[0].resize(static_cast<std::size_t>(width) * height);
            const std::uint8_t* src = file.data() + dds.level_offset[0];
            for (std::size_t i = 0; i < rgba[0].size(); ++i) {
                const std::uint8_t* p = src + i * 4u;
                rgba[0][i] = format == DXGI_FORMAT_B8G8R8A8_UNORM ? RGBA8{p[2], p[1], p[0], p[3]}
                                                                  : RGBA8{p[0], p[1], p[2], p[3]};
            }
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    } else {
        rgba.resize(1);
        if (!decode_image(thread_wic(), record.file, rgba[0], width, height)) return fail("cannot decode the image");
    }
    if (!rgba.empty()) {
        levels = static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));
        rgba.resize(levels);
        for (std::uint32_t level = 1; level < levels; ++level) {
            const std::uint32_t lw = std::max(1u, width >> level);
            const std::uint32_t lh = std::max(1u, height >> level);
            std::uint32_t mw = 0, mh = 0;
            if (level < record.mips.size() && !record.mips[level].empty() &&
                decode_image(thread_wic(), record.mips[level], rgba[level], mw, mh) && mw == lw && mh == lh) {
                continue;
            }
            detail::generate_rgba8_mip_level(rgba[level - 1u], std::max(1u, width >> (level - 1u)),
                                             std::max(1u, height >> (level - 1u)), rgba[level], lw, lh);
        }
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(levels);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(levels);
    std::vector<UINT> rows(levels);
    std::vector<UINT64> row_sizes(levels);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&desc, 0, levels, 0, footprints.data(), rows.data(), row_sizes.data(), &total);
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &desc);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&texture)))) {
        return fail("cannot create the texture");
    }
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) ||
        FAILED(upload->Map(0, &no_read, &mapped))) {
        return fail("cannot create the upload buffer");
    }
    auto* dst = static_cast<std::uint8_t*>(mapped);
    for (std::uint32_t level = 0; level < levels; ++level) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = footprints[level];
        const std::uint8_t* src = rgba.empty() ? file.data() + dds.level_offset[level]
                                               : reinterpret_cast<const std::uint8_t*>(rgba[level].data());
        const std::size_t src_row = rgba.empty() ? dds.row_bytes[level]
                                                 : static_cast<std::size_t>(std::max(1u, width >> level)) * 4u;
        const std::size_t copy = std::min<std::size_t>(src_row, static_cast<std::size_t>(row_sizes[level]));
        for (UINT row = 0; row < rows[level]; ++row) {
            std::memcpy(dst + fp.Offset + static_cast<std::size_t>(row) * fp.Footprint.RowPitch, src + row * src_row,
                        copy);
        }
    }
    upload->Unmap(0, nullptr);

    result->replacement.texture = std::move(texture);
    result->replacement.format = format;
    result->replacement.width = width;
    result->replacement.height = height;
    result->replacement.levels = levels;
    const double scale = static_cast<double>(width) / static_cast<double>(std::max<std::uint16_t>(record.name.width, 1));
    result->replacement.lod_shift_x16 =
        static_cast<std::uint8_t>(std::clamp(std::lround(16.0 * std::log2(std::max(scale, 1.0))), 0l, 128l));
    result->replacement.bytes = info.SizeInBytes;
    result->upload = std::move(upload);
    result->footprints = std::move(footprints);
    result->failed = false;
    result->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}

void TexturePacks::begin_frame(unsigned frame_slot) {
    if (retired_uploads_.size() <= frame_slot) retired_uploads_.resize(frame_slot + 1u);
    retired_uploads_[frame_slot].clear();
    frame_slot_ = frame_slot;
    ++frame_;
}

void TexturePacks::evict_to_budget(std::uint64_t incoming) {
    const std::uint64_t budget = settings_.budget_bytes;
    if (budget == 0u) return;
    while (resident_bytes_ + incoming > budget) {
        std::uint32_t victim = kNone;
        std::uint64_t oldest = ~0ull;
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            if (s.state == SlotState::Resident && s.users == 0u && s.last_use < oldest) {
                oldest = s.last_use;
                victim = i;
            }
        }
        if (victim == kNone) return;
        Slot& s = slots_[victim];
        resident_bytes_ -= s.replacement.bytes;
        --resident_count_;
        ++evictions_;
        // Entries that bound it were retired with their own reference: the GPU keeps it until their fence.
        s.replacement = {};
        s.state = SlotState::Idle;
    }
}

bool TexturePacks::pump(ID3D12GraphicsCommandList* list, std::vector<std::uint32_t>& ready) {
    ready.clear();
    bool changed = std::exchange(changed_, false);
    std::vector<std::unique_ptr<Load>> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (built_ && built_->generation == generation_) {
            index_ = std::move(built_);
            changed = true;
            clear_slots();
            slots_.resize(index_->records.size());
            preload_start_ns_ = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch() / std::chrono::nanoseconds(1));
            std::cerr << "[texture-pack] index files=" << index_->files << " names=" << index_->records.size()
                      << " ms=" << index_->ms << " folder=" << display(settings_.packs) << '\n';
        }
        built_.reset();
        finished.swap(finished_);
    }
    for (auto& load : finished) {
        if (load->generation != generation_ || load->slot >= slots_.size() ||
            slots_[load->slot].state != SlotState::Loading) {
            continue;
        }
        ++loads_;
        load_ms_ += load->ms;
        Slot& s = slots_[load->slot];
        if (load->failed) {
            ++load_failures_;
            --outstanding_;
            s.state = SlotState::Failed;
            continue;
        }
        s.state = SlotState::Ready;
        ready_queue_.push_back(load->slot);
        s.ready = std::move(load);
    }
    if (index_ && settings_.preload) {
        while (preload_cursor_ < slots_.size() && outstanding_ < kPreloadOutstanding &&
               (settings_.budget_bytes == 0u || resident_bytes_ < settings_.budget_bytes)) {
            request(preload_cursor_++);
        }
    }
    if (list == nullptr) {
        return changed;
    }
    std::uint64_t uploaded = 0;
    while (!ready_queue_.empty() && uploaded < kUploadBytesPerFrame) {
        const std::uint32_t i = ready_queue_.front();
        ready_queue_.pop_front();
        Slot& s = slots_[i];
        if (s.state != SlotState::Ready || !s.ready) continue;
        --outstanding_;
        const std::uint64_t bytes = s.ready->replacement.bytes;
        evict_to_budget(bytes);
        if (settings_.budget_bytes != 0u && resident_bytes_ + bytes > settings_.budget_bytes) {
            // Over the budget with every resident replacement bound: the original stays.
            s.ready.reset();
            s.state = SlotState::Idle;
            ++deferred_;
            continue;
        }
        Load& load = *s.ready;
        for (UINT level = 0; level < load.replacement.levels; ++level) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = load.replacement.texture.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = level;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = load.upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = load.footprints[level];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = load.replacement.texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        if (retired_uploads_.size() <= frame_slot_) retired_uploads_.resize(frame_slot_ + 1u);
        retired_uploads_[frame_slot_].push_back(std::move(load.upload));
        s.replacement = std::move(load.replacement);
        s.ready.reset();
        s.state = SlotState::Resident;
        s.last_use = frame_;
        resident_bytes_ += bytes;
        ++resident_count_;
        ++uploads_;
        upload_bytes_ += bytes;
        uploaded += bytes;
        ready.push_back(i);
    }
    if (settings_.preload && preload_start_ns_ != 0u && outstanding_ == 0u &&
        (preload_cursor_ >= slots_.size() ||
         (settings_.budget_bytes != 0u && resident_bytes_ >= settings_.budget_bytes))) {
        const auto now = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch() / std::chrono::nanoseconds(1));
        std::cerr << "[texture-pack] preloaded resident=" << resident_count_ << " mb=" << (resident_bytes_ >> 20u)
                  << " ms=" << (now - preload_start_ns_) / 1000000u << '\n';
        preload_start_ns_ = 0;
    }
    return changed;
}

void TexturePacks::dump(const TexturePackName& name, std::uint32_t width, std::uint32_t height,
                        const std::uint8_t* rgba) {
    if (!dumping() || rgba == nullptr || width == 0u || height == 0u) return;
    std::string stem = texture_pack_name(name);
    if (!dumped_.insert(stem).second) return;
    const std::filesystem::path folder = settings_.dumps / kGameId;
    std::vector<std::uint8_t> pixels(rgba, rgba + static_cast<std::size_t>(width) * height * 4u);
    submit([this, folder, stem = std::move(stem), width, height, pixels = std::move(pixels)]() mutable {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        const std::filesystem::path path = folder / (stem + ".png");
        if (std::filesystem::exists(path, error)) return;
        // Written beside and renamed: a reader never sees a partial file.
        const std::filesystem::path partial = folder / (stem + ".png.part");
        if (encode_png(thread_wic(), partial, width, height, pixels)) {
            std::filesystem::rename(partial, path, error);
            if (!error) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++dumped_count_;
                return;
            }
        }
        std::filesystem::remove(partial, error);
    });
}

TexturePacks::Stats TexturePacks::stats() const noexcept {
    Stats stats{};
    if (index_) {
        stats.names = index_->records.size();
        stats.files = index_->files;
        stats.index_ms = index_->ms;
    }
    stats.loads = loads_;
    stats.load_failures = load_failures_;
    stats.uploads = uploads_;
    stats.upload_bytes = upload_bytes_;
    stats.resident_bytes = resident_bytes_;
    stats.resident = resident_count_;
    stats.evictions = evictions_;
    stats.deferred = deferred_;
    stats.load_ms = load_ms_;
    std::lock_guard<std::mutex> lock(mutex_);
    stats.dumped = dumped_count_;
    return stats;
}

}  // namespace galaxy::gx
