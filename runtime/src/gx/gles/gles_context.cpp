// SPDX-License-Identifier: GPL-3.0-only
// The shared OpenGL ES context (gles_context.h) and the GL entry points (gl_api.h).

#include "galaxy/gx/gles/gles_context.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <stdexcept>

namespace galaxy::gx::gles {

namespace gl {

#define GLES_DEFINE(type, name) type name = nullptr;
GLES_CORE_FUNCTIONS(GLES_DEFINE)
GLES_OPTIONAL_FUNCTIONS(GLES_DEFINE)
#undef GLES_DEFINE

namespace {
std::set<std::string> g_extensions;
}

bool load(Loader loader, std::string& missing) {
    missing.clear();
#define GLES_LOAD_CORE(type, name)                                     \
    name = reinterpret_cast<type>(loader(#name));                      \
    if (name == nullptr && missing.empty()) missing = #name;
#define GLES_LOAD_OPTIONAL(type, name) name = reinterpret_cast<type>(loader(#name));
    GLES_CORE_FUNCTIONS(GLES_LOAD_CORE)
    GLES_OPTIONAL_FUNCTIONS(GLES_LOAD_OPTIONAL)
#undef GLES_LOAD_CORE
#undef GLES_LOAD_OPTIONAL
    if (!missing.empty()) {
        return false;
    }
    g_extensions.clear();
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        if (const GLubyte* name = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))) {
            g_extensions.insert(reinterpret_cast<const char*>(name));
        }
    }
    return true;
}

bool has_extension(const char* name) {
    return g_extensions.contains(name);
}

}  // namespace gl

using namespace gl;

namespace {

const char* string_of(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value != nullptr ? reinterpret_cast<const char*>(value) : "";
}

void GL_APIENTRY debug_callback(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message,
                                const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION_KHR) {
        return;
    }
    static std::atomic<unsigned> reported{0};
    if (reported.fetch_add(1u, std::memory_order_relaxed) < 64u) {
        std::fprintf(stderr, "[gles-debug] %s: %s\n", type == GL_DEBUG_TYPE_ERROR_KHR ? "error" : "note", message);
    }
}

}  // namespace

Texture::~Texture() {
    const GLuint texture = name;
    const GLuint fb = framebuffer;
    if (texture == 0u && fb == 0u) {
        return;
    }
    Context::get().defer([texture, fb] {
        if (fb != 0u) glDeleteFramebuffers(1, &fb);
        if (texture != 0u) glDeleteTextures(1, &texture);
    });
}

Buffer::~Buffer() {
    const GLuint buffer = name;
    const std::uint64_t gx_address = address;
    const bool was_mapped = mapped != nullptr;
    if (buffer == 0u) {
        return;
    }
    Context::get().defer([buffer, gx_address, was_mapped] {
        if (was_mapped) {
            glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }
        glDeleteBuffers(1, &buffer);
        if (gx_address != 0u) {
            Context::get().release_address(gx_address);
        }
    });
}

Context& Context::get() noexcept {
    static Context context;
    return context;
}

