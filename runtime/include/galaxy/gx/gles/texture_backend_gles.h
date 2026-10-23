// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// TextureCache's OpenGL ES side (texture_backend.h): texture names by srv_index, CPU staging memory and RGBA8
// textures filled by glTexSubImage2D when the upload is recorded (the GL renderer draws in submission order, and the
// GLES build decodes textures on the render thread, so the staging bytes are complete by then).

#include "galaxy/gx/gles/gles_context.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace galaxy::gx {

class TextureBackend {
public:
    struct UploadChunk {
        std::shared_ptr<std::uint8_t[]> resource;
        std::uint8_t* cpu_base = nullptr;
        std::uint64_t size = 0;
    };
    // D3D12's footprint rules, kept so the shared staging layout is the same.
    static constexpr std::uint64_t kPlacementAlignment = 512u;
    static constexpr std::uint32_t kPitchAlignment = 256u;

    struct HeapStats {
        std::uint64_t heaps = 0;
        std::uint64_t heap_bytes = 0;
        std::uint64_t used_bytes = 0;
        std::uint64_t placed = 0;
        std::uint64_t committed_fallbacks = 0;
    };

    bool initialize(gpu::Device* device, std::uint32_t view_capacity);
    void shutdown();
    [[nodiscard]] bool ready() const noexcept { return !views_.empty(); }

    void write_view(std::uint32_t index, gles::Texture* texture, unsigned levels);
    [[nodiscard]] UploadChunk create_upload_chunk(std::uint64_t size);
    static void release_upload_chunk(UploadChunk& chunk) noexcept;
    [[nodiscard]] gpu::TextureRef create_texture(
        std::uint32_t width, std::uint32_t height, unsigned levels, std::uint64_t& allocation_bytes);
    [[nodiscard]] static bool poolable(gles::Texture* texture, std::uint32_t& width, std::uint32_t& height,
                                       std::uint32_t& levels);
    [[nodiscard]] static bool alias_compatible(gles::Texture* texture, std::uint32_t& width, std::uint32_t& height);
    void record_upload(gles::Texture* texture, bool reused, const UploadChunk& staging, std::uint64_t offset,
                       std::uint32_t width, std::uint32_t height, unsigned levels, const std::uint64_t* offsets,
                       const std::uint32_t* pitches);
    void set_upload_list(bool open) noexcept { upload_list_ = open; }
    [[nodiscard]] bool has_upload_list() const noexcept { return upload_list_; }
    [[nodiscard]] HeapStats heap_stats() const noexcept { return {}; }

    // The renderer binds these names for the draw's texture table.
    [[nodiscard]] GLuint view(std::uint32_t index) const noexcept { return views_[index]; }
    [[nodiscard]] GLuint null_view() const noexcept { return views_.back(); }
    // The textures uploaded since the last take (diagnostics).
    [[nodiscard]] std::uint64_t take_uploaded_bytes() noexcept { return std::exchange(uploaded_bytes_, 0u); }

private:
    std::vector<GLuint> views_;  // view_capacity slots, then the null view
    std::shared_ptr<gles::Texture> null_texture_;
    bool upload_list_ = false;
    std::uint64_t uploaded_bytes_ = 0;
};

}  // namespace galaxy::gx
