// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Texture packs: higher-resolution replacements for the game's textures, found by the community
// naming convention (a data convention, implemented from its public description):
//   <packs>/<GameID or its 3-letter prefix>/**/tex1_<w>x<h>[_m]_<hash>[_<tlut hash>|_$]_<fmt>
//       [_mip<n>|_arb].png|.dds
// <w>x<h> and <fmt> are the guest texture's size and GX format; <hash> the guest bytes of its base
// level, <tlut hash> the palette entries a C4/C8/C14X2 texture uses ($: any palette). The coordinates
// stay the guest texture's: a replacement is sampled with the same normalized UVs at any size, and its
// sampler LOD range shifts by log2 of the scale (TextureHandle::lod_shift_x16).
//
// TexturePacks owns the pack index (built on a background thread), the loaders (DDS read directly,
// BC1-BC7 or 32-bit RGBA/BGRA; PNG through the Windows Imaging Component; missing levels generated on
// the loader), the GPU-resident replacements (reference-counted by the texture cache's entries,
// unreferenced ones kept in an LRU under a VRAM budget) and the dump writer. Render thread only,
// except where noted.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include "galaxy/gx/gx_bitfields.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace galaxy::gx {

// XXH64 (xxHash specification, github.com/Cyan4973/xxHash doc/xxhash_spec.md).
[[nodiscard]] std::uint64_t xxh64(const void* data, std::size_t size, std::uint64_t seed = 0u) noexcept;

// The naming convention's hashes. Texture: the guest bytes of the base level (block-rounded, as stored).
// Palette: the TLUT entries from the lowest to the highest index the base level uses (2 bytes each, as
// stored in TMEM); 0 for direct formats.
[[nodiscard]] std::uint64_t texture_pack_hash(const std::uint8_t* base_level, std::size_t bytes) noexcept;
[[nodiscard]] std::uint64_t texture_pack_tlut_hash(TexFormat format, const std::uint8_t* base_level,
                                                   std::uint32_t width, std::uint32_t height,
                                                   const std::uint8_t* palette) noexcept;

struct TexturePackName {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t format = 0;
    std::uint8_t level = 0;   // _mip<n>
    bool mipmapped = false;   // _m
    bool any_tlut = false;    // _$
    bool arbitrary = false;   // _arb
    std::uint64_t hash = 0;
    std::uint64_t tlut = 0;
};
// A file name's stem (no folder, no extension); nullopt when it is not a texture name.
[[nodiscard]] std::optional<TexturePackName> parse_texture_pack_name(std::string_view stem) noexcept;
// The stem a texture is dumped under (level 0; `level` adds _mip<n>).
[[nodiscard]] std::string texture_pack_name(const TexturePackName& name);
[[nodiscard]] bool texture_format_palettized(TexFormat format) noexcept;

// A DDS file's layout: levels are contiguous after the header(s).
struct DdsLayout {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    std::size_t data_offset = 0;
    std::array<std::size_t, 16> level_offset{};
    std::array<std::uint32_t, 16> row_bytes{};   // bytes of one row of blocks (or pixels)
    std::array<std::uint32_t, 16> rows{};        // rows of blocks (or pixels)
};
// BC1-BC7 (DXT1/DXT3/DXT5/ATI1/ATI2 or a DX10 header; sRGB forms load as UNORM), DX10 R8G8B8A8 and
// B8G8R8A8, DX9 A8R8G8B8 and A8B8G8R8. False for anything else or a truncated file.
[[nodiscard]] bool parse_dds(std::span<const std::uint8_t> file, DdsLayout& out) noexcept;

struct TexturePackSettings {
    bool enabled = true;
    bool preload = false;
    bool dump = false;
    std::filesystem::path packs;   // the folder holding <GameID> folders
    std::filesystem::path dumps;   // dumps go to <dumps>/<GameID>
    std::uint64_t budget_bytes = 0;
    bool operator==(const TexturePackSettings&) const = default;
};

// A GPU-resident replacement: the cache binds `texture` in place of the decoded guest texture.
struct TextureReplacement {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    std::uint8_t lod_shift_x16 = 0;   // 16 x log2(replacement width / guest width)
    std::uint64_t bytes = 0;
};

class TexturePacks {
public:
    static constexpr std::uint32_t kNone = ~0u;
    static constexpr const char* kGameId = "RMGE01";

    TexturePacks() = default;
    TexturePacks(const TexturePacks&) = delete;
    TexturePacks& operator=(const TexturePacks&) = delete;
    ~TexturePacks() { shutdown(); }

    void initialize(ID3D12Device* device);
    // Applies changed settings: a new folder or enabling rebuilds the index in the background.
    void configure(const TexturePackSettings& settings);
    void shutdown() noexcept;

