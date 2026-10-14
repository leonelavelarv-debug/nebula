// SPDX-License-Identifier: GPL-3.0-only
// Texture packs: hashes, names, DDS layouts, the index, loads on a WARP device, the budget and dumps.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "galaxy/gx/gx_backend.h"
#include "galaxy/gx/texture_pack.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace galaxy::gx;

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::uint64_t hash_of(const char* text) {
    return xxh64(text, std::strlen(text));
}

void test_xxh64() {
    // Reference values: python-xxhash 3.x, xxh64(data, seed=0).
    expect(hash_of("") == 0xef46db3751d8e999ull, "xxh64 of the empty input");
    expect(hash_of("abc") == 0x44bc2cf5ad770999ull, "xxh64 of abc");
    expect(hash_of("Nobody inspects the spammish repetition") == 0xfbcea83c8a378bf1ull,
           "xxh64 of a 39-byte input");
    std::vector<std::uint8_t> bytes(768);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i);
    expect(xxh64(bytes.data(), bytes.size()) == 0x8e03c838c596036full, "xxh64 of 768 bytes (stripes and tail)");
    expect(texture_pack_hash(bytes.data(), bytes.size()) == xxh64(bytes.data(), bytes.size()),
           "texture hash is xxh64 seed 0");
}

void test_names() {
    const char* valid[] = {
        "tex1_256x256_m_123c307fab917b99_1",
        "tex1_200x40_63f3408997969cd6_9a81ceb434acb873_8",
        "tex1_48x51_53403146169829a2_$_9",
        "tex1_36x36_4653193aaa14d048_14_mip2",
        "tex1_64x64_0123456789abcdef_14_arb",
        "tex1_1024x8_ffffffffffffffff_0123456789abcdef_10",
    };
    for (const char* stem : valid) {
        const auto name = parse_texture_pack_name(stem);
        expect(name.has_value(), stem);
        if (name) expect(texture_pack_name(*name) == stem, "name round trip");
    }
    const auto m = parse_texture_pack_name("tex1_256x256_m_123c307fab917b99_1");
    expect(m && m->width == 256u && m->height == 256u && m->mipmapped && m->format == 1u &&
               m->hash == 0x123c307fab917b99ull && m->tlut == 0u && !m->any_tlut,
           "fields of a mipmapped I8 name");
    const auto c4 = parse_texture_pack_name("tex1_200x40_63f3408997969cd6_9a81ceb434acb873_8");
    expect(c4 && c4->format == 8u && c4->tlut == 0x9a81ceb434acb873ull, "palette hash of a C4 name");
    const auto any = parse_texture_pack_name("tex1_48x51_53403146169829a2_$_9");
    expect(any && any->any_tlut && any->width == 48u && any->height == 51u, "palette wildcard");
    const auto mip = parse_texture_pack_name("tex1_36x36_4653193aaa14d048_14_mip2");
    expect(mip && mip->level == 2u && mip->format == 14u, "custom level suffix");
    const char* invalid[] = {
        "tex1_64x64_0123456789abcde_14",                       // 15 hex digits
        "tex1_64x64_0123456789abcdef_7",                       // no GX format 7
        "tex1_64x64_0123456789abcdef_1234567890abcdef_14",     // palette hash on a direct format
        "efb1_n000007_160x120_6",                              // EFB copies are never replaced
        "tex1_0x64_0123456789abcdef_14",
        "tex1_64x64_0123456789abcdef_14_mip0",
        "tex1_64x64_0123456789abcdef_14_extra",
        "tex1_64x64_m",
    };
    for (const char* stem : invalid) {
        expect(!parse_texture_pack_name(stem).has_value(), stem);
    }
}

void test_tlut_hash() {
    // TMEM palette: 256 big-endian entries.
    std::vector<std::uint8_t> palette(512);
    for (std::size_t i = 0; i < palette.size(); ++i) palette[i] = static_cast<std::uint8_t>(i * 7u + 3u);
    // C8 8x4 (one tile): indices 3..7.
    std::vector<std::uint8_t> c8(32, 5u);
    c8[0] = 3u;
    c8[31] = 7u;
    expect(texture_pack_tlut_hash(TexFormat::C8, c8.data(), 8u, 4u, palette.data()) ==
               xxh64(palette.data() + 6u, 10u),
           "C8 palette hash covers indices 3..7");
    // C4 8x8 (one tile): nibbles 1..14.
    std::vector<std::uint8_t> c4(32, 0x55u);
    c4[3] = 0x1Eu;
    expect(texture_pack_tlut_hash(TexFormat::C4, c4.data(), 8u, 8u, palette.data()) ==
               xxh64(palette.data() + 2u, 28u),
           "C4 palette hash covers nibbles 1..14");
    // C14X2 4x4 (one tile): big-endian indices, top two bits ignored.
    std::vector<std::uint8_t> c14(32, 0u);
    for (std::size_t i = 0; i < c14.size(); i += 2u) {
        c14[i] = 0xC0u;   // top bits set
        c14[i + 1u] = 0x10u;
    }
    c14[1] = 0x12u;
    std::vector<std::uint8_t> big(32768);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i ^ (i >> 8u));
    expect(texture_pack_tlut_hash(TexFormat::C14X2, c14.data(), 4u, 4u, big.data()) ==
               xxh64(big.data() + 0x10u * 2u, 3u * 2u),
           "C14X2 palette hash covers indices 0x10..0x12");
    expect(texture_pack_tlut_hash(TexFormat::RGBA8, c8.data(), 4u, 4u, palette.data()) == 0u,
           "direct formats have no palette hash");
}