bool Context::initialize(Loader loader) {
    if (ready_) {
        return true;
    }
    std::string missing;
    if (!gl::load(loader, missing)) {
        failure_ = "the OpenGL ES driver lacks " + missing + " (OpenGL ES 3.0 is needed)";
        std::fprintf(stderr, "[gles] %s\n", failure_.c_str());
        return false;
    }
    caps_.vendor = string_of(GL_VENDOR);
    caps_.renderer = string_of(GL_RENDERER);
    caps_.version = string_of(GL_VERSION);
    glGetIntegerv(GL_MAJOR_VERSION, &caps_.version_major);
    glGetIntegerv(GL_MINOR_VERSION, &caps_.version_minor);
    const bool es31 = caps_.version_major > 3 || (caps_.version_major == 3 && caps_.version_minor >= 1);
    const bool es32 = caps_.version_major > 3 || (caps_.version_major == 3 && caps_.version_minor >= 2);
    if (!es31) {
        failure_ = "OpenGL ES 3.1 is needed (the driver has " + caps_.version + ")";
        std::fprintf(stderr, "[gles] %s\n", failure_.c_str());
        return false;
    }
    caps_.buffer_storage = glBufferStorageEXT != nullptr && has_extension("GL_EXT_buffer_storage");
    GLint vertex_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vertex_blocks);
    caps_.vertex_storage_blocks = vertex_blocks >= 1;
    if (const char* force = std::getenv("GALAXY_GLES_PALETTE"); force != nullptr && std::strcmp(force, "texture") == 0) {
        caps_.vertex_storage_blocks = false;
    }
    if (glTexBufferRange == nullptr && glTexBufferRangeEXT != nullptr) {
        glTexBufferRange = glTexBufferRangeEXT;
    }
    caps_.texture_buffer = glTexBufferRange != nullptr && (es32 || has_extension("GL_EXT_texture_buffer"));
    caps_.timer_query = glQueryCounterEXT != nullptr && glGetQueryObjectui64vEXT != nullptr &&
        has_extension("GL_EXT_disjoint_timer_query");
    caps_.framebuffer_fetch_arm = has_extension("GL_ARM_shader_framebuffer_fetch");
    caps_.framebuffer_fetch_ext = has_extension("GL_EXT_shader_framebuffer_fetch");
    GLint binary_formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &binary_formats);
    caps_.program_binary = binary_formats > 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &caps_.uniform_offset_alignment);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &caps_.storage_offset_alignment);
    if (caps_.texture_buffer) {
        glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT, &caps_.texture_buffer_offset_alignment);
    }
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &caps_.max_uniform_block_bytes);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &caps_.max_texture_size);
    glGetIntegerv(GL_MAX_SAMPLES, &caps_.max_samples);
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &caps_.max_vertex_texture_units);
    caps_.debug_output = glDebugMessageCallbackKHR != nullptr && has_extension("GL_KHR_debug");
    if (!caps_.buffer_storage) {
        failure_ = "the OpenGL ES driver has no GL_EXT_buffer_storage (persistent buffers)";
        std::fprintf(stderr, "[gles] %s\n", failure_.c_str());
        return false;
    }
    if (!caps_.vertex_storage_blocks && !caps_.texture_buffer) {
        failure_ = "the OpenGL ES driver reads neither storage nor texture buffers in vertex shaders";
        std::fprintf(stderr, "[gles] %s\n", failure_.c_str());
        return false;
    }
    {
        const char* value = std::getenv("GALAXY_GLES_CHECK");
        checking_ = value != nullptr && value[0] == '1';
    }
    if (checking_ && caps_.debug_output) {
        glEnable(GL_DEBUG_OUTPUT_KHR);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
        glDebugMessageCallbackKHR(debug_callback, nullptr);
    }
    set_owner_thread();
    ready_ = true;
    std::fprintf(stderr,
                 "[gles] \"%s\" by \"%s\", %s; palette from %s, UBO align %d, SSBO align %d, max UBO %d, "
                 "framebuffer fetch %s, program binaries %s, timer queries %s\n",
                 caps_.renderer.c_str(), caps_.vendor.c_str(), caps_.version.c_str(),
                 caps_.vertex_storage_blocks ? "a storage buffer" : "a texture buffer", caps_.uniform_offset_alignment,
                 caps_.storage_offset_alignment, caps_.max_uniform_block_bytes,
                 caps_.framebuffer_fetch_arm ? "ARM" : caps_.framebuffer_fetch_ext ? "EXT" : "no",
                 caps_.program_binary ? "yes" : "no", caps_.timer_query ? "yes" : "no");
    return true;
}

void Context::shutdown() noexcept {
    if (!ready_) {
        return;
    }
    if (on_owner_thread()) {
        glFinish();
        for (const Pending& pending : pending_) {
            glDeleteSync(pending.sync);
        }
        pending_.clear();
        completed_.store(submitted_.load());
        collect();
    }
    ready_ = false;
}

std::uint64_t Context::submit() {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    const std::uint64_t value = submitted_.load(std::memory_order_relaxed) + 1u;
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    pending_.push_back({value, sync});
    submitted_.store(value, std::memory_order_release);
    return value;
}

std::uint64_t Context::completed_value() {
    if (!ready_ || !on_owner_thread()) {
        return completed_.load(std::memory_order_acquire);
    }
    std::lock_guard<std::mutex> lock(submit_mutex_);
    std::size_t done = 0;
    for (; done < pending_.size(); ++done) {
        const GLenum status = glClientWaitSync(pending_[done].sync, 0, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
            break;
        }
        completed_.store(pending_[done].value, std::memory_order_release);
        glDeleteSync(pending_[done].sync);
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(done));
    return completed_.load(std::memory_order_acquire);
}

bool Context::wait(std::uint64_t value, std::uint64_t timeout_ns) {
    if (completed_value() >= value) {
        return true;
    }
    if (!on_owner_thread()) {
        // Another thread waits for the owner's polls.
        const auto start = std::chrono::steady_clock::now();
        while (completed_.load(std::memory_order_acquire) < value) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            if (timeout_ns != UINT64_MAX && std::chrono::steady_clock::now() - start >
                                                std::chrono::nanoseconds(timeout_ns)) {
                return false;
            }
        }
        return true;
    }
    std::lock_guard<std::mutex> lock(submit_mutex_);
    while (!pending_.empty() && pending_.front().value <= value) {
        const GLenum status = glClientWaitSync(pending_.front().sync, GL_SYNC_FLUSH_COMMANDS_BIT,
                                               timeout_ns == UINT64_MAX ? GL_TIMEOUT_IGNORED : timeout_ns);
        if (status == GL_TIMEOUT_EXPIRED || status == GL_WAIT_FAILED) {
            return false;
        }
        completed_.store(pending_.front().value, std::memory_order_release);
        glDeleteSync(pending_.front().sync);
        pending_.erase(pending_.begin());
    }
    return completed_.load(std::memory_order_acquire) >= value;
}

