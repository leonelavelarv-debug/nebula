// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The OpenGL ES context every GLES GX object shares (docs/RG35XXH.md). The window system makes it (host_window.h,
// gles_*); the renderer makes it current on the thread that draws. GL objects named by textures and buffers the GPU may
// still read are deleted once a fence after their last use has passed, as Vulkan's Context defers; each frame's end
// inserts a fence sync with the next fence value. Buffers get GX addresses (gpu::Address) as on Vulkan.

#include "galaxy/gx/gles/gl_api.h"
#include "galaxy/gx/gpu_types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace galaxy::gx::gles {

// gpu::Device: a tag the shared code passes around (the renderer's device() is never null once it started).
struct Device {};

// A 2D texture: immutable storage of `levels` levels. Render textures get a framebuffer on first use.
struct Texture : std::enable_shared_from_this<Texture> {
    GLuint name = 0;
    GLenum format = GL_RGBA8;  // sized internal format
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 1;
    bool render_target = false;
    GLuint framebuffer = 0;  // colour attachment 0 = this texture (RendererGles::framebuffer_for)
    std::uint64_t allocation_bytes = 0;

    Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    ~Texture();
};

// A persistently mapped buffer (EXT_buffer_storage, coherent) with a GX address, or a plain one (mapped == null).
struct Buffer {
    GLuint name = 0;
    std::byte* mapped = nullptr;
    std::uint64_t size = 0;
    std::uint64_t address = 0;

    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer();
};

struct Capabilities {
    int version_major = 3;
    int version_minor = 0;
    bool buffer_storage = false;       // EXT_buffer_storage: persistent mapped rings
    bool vertex_storage_blocks = false;  // SSBOs readable in the vertex stage (the matrix palette)
    bool texture_buffer = false;       // texelFetch on a buffer (the palette's fallback)
    bool timer_query = false;          // EXT_disjoint_timer_query
    bool framebuffer_fetch_arm = false;
    bool framebuffer_fetch_ext = false;
    bool program_binary = false;       // at least one binary format
    bool debug_output = false;
    GLint uniform_offset_alignment = 256;
    GLint storage_offset_alignment = 256;
    GLint texture_buffer_offset_alignment = 256;
    GLint max_uniform_block_bytes = 16384;
    GLint max_texture_size = 4096;
    GLint max_samples = 1;
    GLint max_vertex_texture_units = 0;
    std::string vendor;
    std::string renderer;
    std::string version;
};

class Context {
public:
    static Context& get() noexcept;

    // The context the window system made is current on this thread: loads GL and reads the device's capabilities.
    // `loader` is the window system's GetProcAddress.
    bool initialize(gl::Loader loader);
    void shutdown() noexcept;
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] const Capabilities& caps() const noexcept { return caps_; }
    [[nodiscard]] Device* device() noexcept { return ready_ ? &device_ : nullptr; }

    // The thread GL calls are made on now (set by the renderer when it makes the context current there).
    void set_owner_thread() noexcept { owner_ = std::this_thread::get_id(); }
    void clear_owner_thread() noexcept { owner_ = std::thread::id{}; }
    [[nodiscard]] bool on_owner_thread() const noexcept { return owner_ == std::this_thread::get_id(); }

    // The end of a frame's (or a one-time) commands: a fence sync carrying the next value, flushed to the GPU.
    std::uint64_t submit();
    // The highest value the GPU has passed (polled on the owner thread; others read the last poll).
    [[nodiscard]] std::uint64_t completed_value();
    [[nodiscard]] std::uint64_t submitted_value() const noexcept {
        return submitted_.load(std::memory_order_acquire);
    }
    // Waits on the owner thread until the GPU has passed `value`; false on a timeout.
    bool wait(std::uint64_t value, std::uint64_t timeout_ns = UINT64_MAX);
    // Runs `destroy` (on the owner thread) once the GPU has passed the next submission.
    void defer(std::function<void()> destroy);
    void collect();

    // RGBA8 (or `format`) storage of `levels` levels; `render_target` for textures drawn into.
    [[nodiscard]] std::shared_ptr<Texture> create_texture(std::uint32_t width, std::uint32_t height,
                                                          std::uint32_t levels, GLenum format, bool render_target,
                                                          const char* name);
    // A persistently mapped buffer with a GX address (the upload rings, uniform data, readbacks).
    [[nodiscard]] std::shared_ptr<Buffer> create_buffer(std::uint64_t size, const char* name, bool readable = false);

    static constexpr unsigned kAddressShift = 40;
    struct BufferOffset {
        GLuint buffer = 0;
        std::uint64_t offset = 0;
        Buffer* object = nullptr;
    };
    [[nodiscard]] BufferOffset resolve(std::uint64_t address) const noexcept {
        const std::uint64_t id = address >> kAddressShift;
        if (id == 0u || id >= kAddressSlots) {
            return {};
        }
        Buffer* const buffer = address_slots_[id].load(std::memory_order_acquire);
        return {buffer != nullptr ? buffer->name : 0u, address & ((std::uint64_t{1} << kAddressShift) - 1u), buffer};
    }
    void release_address(std::uint64_t address) noexcept;

    // GL errors since the last check, logged with `where` (GALAXY_GLES_CHECK=1: after every renderer step).
    bool check_errors(const char* where);
    [[nodiscard]] bool checking() const noexcept { return checking_; }

private:
    static constexpr std::size_t kAddressSlots = 4096;
    [[nodiscard]] std::uint64_t register_address(Buffer* buffer);
    struct Pending {
        std::uint64_t value;
        GLsync sync;
    };
    struct Deferred {
        std::uint64_t value;
        std::function<void()> destroy;
    };
    Device device_;
    Capabilities caps_;
    bool ready_ = false;
    bool checking_ = false;
    std::string failure_;
    std::thread::id owner_;
    std::mutex submit_mutex_;
    std::vector<Pending> pending_;
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::mutex deferred_mutex_;
    std::vector<Deferred> deferred_;
    std::array<std::atomic<Buffer*>, kAddressSlots> address_slots_{};
    std::mutex address_mutex_;
    std::vector<std::uint32_t> free_address_ids_;
    std::uint32_t next_address_id_ = 1;
};

// The calling thread's CPU time (nanoseconds).
[[nodiscard]] std::int64_t thread_cpu_ns() noexcept;

}  // namespace galaxy::gx::gles
