// SPDX-License-Identifier: GPL-3.0-only
// TextureCache's OpenGL ES side (texture_backend.h).

#include "galaxy/gx/texture_backend.h"

#include <algorithm>
#include <stdexcept>

namespace galaxy::gx {

using namespace gles::gl;

bool TextureBackend::initialize(gpu::Device*, std::uint32_t view_capacity) {
    // The null view: one transparent black texel, sampled where a draw binds no map.
    null_texture_ = gles::Context::get().create_texture(1u, 1u, 1u, GL_RGBA8, false, "null texture");
    if (!null_texture_) {
        return false;
    }
    const std::uint8_t black[4] = {0, 0, 0, 0};
    glBindTexture(GL_TEXTURE_2D, null_texture_->name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glBindTexture(GL_TEXTURE_2D, 0);
    views_.assign(view_capacity + 1u, 0u);
    views_.back() = null_texture_->name;
    return true;
}

void TextureBackend::shutdown() {
    views_.clear();
    null_texture_.reset();
    upload_list_ = false;
}

void TextureBackend::write_view(std::uint32_t index, gles::Texture* texture, unsigned) {
    views_[index] = texture != nullptr ? texture->name : views_.back();
}

TextureBackend::UploadChunk TextureBackend::create_upload_chunk(std::uint64_t size) {
    UploadChunk chunk{};
    chunk.resource = std::shared_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size]);
    if (!chunk.resource) {
        throw std::runtime_error("[TextureCache] texture upload arena allocation failed");
    }
    chunk.cpu_base = chunk.resource.get();
    chunk.size = size;
    return chunk;
}

void TextureBackend::release_upload_chunk(UploadChunk& chunk) noexcept {
    chunk.cpu_base = nullptr;
    chunk.resource.reset();
}

gpu::TextureRef TextureBackend::create_texture(
    std::uint32_t width, std::uint32_t height, unsigned levels, std::uint64_t& allocation_bytes) {
    auto texture = gles::Context::get().create_texture(width, height, levels, GL_RGBA8, false, nullptr);
    if (!texture) {
        throw std::runtime_error("[TextureCache] create texture failed");
    }
    allocation_bytes = texture->allocation_bytes;
    return texture;
}

bool TextureBackend::poolable(gles::Texture* texture, std::uint32_t& width, std::uint32_t& height,
                              std::uint32_t& levels) {
    if (texture->format != GL_RGBA8 || texture->render_target) {
        return false;
    }
    width = texture->width;
    height = texture->height;
    levels = texture->levels;
    return true;
}

bool TextureBackend::alias_compatible(gles::Texture* texture, std::uint32_t& width, std::uint32_t& height) {
    if (texture->format != GL_RGBA8 || texture->width == 0u || texture->width > UINT16_MAX ||
        texture->height == 0u || texture->height > UINT16_MAX) {
        return false;
    }
    width = texture->width;
    height = texture->height;
    return true;
}

void TextureBackend::record_upload(gles::Texture* texture, bool, const UploadChunk& staging, std::uint64_t offset,
                                   std::uint32_t width, std::uint32_t height, unsigned levels,
                                   const std::uint64_t* offsets, const std::uint32_t* pitches) {
    if (texture == nullptr || staging.cpu_base == nullptr) {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const unsigned count = std::min(levels, texture->levels);
    for (unsigned l = 0; l < count; ++l) {
        const std::uint32_t lw = std::max<std::uint32_t>(1u, width >> l);
        const std::uint32_t lh = std::max<std::uint32_t>(1u, height >> l);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(pitches[l] / 4u));
        glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), 0, 0, static_cast<GLsizei>(lw),
                        static_cast<GLsizei>(lh), GL_RGBA, GL_UNSIGNED_BYTE, staging.cpu_base + offset + offsets[l]);
        uploaded_bytes_ += static_cast<std::uint64_t>(lw) * lh * 4u;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace galaxy::gx