void Context::defer(std::function<void()> destroy) {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_.push_back({submitted_.load(std::memory_order_acquire) + 1u, std::move(destroy)});
}

void Context::collect() {
    if (!on_owner_thread()) {
        return;
    }
    const std::uint64_t done = completed_value();
    std::vector<Deferred> due;
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        auto split = std::stable_partition(deferred_.begin(), deferred_.end(),
                                           [done](const Deferred& d) { return d.value > done; });
        due.assign(std::make_move_iterator(split), std::make_move_iterator(deferred_.end()));
        deferred_.erase(split, deferred_.end());
    }
    for (Deferred& d : due) {
        d.destroy();
    }
}

std::shared_ptr<Texture> Context::create_texture(std::uint32_t width, std::uint32_t height, std::uint32_t levels,
                                                 GLenum format, bool render_target, const char*) {
    if (width == 0u || height == 0u) {
        return {};
    }
    auto texture = std::make_shared<Texture>();
    glGenTextures(1, &texture->name);
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(std::max(1u, levels)), format, static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height));
    // Sampler objects decide filtering; these only keep a texture complete when none is bound.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1u ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        // The deleter frees the name.
        return {};
    }
    texture->format = format;
    texture->width = width;
    texture->height = height;
    texture->levels = std::max(1u, levels);
    texture->render_target = render_target;
    const std::uint64_t texel = format == GL_RGBA16F ? 8u : 4u;
    std::uint64_t bytes = 0;
    for (std::uint32_t l = 0; l < texture->levels; ++l) {
        bytes += static_cast<std::uint64_t>(std::max(1u, width >> l)) * std::max(1u, height >> l) * texel;
    }
    texture->allocation_bytes = bytes;
    return texture;
}

std::shared_ptr<Buffer> Context::create_buffer(std::uint64_t size, const char*, bool readable) {
    if (size == 0u) {
        return {};
    }
    auto buffer = std::make_shared<Buffer>();
    glGenBuffers(1, &buffer->name);
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffer->name);
    const GLbitfield access = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT |
        (readable ? GL_MAP_READ_BIT : 0u);
    glBufferStorageEXT(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(size), nullptr,
                       access | (readable ? GL_CLIENT_STORAGE_BIT_EXT : 0u));
    void* mapped = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(size), access);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    if (mapped == nullptr) {
        std::fprintf(stderr, "[gles] a %llu-byte persistent buffer could not be mapped (GL error 0x%x)\n",
                     static_cast<unsigned long long>(size), glGetError());
        return {};
    }
    buffer->mapped = static_cast<std::byte*>(mapped);
    buffer->size = size;
    buffer->address = register_address(buffer.get());
    return buffer;
}

std::uint64_t Context::register_address(Buffer* buffer) {
    std::lock_guard<std::mutex> lock(address_mutex_);
    std::uint32_t id = 0;
    if (!free_address_ids_.empty()) {
        id = free_address_ids_.back();
        free_address_ids_.pop_back();
    } else {
        if (next_address_id_ >= kAddressSlots) {
            throw std::runtime_error("GLES GX address space exhausted");
        }
        id = next_address_id_++;
    }
    address_slots_[id].store(buffer, std::memory_order_release);
    return static_cast<std::uint64_t>(id) << kAddressShift;
}

void Context::release_address(std::uint64_t address) noexcept {
    const std::uint64_t id = address >> kAddressShift;
    if (id == 0u || id >= kAddressSlots) {
        return;
    }
    std::lock_guard<std::mutex> lock(address_mutex_);
    address_slots_[id].store(nullptr, std::memory_order_release);
    free_address_ids_.push_back(static_cast<std::uint32_t>(id));
}

bool Context::check_errors(const char* where) {
    bool clean = true;
    for (int i = 0; i < 8; ++i) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) {
            break;
        }
        clean = false;
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1u, std::memory_order_relaxed) < 64u) {
            std::fprintf(stderr, "[gles] GL error 0x%04x after %s\n", error, where);
        }
    }
    return clean;
}

std::int64_t thread_cpu_ns() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        return 0;
    }
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

}  // namespace galaxy::gx::gles

namespace galaxy::gx::gpu {

TextureRef texture_ref(TextureObject* texture) {
    return texture != nullptr ? texture->shared_from_this() : TextureRef{};
}

Resource* raw(const BufferRef& buffer) noexcept {
    return buffer.get();
}

UploadMemory create_upload_memory(Device*, std::size_t bytes, const char* name) {
    gles::Context& context = gles::Context::get();
    if (!context.ready() || bytes == 0u) {
        return {};
    }
    std::shared_ptr<gles::Buffer> buffer = context.create_buffer(bytes, name);
    if (!buffer) {
        return {};
    }
    UploadMemory out;
    out.cpu = buffer->mapped;
    out.gpu = buffer->address;
    out.buffer = std::move(buffer);
    return out;
}

TextureRef create_render_texture(Device*, std::uint32_t width, std::uint32_t height) {
    return gles::Context::get().create_texture(width, height, 1u, GL_RGBA8, true, "XFB copy");
}

}  // namespace galaxy::gx::gpu