void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t value) {
    std::memcpy(v.data() + at, &value, 4u);
}

std::vector<std::uint8_t> dds_header(std::uint32_t width, std::uint32_t height, std::uint32_t levels,
                                     std::uint32_t fourcc, std::uint32_t dxgi, std::uint32_t r_mask = 0u) {
    std::vector<std::uint8_t> file(dxgi != 0u ? 148u : 128u, 0u);
    std::memcpy(file.data(), "DDS ", 4u);
    put32(file, 4u, 124u);
    put32(file, 12u, height);
    put32(file, 16u, width);
    put32(file, 28u, levels);
    put32(file, 76u, 32u);
    if (fourcc != 0u) {
        put32(file, 80u, 0x4u);
        put32(file, 84u, fourcc);
    } else {
        put32(file, 80u, 0x41u);
        put32(file, 88u, 32u);
        put32(file, 92u, r_mask);
        put32(file, 104u, 0xFF000000u);
    }
    if (dxgi != 0u) {
        put32(file, 128u, dxgi);
        put32(file, 132u, 3u);
        put32(file, 140u, 1u);
    }
    return file;
}

constexpr std::uint32_t fourcc(const char* code) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(code[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(code[1])) << 8u) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(code[2])) << 16u) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(code[3])) << 24u);
}

void test_dds() {
    // BC7 64x64, 7 levels: 64*64 + 32*32 + 16*16 + 8*8 + 4*4 + 4*4 + 4*4 bytes (16 per 4x4 block).
    std::vector<std::uint8_t> bc7 = dds_header(64u, 64u, 7u, fourcc("DX10"), DXGI_FORMAT_BC7_UNORM_SRGB);
    const std::size_t bc7_bytes = 4096u + 1024u + 256u + 64u + 16u + 16u + 16u;
    bc7.resize(bc7.size() + bc7_bytes);
    DdsLayout layout{};
    expect(parse_dds(bc7, layout), "BC7 DX10 header parses");
    expect(layout.format == DXGI_FORMAT_BC7_UNORM && layout.levels == 7u && layout.width == 64u,
           "BC7 sRGB loads as UNORM with 7 levels");
    expect(layout.data_offset == 148u && layout.level_offset[1] == 148u + 4096u && layout.rows[0] == 16u &&
               layout.row_bytes[0] == 256u && layout.row_bytes[6] == 16u,
           "BC7 level layout");
    // DXT1 truncated after its first level: one level kept.
    std::vector<std::uint8_t> dxt1 = dds_header(32u, 32u, 6u, fourcc("DXT1"), 0u);
    dxt1.resize(dxt1.size() + 512u + 10u);
    expect(parse_dds(dxt1, layout) && layout.format == DXGI_FORMAT_BC1_UNORM && layout.levels == 1u,
           "DXT1 keeps the levels the file holds");
    // DX9 A8R8G8B8 is BGRA in memory.
    std::vector<std::uint8_t> argb = dds_header(4u, 4u, 1u, 0u, 0u, 0x00FF0000u);
    argb.resize(argb.size() + 64u);
    expect(parse_dds(argb, layout) && layout.format == DXGI_FORMAT_B8G8R8A8_UNORM && layout.row_bytes[0] == 16u,
           "A8R8G8B8 loads as B8G8R8A8");
    std::vector<std::uint8_t> r5g6b5 = dds_header(4u, 4u, 1u, 0u, 0u, 0xF800u);
    r5g6b5.resize(r5g6b5.size() + 64u);
    expect(!parse_dds(r5g6b5, layout), "16-bit DDS is not supported");
    expect(!parse_dds(std::vector<std::uint8_t>(100u, 0u), layout), "short file rejected");
}