    [[nodiscard]] bool enabled() const noexcept { return settings_.enabled && index_ != nullptr; }
    [[nodiscard]] bool dumping() const noexcept { return settings_.dump && !settings_.dumps.empty(); }
    // The index has names for this guest size and format (hashing is needed only then).
    [[nodiscard]] bool may_match(std::uint32_t width, std::uint32_t height, TexFormat format) const noexcept;
    // The replacement slot for a texture's names, or kNone.
    [[nodiscard]] std::uint32_t find(std::uint32_t width, std::uint32_t height, TexFormat format,
                                     std::uint64_t hash, std::uint64_t tlut) const;
    // Null until the slot's texture is on the GPU.
    [[nodiscard]] const TextureReplacement* resident(std::uint32_t slot) const noexcept;
    // The file a slot loads from (traces).
    [[nodiscard]] std::filesystem::path file(std::uint32_t slot) const;
    // A cache entry binds (or waits for) the slot: loads it if needed and keeps it resident.
    void add_user(std::uint32_t slot);
    void remove_user(std::uint32_t slot) noexcept;

    // Frame begin (that slot's fence waited): upload buffers of its last use are released.
    void begin_frame(unsigned frame_slot);
    // Once per frame with the frame's open command list: publishes a finished index (returns true:
    // every cached texture must be looked up again) and records the uploads of finished loads within
    // the per-frame byte budget; `ready` receives the slots that became resident.
    bool pump(ID3D12GraphicsCommandList* list, std::vector<std::uint32_t>& ready);

    // Dump mode: writes a texture's base level once as <dumps>/<GameID>/<name>.png (on a loader thread).
    void dump(const TexturePackName& name, std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba);

    struct Stats {
        std::uint64_t names = 0;
        std::uint64_t files = 0;
        std::uint64_t loads = 0;
        std::uint64_t load_failures = 0;
        std::uint64_t uploads = 0;
        std::uint64_t upload_bytes = 0;
        std::uint64_t resident_bytes = 0;
        std::uint64_t resident = 0;
        std::uint64_t evictions = 0;
        std::uint64_t deferred = 0;
        std::uint64_t dumped = 0;
        double index_ms = 0.0;
        double load_ms = 0.0;      // loader thread time of finished loads
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    struct Record {
        TexturePackName name;
        std::filesystem::path file;
        bool dds = false;
        // Custom lower levels (_mip<n>), indexed by level; empty paths where absent.
        std::vector<std::filesystem::path> mips;
    };
    struct Index {
        std::vector<Record> records;
        // (w, h, fmt) -> any names; name key -> record.
        std::unordered_set<std::uint64_t> sizes;
        std::unordered_map<std::uint64_t, std::uint32_t> exact;   // hash ^ mix(tlut), size, fmt
        std::unordered_map<std::uint64_t, std::uint32_t> any_tlut;
        std::uint64_t files = 0;
        double ms = 0.0;
        std::uint64_t generation = 0;
    };
    struct Load {
        std::uint32_t slot = kNone;
        std::uint64_t generation = 0;
        TextureReplacement replacement;
        Microsoft::WRL::ComPtr<ID3D12Resource> upload;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        double ms = 0.0;
        bool failed = false;
    };
    enum class SlotState : std::uint8_t { Idle, Loading, Ready, Resident, Failed };
    struct Slot {
        SlotState state = SlotState::Idle;
        std::uint32_t users = 0;
        std::uint64_t last_use = 0;
        TextureReplacement replacement;
        std::unique_ptr<Load> ready;
    };

    static std::uint64_t size_key(std::uint32_t width, std::uint32_t height, std::uint32_t format) noexcept;
    static std::uint64_t name_key(std::uint64_t size, std::uint64_t hash, std::uint64_t tlut) noexcept;
    static std::shared_ptr<Index> build_index(const std::filesystem::path& root, std::uint64_t generation);
    std::unique_ptr<Load> load(const Record& record, std::uint32_t slot, std::uint64_t generation) const;
    void request(std::uint32_t slot);
    void evict_to_budget(std::uint64_t incoming);
    void clear_slots() noexcept;
    void submit(std::function<void()> job);
    void worker_main();
    void stop_workers() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    TexturePackSettings settings_;
    std::shared_ptr<const Index> index_;
    std::vector<Slot> slots_;
    std::deque<std::uint32_t> ready_queue_;
    std::uint32_t outstanding_ = 0;     // slots loading or waiting for their upload
    std::uint32_t preload_cursor_ = 0;
    std::uint64_t preload_start_ns_ = 0;  // steady clock at the index swap; 0 once the preload is reported
    bool configured_ = false;
    bool changed_ = false;              // the cache must look its textures up again
    std::uint64_t generation_ = 0;
    std::uint64_t frame_ = 0;
    std::uint64_t resident_bytes_ = 0;
    std::uint64_t resident_count_ = 0;
    std::uint64_t uploads_ = 0;
    std::uint64_t upload_bytes_ = 0;
    std::uint64_t evictions_ = 0;
    std::uint64_t deferred_ = 0;
    std::uint64_t loads_ = 0;
    std::uint64_t load_failures_ = 0;
    double load_ms_ = 0.0;
    unsigned frame_slot_ = 0;
    std::vector<std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>> retired_uploads_;
    std::unordered_set<std::string> dumped_;

    // Shared with the loader threads (mutex_).
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::unique_ptr<Load>> finished_;
    std::shared_ptr<Index> built_;
    std::uint64_t dumped_count_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace galaxy::gx