void test_sampler_shift() {
    TexMode mode{};
    mode.wrap_s = TexWrap::Repeat;
    mode.wrap_t = TexWrap::Repeat;
    mode.mag_filter = TexMagFilter::Linear;
    mode.min_filter = TexMinFilter::Linear;
    mode.min_lod_x16 = 0;
    mode.max_lod_x16 = 0;
    // A 4x replacement of a mip-less texture: levels 0..2 of the replacement (the guest's base size).
    const std::uint32_t key = pack_sampler_key(mode, false, 9u, 32u);
    expect(((key >> 5u) & 0x7u) == static_cast<std::uint32_t>(TexMinFilter::LinearMipLinear),
           "mip-less guest sampler gains a mip filter for a replacement");
    expect(((key >> 16u) & 0xFFu) == 0u && ((key >> 24u) & 0xFFu) == 32u, "LOD range 0..2 for a 4x replacement");
    // Authored mips: the guest range moves by the shift.
    mode.min_filter = TexMinFilter::LinearMipLinear;
    mode.min_lod_x16 = 8;
    mode.max_lod_x16 = 48;
    const std::uint32_t authored = pack_sampler_key(mode, false, 9u, 32u);
    expect(((authored >> 16u) & 0xFFu) == 40u && ((authored >> 24u) & 0xFFu) == 80u,
           "authored LOD range shifts by log2(scale)");
    expect(pack_sampler_key(mode, false, 4u, 0u) == pack_sampler_key(mode, false, 4u),
           "no shift leaves the key unchanged");
    // Near filtering stays near.
    mode.min_filter = TexMinFilter::Near;
    mode.mag_filter = TexMagFilter::Near;
    const std::uint32_t nearest = pack_sampler_key(mode, false, 9u, 16u);
    expect(((nearest >> 5u) & 0x7u) == static_cast<std::uint32_t>(TexMinFilter::NearMipNear),
           "nearest guest sampler keeps nearest filtering");
}

bool write_png(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               std::vector<std::uint8_t> rgba) {
    for (std::size_t i = 0; i + 3u < rgba.size(); i += 4u) std::swap(rgba[i], rgba[i + 2u]);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           format == GUID_WICPixelFormat32bppBGRA &&
           SUCCEEDED(frame->WritePixels(height, width * 4u, width * height * 4u, rgba.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool read_png(const std::filesystem::path& path, std::uint32_t& width, std::uint32_t& height,
              std::vector<std::uint8_t>& rgba) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    rgba.resize(static_cast<std::size_t>(width) * height * 4u);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(rgba.size()), rgba.data()));
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t value = 0;

    bool create() {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        D3D12_COMMAND_QUEUE_DESC qd{};
        return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
               SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
               SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
               SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
               SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(&list))) &&
               SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    // Pumps once with an open list, then runs the list to completion.
    bool pump(TexturePacks& packs, std::vector<std::uint32_t>& ready) {
        packs.begin_frame(0u);
        const bool changed = packs.pump(list.Get(), ready);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++value);
        while (fence->GetCompletedValue() < value) std::this_thread::yield();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        return changed;
    }
};

void test_packs_on_warp() {
    Gpu gpu;
    if (!gpu.create()) {
        std::cerr << "FAILED: WARP device\n";
        ++failures;
        return;
    }
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("nebula-texture-pack-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    const std::filesystem::path game = root / "My Pack" / TexturePacks::kGameId / "sub";
    std::filesystem::create_directories(game);

    // A 32x32 RGBA8 guest texture replaced by a 128x128 PNG.
    std::vector<std::uint8_t> guest(32u * 32u * 4u, 0x5Au);
    TexturePackName png{};
    png.width = 32u;
    png.height = 32u;
    png.format = static_cast<std::uint8_t>(TexFormat::RGBA8);
    png.hash = texture_pack_hash(guest.data(), guest.size());
    std::vector<std::uint8_t> big(128u * 128u * 4u);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 13u);
    expect(write_png(game / (texture_pack_name(png) + ".png"), 128u, 128u, big), "test PNG written");
    {
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint8_t> back;
        expect(read_png(game / (texture_pack_name(png) + ".png"), w, h, back) && back == big,
               "test PNG round trip");
    }
    // A 16x16 CMPR guest texture replaced by a 64x64 BC1 DDS with its mips.
    TexturePackName dds{};
    dds.width = 16u;
    dds.height = 16u;
    dds.format = static_cast<std::uint8_t>(TexFormat::CMPR);
    dds.hash = 0x0123456789abcdefull;
    std::vector<std::uint8_t> bc1 = dds_header(64u, 64u, 7u, fourcc("DXT1"), 0u);
    bc1.resize(bc1.size() + 2048u + 512u + 128u + 32u + 8u + 8u + 8u, 0x11u);
    {
        std::ofstream out(game / (texture_pack_name(dds) + ".dds"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bc1.data()), static_cast<std::streamsize>(bc1.size()));
    }
    // Not a texture name: counted as a file, not indexed.
    { std::ofstream out(game / "readme.png"); out << "x"; }

    TexturePacks packs;
    packs.initialize(gpu.device.Get());
    TexturePackSettings settings{};
    settings.packs = root;
    settings.dumps = root / "dumps";
    settings.budget_bytes = 64ull << 20;
    packs.configure(settings);
    std::vector<std::uint32_t> ready;
    bool indexed = false;
    for (int i = 0; i < 400 && !indexed; ++i) {
        indexed = gpu.pump(packs, ready);
        if (!indexed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(indexed, "index published");
    expect(packs.stats().names == 2u && packs.stats().files == 3u, "two names among three files");
    expect(packs.may_match(32u, 32u, TexFormat::RGBA8) && !packs.may_match(32u, 32u, TexFormat::RGB565),
           "size and format prefilter");
    const std::uint32_t png_slot = packs.find(32u, 32u, TexFormat::RGBA8, png.hash, 0u);
    const std::uint32_t dds_slot = packs.find(16u, 16u, TexFormat::CMPR, dds.hash, 0u);
    expect(png_slot != TexturePacks::kNone && dds_slot != TexturePacks::kNone, "both names found");
    expect(packs.find(32u, 32u, TexFormat::RGBA8, png.hash ^ 1u, 0u) == TexturePacks::kNone, "other hash misses");
    packs.add_user(png_slot);
    packs.add_user(dds_slot);
    std::vector<std::uint32_t> resident;
    for (int i = 0; i < 400 && resident.size() < 2u; ++i) {
        gpu.pump(packs, ready);
        resident.insert(resident.end(), ready.begin(), ready.end());
        if (resident.size() < 2u) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(resident.size() == 2u, "both replacements uploaded");
    const TextureReplacement* a = packs.resident(png_slot);
    const TextureReplacement* b = packs.resident(dds_slot);
    expect(a != nullptr && a->width == 128u && a->levels == 8u && a->format == DXGI_FORMAT_R8G8B8A8_UNORM &&
               a->lod_shift_x16 == 32u,
           "PNG: 128x128 RGBA8, full chain, 4x shift");
    expect(b != nullptr && b->width == 64u && b->levels == 7u && b->format == DXGI_FORMAT_BC1_UNORM &&
               b->lod_shift_x16 == 32u,
           "DDS: BC1 with its 7 levels, 4x shift");
    expect(gpu.device->GetDeviceRemovedReason() == S_OK, "device alive after uploads");

    // Budget: both unbound, then a budget below their sum evicts the older one.
    const std::uint64_t both = packs.stats().resident_bytes;
    packs.remove_user(png_slot);
    gpu.pump(packs, ready);
    packs.remove_user(dds_slot);
    settings.budget_bytes = both - 1u;
    packs.configure(settings);
    expect(packs.stats().evictions == 1u && packs.resident(png_slot) == nullptr && packs.resident(dds_slot) != nullptr,
           "budget evicts the least recently used unbound replacement");

    // Dump: a 4x4 texture written once as a PNG under its name.
    settings.dump = true;
    packs.configure(settings);
    expect(packs.dumping(), "dump mode on");
    TexturePackName dumped{};
    dumped.width = 4u;
    dumped.height = 4u;
    dumped.format = static_cast<std::uint8_t>(TexFormat::IA8);
    dumped.hash = 0xfedcba9876543210ull;
    std::vector<std::uint8_t> pixels(64u);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<std::uint8_t>(i * 4u);
    packs.dump(dumped, 4u, 4u, pixels.data());
    packs.dump(dumped, 4u, 4u, pixels.data());
    const std::filesystem::path file = settings.dumps / TexturePacks::kGameId / (texture_pack_name(dumped) + ".png");
    for (int i = 0; i < 400 && packs.stats().dumped == 0u; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> back;
    expect(packs.stats().dumped == 1u && read_png(file, w, h, back) && w == 4u && h == 4u && back == pixels,
           "dumped PNG holds the texture once");

    // Off: the index goes and lookups stop.
    settings.enabled = false;
    packs.configure(settings);
    expect(gpu.pump(packs, ready) && !packs.enabled() && packs.find(32u, 32u, TexFormat::RGBA8, png.hash, 0u) ==
                                                              TexturePacks::kNone,
           "turning packs off drops the index");
    packs.shutdown();
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    test_xxh64();
    test_names();
    test_tlut_hash();
    test_dds();
    test_sampler_shift();
    test_packs_on_warp();
    if (failures == 0) {
        std::cout << "texture pack tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
