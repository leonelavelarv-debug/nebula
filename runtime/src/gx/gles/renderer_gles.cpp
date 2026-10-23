// SPDX-License-Identifier: GPL-3.0-only
// The OpenGL ES GX renderer (renderer_gles.h).

#include "galaxy/gx/gles/renderer_gles.h"

#include "galaxy/content_viewport.h"
#include "galaxy/experimental_ultrawide_aspect.h"
#include "galaxy/gx/gles/shader_translate_gles.h"
#include "galaxy/gx/gx_state.h"
#include "galaxy/gx/post_process_settings.h"
#include "galaxy/gx/texture_cache.h"
#include "galaxy/gx/uber_constants.h"
#include "galaxy/native_host.h"
#include "galaxy/platform_time.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace galaxy::gx {

using namespace gles::gl;

namespace {

bool env_flag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return value[0] != '0' && value[0] != 'n' && value[0] != 'N';
}

std::size_t env_megabytes(const char* name, std::size_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    const unsigned long mb = std::strtoul(value, nullptr, 10);
    return mb != 0u ? static_cast<std::size_t>(mb) << 20u : fallback;
}

std::int64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void write_ppm(const std::string& path, const std::vector<std::uint8_t>& rgba, unsigned width, unsigned height,
               bool bottom_up) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    out << "P6\n" << width << ' ' << height << "\n255\n";
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3u);
    for (unsigned y = 0; y < height; ++y) {
        const unsigned source = bottom_up ? height - 1u - y : y;
        const std::uint8_t* pixels = rgba.data() + static_cast<std::size_t>(source) * width * 4u;
        for (unsigned x = 0; x < width; ++x) {
            row[x * 3u] = pixels[x * 4u];
            row[x * 3u + 1u] = pixels[x * 4u + 1u];
            row[x * 3u + 2u] = pixels[x * 4u + 2u];
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
}

// GxVertexOut (vertex_loader.h) at the shaders' semantic locations (vk::compile_hlsl).
struct GlesVertexAttribute {
    GLuint location;
    GLint size;
    GLenum type;
    bool integer;
    GLuint offset;
};
constexpr GlesVertexAttribute kVertexAttributes[] = {
    {0, 3, GL_FLOAT, false, 0},   {1, 3, GL_FLOAT, false, 12},  {2, 3, GL_FLOAT, false, 24},
    {3, 3, GL_FLOAT, false, 36},  {4, 1, GL_UNSIGNED_INT, true, 48}, {5, 1, GL_UNSIGNED_INT, true, 52},
    {6, 2, GL_FLOAT, false, 56},  {7, 2, GL_FLOAT, false, 64},  {8, 2, GL_FLOAT, false, 72},
    {9, 2, GL_FLOAT, false, 80},  {10, 2, GL_FLOAT, false, 88}, {11, 2, GL_FLOAT, false, 96},
    {12, 2, GL_FLOAT, false, 104}, {13, 2, GL_FLOAT, false, 112}, {14, 4, GL_UNSIGNED_INT, true, 120},
};

constexpr std::uint64_t kPaletteBytes = GxState::kMatrixPaletteWords * sizeof(std::uint32_t);

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Start and end
// ---------------------------------------------------------------------------------------------------------------------

bool RendererGles::initialize(int window_width, int window_height, unsigned efb_scale) {
    efb_scale_ = std::clamp(efb_scale, 1u, kMaxEfbScale);
    frame_slot_ = 0;
    frame_index_ = 0;
    const host_window::OpenedWindow opened = host_window::open_game_window(window_owner_, window_width, window_height);
    if (opened.window == nullptr) {
        return false;
    }
    window_ = opened.window;
    window_shown_ = opened.shown;
    if (!host_window::gles_create_context(window_)) {
        std::cerr << "[Renderer] no OpenGL ES 3 context for the window\n";
        return false;
    }
    gles::Context& context = gles::Context::get();
    if (!context.initialize(&host_window::gles_proc_address)) {
        return false;
    }
    current_ = true;
    const gles::Capabilities& caps = context.caps();
    gx_layout_.palette_storage_block = caps.vertex_storage_blocks;
    gx_layout_.framebuffer_fetch = caps.framebuffer_fetch_arm || caps.framebuffer_fetch_ext;
    if (glVertexAttribFormat == nullptr || glVertexAttribIFormat == nullptr || glVertexAttribBinding == nullptr ||
        glBindVertexBuffer == nullptr) {
        std::cerr << "[Renderer] the driver lacks the OpenGL ES 3.1 vertex attribute bindings\n";
        return false;
    }
    {
        const char* interval = std::getenv("GALAXY_GLES_SWAP_INTERVAL");
        host_window::gles_set_swap_interval(interval != nullptr ? std::atoi(interval) : 1);
    }
    try {
        const unsigned largest = static_cast<unsigned>(std::max(caps.max_texture_size, 1024));
        while (efb_scale_ > 1u && kEfbWidth * efb_scale_ > largest) {
            --efb_scale_;
        }
        efb_scale_ = std::min(efb_scale_, host_window::display_efb_scale_cap());
        // GALAXY_GLES_EFB_DIVISOR=2: the EFB drawn at half the Wii's resolution (320x264), a quarter of the pixels
        // (the Mali-G31's fill rate); copies, peeks and the picture keep the Wii's sizes.
        {
            const char* divisor = std::getenv("GALAXY_GLES_EFB_DIVISOR");
            efb_divisor_ = divisor != nullptr && efb_scale_ == 1u ? std::clamp<unsigned>(
                static_cast<unsigned>(std::strtoul(divisor, nullptr, 10)), 1u, 4u) : 1u;
        }
        if (!create_efb(efb_scale_) || !create_utility_programs()) {
            shutdown();
            return false;
        }
        // The draws' vertex layout: GxVertexOut at binding 0, every location's format set once.
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        for (const GlesVertexAttribute& attribute : kVertexAttributes) {
            if (attribute.integer) {
                glVertexAttribIFormat(attribute.location, attribute.size, attribute.type, attribute.offset);
            } else {
                glVertexAttribFormat(attribute.location, attribute.size, attribute.type, GL_FALSE, attribute.offset);
            }
            glVertexAttribBinding(attribute.location, 0);
        }
        glBindVertexArray(0);
        glGenVertexArrays(1, &empty_vao_);
        glGenSamplers(1, &linear_clamp_);
        glGenSamplers(1, &point_clamp_);
        for (const GLuint sampler : {linear_clamp_, point_clamp_}) {
            const GLint filter = sampler == linear_clamp_ ? GL_LINEAR : GL_NEAREST;
            glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, filter);
            glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, filter);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glGenFramebuffers(1, &read_framebuffer_);
        if (!gx_layout_.palette_storage_block) {
            glGenTextures(1, &palette_texture_);
        }
        // Ring sizes (GALAXY_GLES_RING_*_MB): Vulkan's defaults scaled to a handheld's memory; the peaks are logged.
        gpu::Device* const dev = context.device();
        if (!vertex_ring_.initialize(dev, "vertex", env_megabytes("GALAXY_GLES_RING_VERTEX_MB", 28u << 20u),
                                     kFramesInFlight) ||
            !index_ring_.initialize(dev, "index", env_megabytes("GALAXY_GLES_RING_INDEX_MB", 6u << 20u),
                                    kFramesInFlight) ||
            !constant_ring_.initialize(dev, "constant", env_megabytes("GALAXY_GLES_RING_CONSTANT_MB", 6u << 20u),
                                       kFramesInFlight) ||
            !matrix_ring_.initialize(dev, "matrix", env_megabytes("GALAXY_GLES_RING_MATRIX_MB", 8u << 20u),
                                     kFramesInFlight)) {
            std::cerr << "[Renderer] upload rings unavailable\n";
            shutdown();
            return false;
        }
        timer_queries_ = caps.timer_query && glGenQueries != nullptr && glBeginQuery != nullptr &&
            glGetQueryObjectui64vEXT != nullptr && glGetQueryObjectuiv != nullptr;
        refresh_default_sampler_table();
        host_window::gles_drawable_size(window_, drawable_width_, drawable_height_);
        display_refresh_hz_ = host_window::display_refresh_hz(window_);
        set_post_process_status([] {
            PostProcessStatus off{};
            std::snprintf(off.skip_reason.data(), off.skip_reason.size(), "GLES");
            return off;
        }());
    } catch (const std::exception& error) {
        std::cerr << "[Renderer] OpenGL ES initialize failed: " << error.what() << '\n';
        shutdown();
        return false;
    }
    context.check_errors("initialize");
    std::cout << "[Renderer] OpenGL ES EFB=" << efb_width() << 'x' << efb_height()
              << " drawable=" << drawable_width_ << 'x' << drawable_height_ << '\n';
    return true;
}

bool RendererGles::make_current() {
    gles::Context& context = gles::Context::get();
    if (context.on_owner_thread()) {
        return true;
    }
    if (!host_window::gles_make_current(window_, true)) {
        return false;
    }
    context.set_owner_thread();
    bound_ = Bound{};
    enabled_attributes_ = ~0u;  // unknown on this thread: set again at the next draw
    return true;
}

void RendererGles::shutdown() {
    gles::Context& context = gles::Context::get();
    const bool usable = context.ready() && make_current();
    if (usable) {
        glFinish();
    }
    efb_copy_dests_.clear();
    for (Frame& frame : frames_) {
        frame.retired.clear();
    }
    peek_target_.reset();
    field_target_.reset();
    for (AsyncField& field : async_fields_) {
        field.target.reset();
    }
    vertex_ring_.reset();
    index_ring_.reset();
    constant_ring_.reset();
    matrix_ring_.reset();
    if (usable) {
        for (Readback* readback : {&peek_readback_}) {
            if (readback->buffer != 0u) glDeleteBuffers(1, &readback->buffer);
        }
        for (Readback& readback : pointer_depth_ring_) {
            if (readback.buffer != 0u) glDeleteBuffers(1, &readback.buffer);
        }
        for (AsyncField& field : async_fields_) {
            for (Readback& readback : field.readbacks) {
                if (readback.buffer != 0u) glDeleteBuffers(1, &readback.buffer);
            }
        }
        for (auto& [key, sampler] : samplers_) {
            glDeleteSamplers(1, &sampler);
        }
        if (linear_clamp_ != 0u) glDeleteSamplers(1, &linear_clamp_);
        if (point_clamp_ != 0u) glDeleteSamplers(1, &point_clamp_);
        if (vao_ != 0u) glDeleteVertexArrays(1, &vao_);
        if (empty_vao_ != 0u) glDeleteVertexArrays(1, &empty_vao_);
        if (read_framebuffer_ != 0u) glDeleteFramebuffers(1, &read_framebuffer_);
        if (palette_texture_ != 0u) glDeleteTextures(1, &palette_texture_);
        if (conversion_program_ != 0u) glDeleteProgram(conversion_program_);
        if (xfb_program_ != 0u) glDeleteProgram(xfb_program_);
        xfb_program_ = 0u;
        last_xfb_.reset();
        if (glDeleteQueries != nullptr) {
            if (gpu_part_ != kGpuNone) {
                glEndQuery(GL_TIME_ELAPSED_EXT);
                gpu_part_ = kGpuNone;
            }
            for (GpuSegments& segments : gpu_segments_) {
                if (!segments.queries.empty()) {
                    glDeleteQueries(static_cast<GLsizei>(segments.queries.size()), segments.queries.data());
                }
                segments = GpuSegments{};
            }
        }
        destroy_efb();
        context.collect();
        context.shutdown();
    }
    samplers_.clear();
    sampler_tables_.clear();
    sampler_table_ids_.clear();
    linear_clamp_ = point_clamp_ = vao_ = empty_vao_ = read_framebuffer_ = palette_texture_ = conversion_program_ = 0u;
    timer_queries_ = false;
    if (window_ != nullptr) {
        host_window::gles_destroy_context(window_);
        host_window::close_game_window(window_owner_);
        window_ = nullptr;
    }
}

void RendererGles::drain_gpu() {
    if (make_current()) {
        glFinish();
        gles::Context::get().completed_value();
        gles::Context::get().collect();
    }
}

bool RendererGles::quit_requested() const noexcept {
    return host_window::quit_request.load(std::memory_order_acquire);
}

UploadBuffer RendererGles::create_upload_buffer(std::size_t bytes, const char* name) {
    return gpu::create_upload_memory(device(), bytes, name);
}

std::uint64_t RendererGles::completed_fence_value() const {
    return gles::Context::get().completed_value();
}

void RendererGles::wait_for_fence_value(std::uint64_t fence_value) {
    if (fence_value == 0u) {
        throw std::runtime_error("invalid frame-specific GPU fence wait");
    }
    if (!gles::Context::get().wait(fence_value, 10'000'000'000ull)) {
        throw std::runtime_error("frame-specific GPU completion wait timed out");
    }
}

bool RendererGles::vblank_grid(std::int64_t& vblank, std::int64_t& period) const noexcept {
    return host_window::vblank_grid(window_, vblank, period);
}

// ---------------------------------------------------------------------------------------------------------------------
// The EFB and the renderer's own programs
// ---------------------------------------------------------------------------------------------------------------------

bool RendererGles::create_efb(unsigned scale) {
    gles::Context& context = gles::Context::get();
    (void)scale;
    const unsigned w = efb_width();
    const unsigned h = efb_height();
    efb_color_ = context.create_texture(w, h, 1u, GL_RGBA8, true, "EFB colour");
    efb_depth_ = context.create_texture(w, h, 1u, GL_DEPTH_COMPONENT32F, true, "EFB depth");
    if (!efb_color_ || !efb_depth_) {
        std::cerr << "[Renderer] EFB targets " << w << 'x' << h << " unavailable\n";
        return false;
    }
    glGenFramebuffers(1, &efb_framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, efb_framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, efb_color_->name, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, efb_depth_->name, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[Renderer] EFB framebuffer incomplete (0x" << std::hex << status << std::dec << ")\n";
        return false;
    }
    // Black, farthest (reversed-Z: 0).
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    bound_ = Bound{};
    bound_.framebuffer = efb_framebuffer_;
    return true;
}

void RendererGles::destroy_efb() noexcept {
    if (efb_framebuffer_ != 0u) {
        glDeleteFramebuffers(1, &efb_framebuffer_);
        efb_framebuffer_ = 0u;
    }
    efb_color_.reset();
    efb_depth_.reset();
}

GLuint RendererGles::build_utility_program(std::string_view vs_hlsl, std::string_view ps_hlsl, const char* what) {
    gles::TranslateOptions options;
    std::string error;
    const std::string vs = gles::translate(vs_hlsl, gles::Stage::Vertex, gles::ShaderKind::Utility, options, error);
    if (vs.empty()) {
        std::cerr << "[Renderer] " << what << " vertex shader: " << error << '\n';
        return 0u;
    }
    const std::string ps = gles::translate(ps_hlsl, gles::Stage::Fragment, gles::ShaderKind::Utility, options, error);
    if (ps.empty()) {
        std::cerr << "[Renderer] " << what << " pixel shader: " << error << '\n';
        return 0u;
    }
    const auto compile = [&](GLenum stage, const std::string& source) -> GLuint {
        const GLuint shader = glCreateShader(stage);
        const char* text = source.c_str();
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[2048]{};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::cerr << "[Renderer] " << what << " compile: " << log << "\n" << source << '\n';
            glDeleteShader(shader);
            return 0u;
        }
        return shader;
    };
    const GLuint v = compile(GL_VERTEX_SHADER, vs);
    const GLuint p = v != 0u ? compile(GL_FRAGMENT_SHADER, ps) : 0u;
    if (v == 0u || p == 0u) {
        if (v != 0u) glDeleteShader(v);
        return 0u;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, p);
    glLinkProgram(program);
    glDeleteShader(v);
    glDeleteShader(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[2048]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::cerr << "[Renderer] " << what << " link: " << log << '\n';
        glDeleteProgram(program);
        return 0u;
    }
    return program;
}

bool RendererGles::create_utility_programs() {
    conversion_program_ =
        build_utility_program(blit_vertex_shader_source(), efb_conversion_shader_source(), "EFB conversion");
    // A divided EFB reaches the display copy at a fraction of its size: that copy's upscale (bilinear) gets a
    // contrast-adaptive sharpen in the same pass (GALAXY_GLES_SHARPEN, 0 off; four more bilinear taps a pixel,
    // clamped to their range so edges get no halos).
    xfb_program_ = 0u;
    const char* sharpen_env = std::getenv("GALAXY_GLES_SHARPEN");
    const float sharpen = std::clamp(sharpen_env != nullptr ? static_cast<float>(std::atof(sharpen_env)) : 0.5f,
                                     0.0f, 2.0f);
    if (conversion_program_ != 0u && efb_divisor_ > 1u && sharpen > 0.0f) {
        std::string source(efb_conversion_shader_source());
        const std::string tap = "tex_sample = srctex.SampleLevel(s_lin, sample_pos * inv, 0);";
        const std::size_t at = source.find(tap);
        if (at != std::string::npos) {
            char strength[32];
            std::snprintf(strength, sizeof(strength), "%.3f", static_cast<double>(sharpen));
            const std::string sharpened = tap + R"HLSL(
        {
            const float4 a = srctex.SampleLevel(s_lin, (sample_pos + float2(-0.5, -0.5)) * inv, 0);
            const float4 b = srctex.SampleLevel(s_lin, (sample_pos + float2( 0.5, -0.5)) * inv, 0);
            const float4 c = srctex.SampleLevel(s_lin, (sample_pos + float2(-0.5,  0.5)) * inv, 0);
            const float4 d = srctex.SampleLevel(s_lin, (sample_pos + float2( 0.5,  0.5)) * inv, 0);
            const float3 lo = min(min(min(a.rgb, b.rgb), min(c.rgb, d.rgb)), tex_sample.rgb);
            const float3 hi = max(max(max(a.rgb, b.rgb), max(c.rgb, d.rgb)), tex_sample.rgb);
            const float3 blur = (a.rgb + b.rgb + c.rgb + d.rgb) * 0.25;
            tex_sample.rgb = clamp(tex_sample.rgb + )HLSL" + std::string(strength) +
                R"HLSL( * (tex_sample.rgb - blur), lo, hi);
        })HLSL";
            source.replace(at, tap.size(), sharpened);
            xfb_program_ = build_utility_program(blit_vertex_shader_source(), source, "display copy (sharpened)");
            if (xfb_program_ != 0u) {
                std::fprintf(stderr, "[gles] display copy sharpened (strength %.2f)\n", static_cast<double>(sharpen));
            }
        }
    }
    return conversion_program_ != 0u;
}

GLuint RendererGles::framebuffer_for(gles::Texture& texture) {
    if (texture.framebuffer == 0u) {
        glGenFramebuffers(1, &texture.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, texture.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.name, 0);
        bound_.framebuffer = texture.framebuffer;
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[Renderer] copy framebuffer incomplete (0x" << std::hex << status << std::dec << ")\n";
        }
    }
    return texture.framebuffer;
}

std::array<int, 4> RendererGles::physical_rect(const std::array<int, 4>& rect) const noexcept {
    if (efb_divisor_ == 1u) {
        return rect;
    }
    const int d = static_cast<int>(efb_divisor_);
    const int left = rect[0] / d;
    const int top = rect[1] / d;
    const int right = (rect[0] + rect[2] + d - 1) / d;
    const int bottom = (rect[1] + rect[3] + d - 1) / d;
    return {left, top, right - left, bottom - top};
}

// The conversion shader's EFB coordinates (src_xform, the copy's row clamps and filter offset; a peek's texel and block)
// in the divided EFB's texels.
void RendererGles::to_physical(EfbConversionShaderConstants& c, bool peek) const noexcept {
    if (efb_divisor_ == 1u) {
        return;
    }
    const float d = static_cast<float>(efb_divisor_);
    auto& v = c.values;
    if (peek) {
        v[0] = std::floor(v[0] / d);
        v[1] = std::floor(v[1] / d);
        v[2] /= d;
        v[3] /= d;
        v[11] /= d;
        v[12] /= d;
        v[14] /= d;
        if (v[15] >= 0.0f) {
            v[15] = 1.0f;  // one texel holds the whole logical pixel
        }
        return;
    }
    v[0] /= d;
    v[1] /= d;
    v[2] /= d;
    v[3] /= d;
    v[11] /= d;
    v[12] /= d;
    v[14] /= d;
}

void RendererGles::bind_efb_framebuffer() {
    if (bound_.framebuffer != efb_framebuffer_) {
        glBindFramebuffer(GL_FRAMEBUFFER, efb_framebuffer_);
        bound_.framebuffer = efb_framebuffer_;
        bound_.viewport_valid = false;
    }
}

void RendererGles::reset_gx_bindings() noexcept {
    const GLuint framebuffer = bound_.framebuffer;
    bound_ = Bound{};
    bound_.framebuffer = framebuffer;
}

void RendererGles::utility_state(bool depth_write, bool write_rgb, bool write_alpha) {
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    if (depth_write) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(depth_write ? GL_TRUE : GL_FALSE);
    glColorMask(write_rgb, write_rgb, write_rgb, write_alpha);
    bound_.state_valid = false;
    bound_.scissor_valid = false;
    bound_.scissor_input_valid = false;
    bound_.viewport_valid = false;
    bound_.program = ~0u;
    bound_.pipeline = nullptr;
}

void RendererGles::bind_utility_constants(const void* data, std::size_t bytes) {
    const std::size_t alignment =
        static_cast<std::size_t>(std::max(gles::Context::get().caps().uniform_offset_alignment, 16));
    const UploadRing::Allocation allocation = constant_ring_.allocate(std::max<std::size_t>(bytes, 64u), alignment);
    std::memcpy(allocation.cpu, data, bytes);
    const gles::Context::BufferOffset resolved = gles::Context::get().resolve(allocation.gpu);
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, resolved.buffer, static_cast<GLintptr>(resolved.offset),
                      static_cast<GLsizeiptr>(std::max<std::size_t>(bytes, 64u)));
    bound_.constants_valid = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------------------------------------------------

void RendererGles::gpu_part(int part) {
    if (!timer_queries_ || part == gpu_part_ || (!gpu_in_frame_ && part != kGpuNone)) {
        return;
    }
    GpuSegments& segments = gpu_segments_[frame_slot_];
    static constexpr std::size_t kMaxSegments = 96u;
    if (gpu_part_ != kGpuNone) {
        if (part != kGpuNone && segments.used >= kMaxSegments) {
            return;  // the rest of the frame counts as the running part
        }
        glEndQuery(GL_TIME_ELAPSED_EXT);
        gpu_part_ = kGpuNone;
    }
    if (part == kGpuNone) {
        return;
    }
    if (segments.used == segments.queries.size()) {
        GLuint query = 0;
        glGenQueries(1, &query);
        if (query == 0u) {
            return;
        }
        segments.queries.push_back(query);
        segments.parts.push_back(0u);
    }
    glBeginQuery(GL_TIME_ELAPSED_EXT, segments.queries[segments.used]);
    segments.parts[segments.used] = static_cast<std::uint8_t>(part);
    ++segments.used;
    gpu_part_ = part;
}

void RendererGles::read_gpu_parts(GpuSegments& segments) {
    if (segments.pending && segments.used != 0u) {
        GLuint available = GL_FALSE;
        glGetQueryObjectuiv(segments.queries[segments.used - 1u], GL_QUERY_RESULT_AVAILABLE, &available);
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        if (available == GL_TRUE && disjoint == 0) {
            std::array<double, 5> parts{};
            double total = 0.0;
            for (std::size_t i = 0; i < segments.used; ++i) {
                GLuint64 ns = 0;
                glGetQueryObjectui64vEXT(segments.queries[i], GL_QUERY_RESULT, &ns);
                const double ms = ns < 1'000'000'000ull ? static_cast<double>(ns) / 1e6 : 0.0;
                parts[segments.parts[i]] += ms;
                total += ms;
            }
            for (std::size_t i = 0; i < parts.size(); ++i) {
                window_stats_.gpu_part_ms[i] += parts[i];
            }
            window_stats_.gpu_ms += total;
            window_stats_.gpu_max_ms = std::max(window_stats_.gpu_max_ms, total);
            ++window_stats_.gpu_frames;
        }
    }
    segments.used = 0u;
    segments.pending = false;
}

void RendererGles::begin_frame(bool) {
    if (!make_current()) {
        throw std::runtime_error("the OpenGL ES context could not be made current on the render thread");
    }
    gles::Context& context = gles::Context::get();
    frame_slot_ = static_cast<unsigned>(frame_index_ % kFramesInFlight);
    Frame& frame = frames_[frame_slot_];
    const auto wait_start = std::chrono::steady_clock::now();
    if (frame.fence != 0u) {
        wait_for_fence_value(frame.fence);
    }
    const auto waited = std::chrono::steady_clock::now() - wait_start;
    last_frame_slot_wait_us_ =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(waited).count());
    window_stats_.frame_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
    read_gpu_parts(gpu_segments_[frame_slot_]);
    context.collect();
    frame.retired.clear();
    refresh_default_sampler_table();
    vertex_ring_.begin_frame(frame_slot_);
    index_ring_.begin_frame(frame_slot_);
    constant_ring_.begin_frame(frame_slot_);
    matrix_ring_.begin_frame(frame_slot_);
    // This slot's texture tables are free again (its last frame's draws have finished).
    if (texture_tables_.size() < kFramesInFlight) {
        texture_tables_.resize(kFramesInFlight);
    }
    texture_tables_[frame_slot_].clear();
    reset_gx_bindings();
    bound_.framebuffer = ~0u;
    bind_efb_framebuffer();
    gpu_in_frame_ = true;
    gpu_part(kGpuScene);
    {
        const std::uint64_t resize = host_window::pending_resize_extent.exchange(0u, std::memory_order_acq_rel);
        if (resize != 0u) {
            host_window::gles_drawable_size(window_, drawable_width_, drawable_height_);
        }
    }
    if (context.checking()) {
        context.check_errors("begin_frame");
    }
}

std::uint64_t RendererGles::end_frame(bool present_swap_chain, SubmissionCallback on_submitted,
                                      void* submission_context) {
    gles::Context& context = gles::Context::get();
    gpu_part(kGpuNone);
    gpu_in_frame_ = false;
    gpu_segments_[frame_slot_].pending = gpu_segments_[frame_slot_].used != 0u;
    const std::uint64_t fence = context.submit();
    frames_[frame_slot_].fence = fence;
    if (pointer_depth_in_frame_) {
        pointer_depth_fence_ = fence;
        pointer_depth_in_frame_ = false;
    }
    for (AsyncField& field : async_fields_) {
        if (field.fence == kAsyncPeekInFrame) {
            field.fence = fence;
            field.readbacks[field.pending_slot].fence = fence;
        }
    }
    if (on_submitted != nullptr) {
        on_submitted(submission_context, fence);
    }
    if (present_swap_chain && presented_this_frame_) {
        const std::int64_t start = monotonic_ns();
        host_window::gles_swap_buffers(window_);
        const std::int64_t end = monotonic_ns();
        window_stats_.present_wait_ns += end - start;
        if (!window_shown_) {
            host_window::show_window(window_);
            window_shown_ = true;
        }
        // Presents a second, for the FRAME RATE row.
        if (present_window_start_ns_ == 0) {
            present_window_start_ns_ = end;
        }
        ++present_window_count_;
        if (end - present_window_start_ns_ >= 1'000'000'000) {
            const double hz = static_cast<double>(present_window_count_) * 1e9 /
                static_cast<double>(end - present_window_start_ns_);
            measured_present_hz_.store(hz, std::memory_order_relaxed);
            host_window::measured_present_hz.store(hz, std::memory_order_relaxed);
            present_window_start_ns_ = end;
            present_window_count_ = 0u;
        }
    }
    presented_this_frame_ = false;
    ++frame_index_;
    if (frame_index_ % 600u == 0u) {
        report_stats();
    }
    if (context.checking()) {
        context.check_errors("end_frame");
    }
    return fence;
}

void RendererGles::report_stats() {
    static std::int64_t last_cpu = 0;
    static auto last_time = std::chrono::steady_clock::now();
    const std::int64_t cpu = gles::thread_cpu_ns();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last_time).count();
    if (last_cpu != 0 && seconds > 0.0) {
        const double frames = 600.0;
        const auto gpu_part_average = [this](std::size_t part) {
            return window_stats_.gpu_frames != 0u ? window_stats_.gpu_part_ms[part] / window_stats_.gpu_frames : 0.0;
        };
        std::fprintf(stderr,
                     "[gles] %.1f frames/s; render thread %.1f ms/s (swap waits %.1f, frame-slot waits %.1f ms/s, readbacks %.1f ms/s); a frame: %.0f draws, "
                     "%.1f copies, %.1f clears, %.1f peeks (%.2f waited, %.2f whole fields), %.0f program changes, %.1f "
                     "draws skipped, %.0f vertices, %.2f depth discards, %.2f repeated display copies; in draw() %.1f ms/s, waited peeks %.1f ms/s; GPU %.2f ms "
                     "average (scene %.2f, copies %.2f, xfb %.2f, peeks %.2f, present %.2f), %.2f longest; "
                     "rings peak KiB v=%zu i=%zu c=%zu m=%zu; textures %llu KiB uploaded\n",
                     frames / seconds, static_cast<double>(cpu - last_cpu) / 1e6 / seconds,
                     static_cast<double>(window_stats_.present_wait_ns) / 1e6 / seconds,
                     static_cast<double>(window_stats_.frame_wait_ns) / 1e6 / seconds,
                     static_cast<double>(window_stats_.readback_ns) / 1e6 / seconds,
                     static_cast<double>(window_stats_.draws) / frames, static_cast<double>(window_stats_.copies) / frames,
                     static_cast<double>(window_stats_.clears) / frames, static_cast<double>(window_stats_.peeks) / frames,
                     static_cast<double>(window_stats_.sync_peeks) / frames,
                     static_cast<double>(window_stats_.sync_fields) / frames,
                     static_cast<double>(window_stats_.pipeline_changes) / frames,
                     static_cast<double>(window_stats_.skipped_draws) / frames,
                     static_cast<double>(window_stats_.vertices) / frames,
                     static_cast<double>(window_stats_.depth_discards) / frames,
                     static_cast<double>(window_stats_.xfb_repeats) / frames,
                     static_cast<double>(window_stats_.draw_ns) / 1e6 / seconds,
                     static_cast<double>(window_stats_.sync_peek_ns) / 1e6 / seconds,
                     window_stats_.gpu_frames != 0u ? window_stats_.gpu_ms / window_stats_.gpu_frames : 0.0,
                     gpu_part_average(0), gpu_part_average(1), gpu_part_average(2), gpu_part_average(3),
                     gpu_part_average(4),
                     window_stats_.gpu_max_ms, vertex_ring_.take_peak_bytes_per_frame() >> 10u,
                     index_ring_.take_peak_bytes_per_frame() >> 10u, constant_ring_.take_peak_bytes_per_frame() >> 10u,
                     matrix_ring_.take_peak_bytes_per_frame() >> 10u,
                     static_cast<unsigned long long>(textures_ != nullptr ? textures_->backend().take_uploaded_bytes() >> 10u
                                                                          : 0u));
    }
    last_cpu = cpu;
    last_time = now;
    window_stats_ = Window{};
}

// ---------------------------------------------------------------------------------------------------------------------
// GX state and draws
// ---------------------------------------------------------------------------------------------------------------------

void RendererGles::bind_efb(const float xf_vp[6]) {
    bind_efb_framebuffer();
    float x = 0.0f, y = 0.0f;
    float w = static_cast<float>(kEfbWidth * efb_scale_);
    float h = static_cast<float>(kEfbHeight * efb_scale_);
    const float divide = 1.0f / static_cast<float>(efb_divisor_);
    if (xf_vp != nullptr) {
        const float scale = static_cast<float>(efb_scale_);
        const float hw = std::abs(xf_vp[0]) * scale;
        const float hh = std::abs(xf_vp[2]) * scale;
        x = (xf_vp[1] - 342.0f) * scale - hw;
        y = (xf_vp[3] - 342.0f) * scale - hh;
        w = hw * 2.0f;
        h = hh * 2.0f;
    }
    // Render targets are drawn top row first (D3D's rows), so GL's viewport takes D3D's rectangle as it is.
    const std::array<int, 4> viewport{static_cast<int>(std::lround(x * divide)),
                                      static_cast<int>(std::lround(y * divide)),
                                      static_cast<int>(std::lround(w * divide)),
                                      static_cast<int>(std::lround(h * divide))};
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glDepthRangef(0.0f, 1.0f);
    bound_.viewport = viewport;
    bound_.depth_range = {0.0f, 1.0f};
    bound_.viewport_valid = true;
    glEnable(GL_SCISSOR_TEST);
    const std::array<int, 4> scissor{0, 0, static_cast<int>(efb_width()), static_cast<int>(efb_height())};
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    bound_.scissor = scissor;
    bound_.scissor_valid = true;
    bound_.scissor_input_valid = false;
}

void RendererGles::clear_efb(std::uint32_t clear_ar, std::uint32_t clear_gb, std::uint32_t clear_z24,
                             bool color_enable, bool alpha_enable, bool depth_enable, unsigned rect_x,
                             unsigned rect_y, unsigned rect_w, unsigned rect_h, std::uint8_t pixel_format) {
    ++window_stats_.clears;
    bind_efb_framebuffer();
    const std::int32_t efb_w = static_cast<std::int32_t>(kEfbWidth * efb_scale_);
    const std::int32_t efb_h = static_cast<std::int32_t>(kEfbHeight * efb_scale_);
    std::array<int, 4> rect{0, 0, efb_w, efb_h};
    if (rect_w != 0u && rect_h != 0u) {
        const ScaledEfbRect scaled = compute_scaled_efb_rect(
            static_cast<std::int32_t>(rect_x), static_cast<std::int32_t>(rect_y),
            static_cast<std::int32_t>(rect_x + rect_w), static_cast<std::int32_t>(rect_y + rect_h), kEfbWidth,
            kEfbHeight, efb_scale_);
        if (scaled.right <= scaled.left || scaled.bottom <= scaled.top) {
            return;
        }
        rect = {scaled.left, scaled.top, scaled.right - scaled.left, scaled.bottom - scaled.top};
    }
    rect = physical_rect(rect);
    if (pixel_format != 1u && color_enable) {
        alpha_enable = true;  // no destination alpha: it reads as one either way
    }
    if (!color_enable && !alpha_enable && !depth_enable) {
        return;
    }
    const EfbClearValues clear = make_efb_clear_values(clear_ar, clear_gb, clear_z24, pixel_format);
    // A whole-EFB clear without the scissor: a tiled GPU then clears its tiles instead of loading them.
    const bool whole = rect[0] == 0 && rect[1] == 0 && rect[2] == static_cast<int>(efb_width()) &&
        rect[3] == static_cast<int>(efb_height());
    if (whole) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        glEnable(GL_SCISSOR_TEST);
        glScissor(rect[0], rect[1], rect[2], rect[3]);
    }
    bound_.scissor_valid = false;
    bound_.scissor_input_valid = false;
    glColorMask(color_enable, color_enable, color_enable, alpha_enable);
    glDepthMask(depth_enable ? GL_TRUE : GL_FALSE);
    GLbitfield bits = 0;
    if (color_enable || alpha_enable) {
        glClearColor(clear.color[0], clear.color[1], clear.color[2], clear.color[3]);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (depth_enable) {
        glClearDepthf(clear.reversed_depth);  // reversed-Z: GX's farthest clears to about 0
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    glClear(bits);
    // The masks are the pipelines' state again at the next draw.
    bound_.state_valid = false;
}

gpu::Table RendererGles::texture_table_for(const TextureHandle* handles, unsigned count, const TextureCache& cache,
                                           bool) {
    if (texture_tables_.size() < kFramesInFlight) {
        texture_tables_.resize(kFramesInFlight);
    }
    std::vector<TextureTable>& tables = texture_tables_[frame_slot_];
    TextureTable table{};
    for (unsigned m = 0; m < kMaxTextureMaps; ++m) {
        const bool bound = m < count && handles[m].resource != nullptr;
        table.names[m] = bound ? cache.backend().view(handles[m].srv_index) : cache.backend().null_view();
    }
    tables.push_back(table);
    return gpu::Table{(static_cast<std::uint64_t>(frame_slot_ + 1u) << 32u) | tables.size()};
}

void RendererGles::attach_texture_uploads(TextureCache& cache) {
    textures_ = &cache;
    cache.backend().set_upload_list(true);
}

GLuint RendererGles::sampler_for(std::uint32_t key, const RenderConfig& cfg) {
    const std::uint64_t cache_key = (static_cast<std::uint64_t>(key) << 32u) | sampler_config_key(cfg);
    if (const auto it = samplers_.find(cache_key); it != samplers_.end()) {
        return it->second;
    }
    const auto wrap = [](std::uint32_t gx) -> GLint {
        switch (gx) {
        case 0: return GL_CLAMP_TO_EDGE;
        case 2: return GL_MIRRORED_REPEAT;
        default: return GL_REPEAT;
        }
    };
    const bool mag_linear = ((key >> 4u) & 1u) != 0u;
    const std::uint32_t minf = (key >> 5u) & 7u;
    const float min_lod = static_cast<float>((key >> 16u) & 0xffu) / 16.0f;
    const float max_lod = static_cast<float>((key >> 24u) & 0xffu) / 16.0f;
    // TexMinFilter: 0 near, 1 near-mip-near, 2 near-mip-linear, 4 linear, 5 linear-mip-near, 6 linear-mip-linear;
    // 3: linear without anisotropy (pack_sampler_key's arbitrary levels).
    const bool min_linear = minf >= 4u;
    const bool no_anisotropy = (minf & 3u) == 3u;
    const unsigned mip_mode = no_anisotropy ? 2u : minf & 3u;
    GLint min_filter = min_linear ? GL_LINEAR : GL_NEAREST;
    if (mip_mode == 1u) {
        min_filter = min_linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    } else if (mip_mode == 2u) {
        min_filter = min_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    }
    GLuint sampler = 0;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, mag_linear ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wrap(key & 3u));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wrap((key >> 2u) & 3u));
    if (mip_mode != 0u) {
        glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, min_lod);
        glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, max_lod > min_lod ? max_lod : min_lod);
    }
    // OpenGL ES samplers have no LOD bias: GX's (and the settings') bias is left out.
    samplers_.emplace(cache_key, sampler);
    return sampler;
}

gpu::Table RendererGles::sampler_table_for(const std::uint32_t keys[8]) {
    const RenderConfig cfg = get_render_config();
    const SamplerTableKey table_key = make_sampler_table_key(keys, cfg);
    if (const auto it = sampler_table_ids_.find(table_key); it != sampler_table_ids_.end()) {
        return gpu::Table{it->second};
    }
    SamplerTable table{};
    for (unsigned i = 0; i < 8u; ++i) {
        table.samplers[i] = sampler_for(keys[i], cfg);
    }
    sampler_tables_.push_back(table);
    const std::uint64_t id = sampler_tables_.size();
    sampler_table_ids_.emplace(table_key, id);
    return gpu::Table{id};
}

void RendererGles::refresh_default_sampler_table() {
    const std::uint32_t config = sampler_config_key(get_render_config());
    if (default_sampler_table_.ptr != 0u && config == default_sampler_config_) {
        return;
    }
    std::uint32_t keys[8]{};
    for (std::uint32_t& key : keys) {
        key = 1u | (1u << 2u) | (1u << 4u) | (5u << 5u) | (0xffu << 24u);
    }
    default_sampler_table_ = sampler_table_for(keys);
    default_sampler_config_ = config;
}

void RendererGles::apply_pipeline_state(const gles::Pipeline& p) {
    Bound& b = bound_;
    if (b.program != p.program) {
        glUseProgram(p.program);
        b.program = p.program;
        ++window_stats_.pipeline_changes;
    }
    if (enabled_attributes_ != p.attributes) {
        const std::uint32_t changed = enabled_attributes_ ^ p.attributes;
        for (const GlesVertexAttribute& attribute : kVertexAttributes) {
            const std::uint32_t bit = 1u << attribute.location;
            if ((changed & bit) != 0u || enabled_attributes_ == ~0u) {
                if ((p.attributes & bit) != 0u) {
                    glEnableVertexAttribArray(attribute.location);
                } else {
                    glDisableVertexAttribArray(attribute.location);
                }
            }
        }
        enabled_attributes_ = p.attributes;
    }
    const bool all = !b.state_valid;
    if (all || b.depth_test != p.depth_test) {
        (p.depth_test ? glEnable : glDisable)(GL_DEPTH_TEST);
        b.depth_test = p.depth_test;
    }
    if (all || b.depth_func != p.depth_func) {
        glDepthFunc(p.depth_func);
        b.depth_func = p.depth_func;
    }
    if (all || b.depth_write != p.depth_write) {
        glDepthMask(p.depth_write ? GL_TRUE : GL_FALSE);
        b.depth_write = p.depth_write;
    }
    if (all || b.blend != p.blend) {
        (p.blend ? glEnable : glDisable)(GL_BLEND);
        b.blend = p.blend;
    }
    if (p.blend && (all || b.blend_src_rgb != p.blend_src_rgb || b.blend_dst_rgb != p.blend_dst_rgb ||
                    b.blend_src_alpha != p.blend_src_alpha || b.blend_dst_alpha != p.blend_dst_alpha)) {
        glBlendFuncSeparate(p.blend_src_rgb, p.blend_dst_rgb, p.blend_src_alpha, p.blend_dst_alpha);
        b.blend_src_rgb = p.blend_src_rgb;
        b.blend_dst_rgb = p.blend_dst_rgb;
        b.blend_src_alpha = p.blend_src_alpha;
        b.blend_dst_alpha = p.blend_dst_alpha;
    }
    if (p.blend && (all || b.blend_op_rgb != p.blend_op_rgb || b.blend_op_alpha != p.blend_op_alpha)) {
        glBlendEquationSeparate(p.blend_op_rgb, p.blend_op_alpha);
        b.blend_op_rgb = p.blend_op_rgb;
        b.blend_op_alpha = p.blend_op_alpha;
    }
    if (all || b.write_rgb != p.write_rgb || b.write_alpha != p.write_alpha) {
        glColorMask(p.write_rgb, p.write_rgb, p.write_rgb, p.write_alpha);
        b.write_rgb = p.write_rgb;
        b.write_alpha = p.write_alpha;
    }
    if (all || b.cull != p.cull) {
        if (p.cull == 0u) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(p.cull);
        }
        b.cull = p.cull;
    }
    if (all) {
        glFrontFace(GL_CCW);  // D3D's clockwise front, seen through the negated y (shader_translate_gles.h)
    }
    b.state_valid = true;
    b.pipeline = &p;
}

void RendererGles::draw(const DrawCall& call) {
    if (call.pipeline == nullptr) {
        ++window_stats_.skipped_draws;
        return;
    }
    ++window_stats_.draws;
    const std::int64_t start = monotonic_ns();
    struct Timed {
        std::int64_t start;
        std::int64_t& total;
        ~Timed() { total += monotonic_ns() - start; }
    } timed{start, window_stats_.draw_ns};
    gles::Context& context = gles::Context::get();
    bind_efb_framebuffer();
    if (enabled_attributes_ == ~0u || bound_.pipeline == nullptr) {
        glBindVertexArray(vao_);
    }
    apply_pipeline_state(*call.pipeline);

    // b0, b1 and the palette at their draw's addresses; an address of 0 keeps the previous one (D3D12's root
    // arguments); before any, the rings' starts stand in.
    if (!bound_.constants_valid) {
        bound_.vs_constants = bound_.ps_constants = bound_.palette = 0u;
        bound_.buffers = {};
        bound_.offsets = {};
        bound_.sizes = {};
    }
    const auto bind = [&](gpu::Address address, unsigned slot, GLenum target, GLuint index, std::uint64_t size,
                          gpu::Address fallback) {
        gpu::Address use = address != 0u ? address
            : bound_.constants_valid       ? 0u
                                           : fallback;
        if (use == 0u) {
            return;
        }
        const gles::Context::BufferOffset resolved = context.resolve(use);
        if (resolved.object == nullptr) {
            return;
        }
        std::uint64_t range = size;
        if (size == 0u) {
            // The palette from the ring's base reaches the buffer's end (mtx_palette_base counts from there).
            range = resolved.offset == 0u ? resolved.object->size : kPaletteBytes;
        }
        range = std::min<std::uint64_t>(range, resolved.object->size - resolved.offset);
        if (bound_.buffers[slot] == resolved.buffer && bound_.offsets[slot] == resolved.offset &&
            bound_.sizes[slot] == range) {
            return;
        }
        if (target == GL_TEXTURE_BUFFER) {
            glActiveTexture(GL_TEXTURE0 + gles::kPaletteTextureUnit);
            glBindTexture(GL_TEXTURE_BUFFER, palette_texture_);
            glTexBufferRange(GL_TEXTURE_BUFFER, GL_RGBA32UI, resolved.buffer, static_cast<GLintptr>(resolved.offset),
                             static_cast<GLsizeiptr>(std::min<std::uint64_t>(range, 1u << 20u)));
            glActiveTexture(GL_TEXTURE0);
            bound_.textures_valid = false;
        } else {
            glBindBufferRange(target, index, resolved.buffer, static_cast<GLintptr>(resolved.offset),
                              static_cast<GLsizeiptr>(range));
        }
        bound_.buffers[slot] = resolved.buffer;
        bound_.offsets[slot] = resolved.offset;
        bound_.sizes[slot] = range;
    };
    bind(call.vs_constants, 0u, GL_UNIFORM_BUFFER, 0u, sizeof(GxVsConstants), constant_ring_.gpu_base());
    bind(call.ps_constants, 1u, GL_UNIFORM_BUFFER, 1u, sizeof(GxPsConstants), constant_ring_.gpu_base());
    bind(call.matrix_palette, 2u, gx_layout_.palette_storage_block ? GL_SHADER_STORAGE_BUFFER : GL_TEXTURE_BUFFER, 0u,
         0u, matrix_ring_.gpu_base());
    bound_.constants_valid = true;

    // Textures (units 0-7) and their samplers.
    if (call.texture_table.ptr != 0u) {
        const unsigned slot = static_cast<unsigned>(call.texture_table.ptr >> 32u) - 1u;
        const std::size_t index = static_cast<std::size_t>(call.texture_table.ptr & 0xffffffffu) - 1u;
        if (slot < texture_tables_.size() && index < texture_tables_[slot].size()) {
            const TextureTable& table = texture_tables_[slot][index];
            for (unsigned m = 0; m < 8u; ++m) {
                if (!bound_.textures_valid || bound_.textures[m] != table.names[m]) {
                    glActiveTexture(GL_TEXTURE0 + m);
                    glBindTexture(GL_TEXTURE_2D, table.names[m]);
                    bound_.textures[m] = table.names[m];
                }
            }
            bound_.textures_valid = true;
            glActiveTexture(GL_TEXTURE0);
        }
    }
    if (call.sampler_table.ptr != 0u && call.sampler_table.ptr <= sampler_tables_.size()) {
        const SamplerTable& table = sampler_tables_[call.sampler_table.ptr - 1u];
        for (unsigned m = 0; m < 8u; ++m) {
            if (!bound_.samplers_valid || bound_.samplers[m] != table.samplers[m]) {
                glBindSampler(m, table.samplers[m]);
                bound_.samplers[m] = table.samplers[m];
            }
        }
        bound_.samplers_valid = true;
    }

    // The GX scissor, scaled and clamped; an empty one rasterises nothing.
    if (!bound_.scissor_input_valid || call.scissor.left != bound_.scissor_input.left ||
        call.scissor.top != bound_.scissor_input.top || call.scissor.right != bound_.scissor_input.right ||
        call.scissor.bottom != bound_.scissor_input.bottom) {
        const ScaledEfbRect scaled = compute_scaled_efb_rect(call.scissor.left, call.scissor.top, call.scissor.right,
                                                             call.scissor.bottom, kEfbWidth, kEfbHeight, efb_scale_);
        if (scaled.empty()) {
            return;
        }
        const std::array<int, 4> scissor = physical_rect(
            {scaled.left, scaled.top, scaled.right - scaled.left, scaled.bottom - scaled.top});
        if (!bound_.scissor_valid || scissor != bound_.scissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
            bound_.scissor = scissor;
            bound_.scissor_valid = true;
        }
        bound_.scissor_input = call.scissor;
        bound_.scissor_input_valid = true;
    }
    // The XF viewport with the GX depth range (reversed-Z, as D3D12's: compute_scaled_gx_viewport).
    if (call.has_viewport) {
        const ScaledGxViewport scaled = compute_scaled_gx_viewport(call.xf_viewport, efb_scale_);
        if (scaled.valid()) {
            const float divide = 1.0f / static_cast<float>(efb_divisor_);
            const std::array<int, 4> viewport{
                static_cast<int>(std::lround(scaled.x * divide)), static_cast<int>(std::lround(scaled.y * divide)),
                static_cast<int>(std::lround(scaled.width * divide)),
                static_cast<int>(std::lround(scaled.height * divide))};
            const std::array<float, 2> range{scaled.min_depth, scaled.max_depth};
            if (!bound_.viewport_valid || viewport != bound_.viewport) {
                glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
                bound_.viewport = viewport;
            }
            if (!bound_.viewport_valid || range != bound_.depth_range) {
                glDepthRangef(range[0], range[1]);
                bound_.depth_range = range;
            }
            bound_.viewport_valid = true;
        }
    }
    if (!bound_.viewport_valid) {
        bind_efb(nullptr);
    }
    if (call.vertex_view.BufferLocation != bound_.vertex_address) {
        const gles::Context::BufferOffset vertices = context.resolve(call.vertex_view.BufferLocation);
        glBindVertexBuffer(0, vertices.buffer, static_cast<GLintptr>(vertices.offset),
                           static_cast<GLsizei>(call.vertex_view.StrideInBytes != 0u ? call.vertex_view.StrideInBytes
                                                                                      : 144u));
        bound_.vertex_address = call.vertex_view.BufferLocation;
    }
    if (call.index_count == 0u) {
        return;
    }
    window_stats_.vertices += call.index_count;
    if (call.index_view.BufferLocation != 0u) {
        const gles::Context::BufferOffset indices = context.resolve(call.index_view.BufferLocation);
        if (indices.buffer != bound_.index_buffer) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices.buffer);
            bound_.index_buffer = indices.buffer;
        }
        glDrawElements(call.topology, static_cast<GLsizei>(call.index_count), GL_UNSIGNED_SHORT,
                       reinterpret_cast<const void*>(static_cast<std::uintptr_t>(indices.offset)));
    } else {
        glDrawArrays(call.topology, 0, static_cast<GLsizei>(call.index_count));
    }
    if (context.checking()) {
        context.check_errors("draw");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// EFB copies
// ---------------------------------------------------------------------------------------------------------------------

void RendererGles::retire(gpu::TextureRef texture) {
    if (texture) {
        frames_[frame_slot_].retired.push_back(std::move(texture));
    }
}

void RendererGles::conversion_draw(GLuint framebuffer, unsigned dest_w, unsigned dest_h, bool depth,
                                   const EfbConversionShaderConstants& constants) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    bound_.framebuffer = framebuffer;
    // Every texel of the destination is written: nothing of it needs loading (a tiled GPU's reload).
    const GLenum attachment = GL_COLOR_ATTACHMENT0;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &attachment);
    glViewport(0, 0, static_cast<GLsizei>(dest_w), static_cast<GLsizei>(dest_h));
    utility_state();
    glUseProgram(conversion_override_ != 0u ? conversion_override_ : conversion_program_);
    bind_utility_constants(constants.values.data(), sizeof(constants.values));
    const GLuint source = depth ? efb_depth_->name : efb_color_->name;
    // t0 with s0 (linear; point for depth, which OpenGL ES does not filter) at unit 0, with s1 (point) at unit 1.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glBindSampler(0, depth ? point_clamp_ : linear_clamp_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, source);
    glBindSampler(1, point_clamp_);
    glActiveTexture(GL_TEXTURE0);
    bound_.textures_valid = false;
    bound_.samplers_valid = false;
    glBindVertexArray(empty_vao_);
    enabled_attributes_ = ~0u;
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(vao_);
    // The EFB textures leave the units, so a draw that samples a copy never also has the EFB bound.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    reset_gx_bindings();
    bind_efb(nullptr);
}

void RendererGles::record_conversion_blit(gles::Texture& dest, unsigned dest_w, unsigned dest_h,
                                          const EfbCopyParams& params, bool is_depth_copy, bool intensity,
                                          bool keep_alpha, std::uint8_t copy_format, float src_x, float src_y,
                                          float step_x, float step_y, float filter_row_offset) {
    EfbConversionShaderConstants constants =
        make_efb_conversion_constants(params, is_depth_copy, intensity, keep_alpha, copy_format, src_x, src_y,
                                      step_x, step_y, filter_row_offset, efb_scale_, kEfbHeight);
    to_physical(constants, false);
    conversion_draw(framebuffer_for(dest), dest_w, dest_h, is_depth_copy, constants);
}

void RendererGles::copy_efb_to_xfb(const EfbCopyParams& params, XfbTexture& dest) {
    if (!dest.texture) {
        throw std::runtime_error("XFB copy has no destination texture");
    }
    ++window_stats_.copies;
    if (discard_depth_at_copy_) {
        // The clear after this copy rewrites the whole depth buffer: the pass ending here need not store it.
        discard_depth_at_copy_ = false;
        bind_efb_framebuffer();
        const GLenum depth = GL_DEPTH_ATTACHMENT;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
        ++window_stats_.depth_discards;
    }
    gpu_part(kGpuXfb);
    // A skipped frame's display copy converts the same EFB with the same parameters again: copy the last result.
    XfbKey key{};
    key.src_x = params.src_x;
    key.src_y = params.src_y;
    key.src_width = params.src_width;
    key.src_height = params.src_height;
    key.y_scale_raw = params.y_scale_raw;
    key.filter0_raw = params.filter0_raw;
    key.filter1_raw = params.filter1_raw;
    key.flags = static_cast<std::uint32_t>(params.gamma) | (params.scale_y ? 0x100u : 0u) |
        (params.clamp_top ? 0x200u : 0u) | (params.clamp_bottom ? 0x400u : 0u) | (params.yuv ? 0x800u : 0u);
    key.width = dest.width;
    key.height = dest.height;
    const bool repeat = xfb_repeat_ && last_xfb_ && std::memcmp(&key, &last_xfb_key_, sizeof(key)) == 0;
    xfb_repeat_ = false;
    if (repeat) {
        ++window_stats_.xfb_repeats;
        if (last_xfb_.get() != dest.texture.get()) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_for(*last_xfb_));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer_for(*dest.texture));
            glDisable(GL_SCISSOR_TEST);
            bound_.scissor_valid = false;
            const GLint w = static_cast<GLint>(dest.width);
            const GLint h = static_cast<GLint>(dest.height);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, efb_framebuffer_);
            bound_.framebuffer = efb_framebuffer_;
        }
    } else {
        const EfbCopySamplingGeometry sampling = compute_xfb_sampling_geometry(params, dest.width, dest.height,
                                                                               efb_scale_);
        conversion_override_ = xfb_program_;
        record_conversion_blit(*dest.texture, dest.width, dest.height, params, false, false, true, 15u,
                               sampling.src_x, sampling.src_y, sampling.step_x, sampling.step_y,
                               sampling.filter_row_offset);
        conversion_override_ = 0u;
    }
    last_xfb_ = dest.texture;
    last_xfb_key_ = key;
    gpu_part(kGpuScene);
}

bool RendererGles::copy_efb_to_texture(const EfbCopyParams& params, TextureCache& textures, bool is_depth_copy) {
    ++window_stats_.copies;
    gpu_part(kGpuCopies);
    struct BackToScene {
        RendererGles& r;
        ~BackToScene() { r.gpu_part(kGpuScene); }
    } back_to_scene{*this};
    const unsigned dw_unscaled = params.half_scale ? std::max(1u, params.src_width / 2u) : params.src_width;
    const unsigned dh_unscaled = params.half_scale ? std::max(1u, params.src_height / 2u) : params.src_height;
    const EffectCopyPolicy policy = effect_copy_policy();
    const bool native = (policy.native_bloom_workspace || policy.native_effect_copies) && efb_scale_ > 1u &&
        is_effect_copy(policy, params, is_depth_copy, dw_unscaled, dh_unscaled);
    const ScaledEfbCopyExtent extent = compute_scaled_efb_copy_extent(
        static_cast<std::uint16_t>(dw_unscaled), static_cast<std::uint16_t>(dh_unscaled), native ? 1u : efb_scale_);
    const unsigned w = extent.width;
    const unsigned h = extent.height;
    if (w == 0u || h == 0u) {
        return false;
    }
    const std::uint64_t dest_key = static_cast<std::uint64_t>(params.dest_addr) |
        (static_cast<std::uint64_t>(params.target_format) << 32u) | (is_depth_copy ? (1ull << 40u) : 0ull) |
        (native ? (1ull << 41u) : 0ull);
    EfbCopyDest& dest = efb_copy_dests_[dest_key];
    dest.last_used = ++efb_copy_dests_tick_;
    if (!dest.texture || dest.width != w || dest.height != h) {
        if (dest.texture) {
            retire(std::move(dest.texture));
            efb_copy_dests_bytes_ -= std::min(efb_copy_dests_bytes_, dest.bytes);
        }
        dest.texture = gles::Context::get().create_texture(w, h, 1u, GL_RGBA8, true, nullptr);
        if (!dest.texture) {
            efb_copy_dests_.erase(dest_key);
            static std::atomic<unsigned> reported{0u};
            if (reported.fetch_add(1u, std::memory_order_relaxed) < 8u) {
                std::cerr << "[efb-copy] destination allocation failed size=" << w << 'x' << h << "; copy skipped\n";
            }
            return false;
        }
        dest.width = w;
        dest.height = h;
        dest.bytes = dest.texture->allocation_bytes;
        efb_copy_dests_bytes_ += dest.bytes;
    }
    const std::uint64_t budget = efb_copy_dest_budget_bytes();
    while (budget != 0u && efb_copy_dests_bytes_ > budget) {
        auto victim = efb_copy_dests_.end();
        for (auto it = efb_copy_dests_.begin(); it != efb_copy_dests_.end(); ++it) {
            if (it->first != dest_key &&
                (victim == efb_copy_dests_.end() || it->second.last_used < victim->second.last_used)) {
                victim = it;
            }
        }
        if (victim == efb_copy_dests_.end()) {
            break;
        }
        if (victim->second.texture) {
            retire(std::move(victim->second.texture));
        }
        efb_copy_dests_bytes_ -= std::min(efb_copy_dests_bytes_, victim->second.bytes);
        efb_copy_dests_.erase(victim);
    }
    EfbCopyDest& current = efb_copy_dests_[dest_key];
    gles::Texture& texture = *current.texture;
    const auto identity = efb_identity_copy_rect(params, is_depth_copy, efb_scale_, w, h);
    if (const auto& box = identity; box && efb_divisor_ == 1u) {
        // An exact colour copy: the EFB's texels as they are.
        const GLuint target = framebuffer_for(texture);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, efb_framebuffer_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glDisable(GL_SCISSOR_TEST);
        const GLint left = static_cast<GLint>(box->left);
        const GLint top = static_cast<GLint>(box->top);
        glBlitFramebuffer(left, top, left + static_cast<GLint>(w), top + static_cast<GLint>(h), 0, 0,
                          static_cast<GLint>(w), static_cast<GLint>(h), GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, efb_framebuffer_);
        bound_.framebuffer = efb_framebuffer_;
        reset_gx_bindings();
        bind_efb(nullptr);
        return textures.register_efb_copy(params.dest_addr, params, current.texture);
    }
    const EfbCopySamplingGeometry sampling = compute_efb_copy_sampling_geometry(params, w, h, efb_scale_);
    // Intensity copies to RA4/RA8 (IA4/IA8) keep the source alpha; pure-I formats replicate Y into alpha.
    const bool keep_alpha = params.target_format == 2u || params.target_format == 3u;
    record_conversion_blit(texture, w, h, params, is_depth_copy, params.intensity, keep_alpha, params.target_format,
                           sampling.src_x, sampling.src_y, sampling.step_x, sampling.step_y,
                           sampling.filter_row_offset);
    return textures.register_efb_copy(params.dest_addr, params, current.texture);
}

bool RendererGles::copy_output_efb_to_texture(const EfbCopyParams& params, TextureCache& textures) {
    return copy_efb_to_texture(params, textures, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------------------------------------------------

bool RendererGles::present(const XfbTexture* source, bool capture_backbuffer, bool black) {
    if (source == nullptr || !source->texture) {
        throw std::runtime_error("present requested without a valid XFB blit source");
    }
    gles::Texture& xfb = *source->texture;
    host_window::gles_drawable_size(window_, drawable_width_, drawable_height_);
    const int bb_w = std::max(drawable_width_, 1);
    const int bb_h = std::max(drawable_height_, 1);
    host_window::backbuffer_width = static_cast<unsigned>(bb_w);
    host_window::backbuffer_height = static_cast<unsigned>(bb_h);
    const auto aspect = galaxy::experimental_effective_host_aspect();
    const auto content_aspect = galaxy::presentation_content_aspect(
        aspect ? aspect->content_aspect : galaxy::kWiiContentAspect, source->prerecorded_movie);
    galaxy::ContentViewport content = galaxy::fit_content_viewport(bb_w, bb_h, content_aspect);
    if (content.width <= 0 || content.height <= 0) {
        content = galaxy::ContentViewport{0, 0, bb_w, bb_h};
    }
    (void)galaxy::g_presentation_content.publish(bb_w, bb_h, content);

    gpu_part(kGpuPresent);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bound_.framebuffer = 0u;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float surround = source->safety_surround;
    glClearColor(surround, surround, surround, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!black) {
        // The XFB's top row (its first) at the window's top: GL's window rows count from the bottom.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_for(xfb));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        const GLint top = bb_h - content.top;
        glBlitFramebuffer(0, 0, static_cast<GLint>(xfb.width), static_cast<GLint>(xfb.height), content.left, top,
                          content.left + content.width, top - content.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    bool captured = false;
    if (capture_backbuffer && !backbuffer_capture_path_.empty()) {
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(bb_w) * bb_h * 4u);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, bb_w, bb_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        write_ppm(backbuffer_capture_path_, rgba, static_cast<unsigned>(bb_w), static_cast<unsigned>(bb_h), true);
        captured = true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, efb_framebuffer_);
    bound_.framebuffer = efb_framebuffer_;
    reset_gx_bindings();
    bound_.state_valid = false;
    presented_this_frame_ = true;
    gpu_part(kGpuScene);
    return captured;
}

// ---------------------------------------------------------------------------------------------------------------------
// Peeks and captures
// ---------------------------------------------------------------------------------------------------------------------

void RendererGles::create_first_use_resources() {
    gles::Context& context = gles::Context::get();
    peek_target_ = context.create_texture(1u, 1u, 1u, GL_RGBA8, true, "peek target");
    field_target_ = context.create_texture(kEfbWidth, kEfbHeight, 1u, GL_RGBA8, true, "peek field target");
    if (!peek_target_ || !field_target_) {
        throw std::runtime_error("EFB peek resources unavailable");
    }
}

void RendererGles::ensure_readback(Readback& readback, std::size_t bytes, std::size_t data_bytes) {
    if (readback.buffer == 0u) {
        glGenBuffers(1, &readback.buffer);
    }
    if (readback.bytes < bytes) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, readback.buffer);
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        readback.bytes = bytes;
    }
    readback.data.resize(data_bytes);
}

void RendererGles::record_peek_field(gles::Texture& target, EfbPeekKind kind,
                                     const EfbConversionShaderConstants& constants, unsigned width, unsigned height,
                                     Readback& readback) {
    ++window_stats_.peeks;
    gpu_part(kGpuPeeks);
    const GLuint framebuffer = framebuffer_for(target);
    EfbConversionShaderConstants physical = constants;
    to_physical(physical, true);
    // A whole field of a divided EFB repeats each physical texel divisor x divisor times. Convert one texel per
    // physical texel instead and expand on the CPU: a quarter of the GPU work and of the uncached readback.
    const bool field = constants.values[15] < 0.0f;
    const unsigned divisor = field && efb_divisor_ > 1u && width % efb_divisor_ == 0u &&
            height % efb_divisor_ == 0u ? efb_divisor_ : 1u;
    if (divisor > 1u) {
        physical.values[2] = constants.values[2];
        physical.values[3] = constants.values[3];
    }
    const unsigned read_width = width / divisor;
    const unsigned read_height = height / divisor;
    readback.divisor = divisor;
    readback.width = read_width;
    readback.height = read_height;
    conversion_draw(framebuffer, read_width, read_height, kind == EfbPeekKind::Depth, physical);
    ensure_readback(readback, static_cast<std::size_t>(read_width) * read_height * 4u,
                    static_cast<std::size_t>(width) * height * 4u);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, readback.buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, static_cast<GLsizei>(read_width), static_cast<GLsizei>(read_height), GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, efb_framebuffer_);
    bound_.framebuffer = efb_framebuffer_;
    gpu_part(kGpuScene);
}

bool RendererGles::complete_readback(Readback& readback) {
    if (readback.buffer == 0u || readback.bytes == 0u) {
        return false;
    }
    const std::int64_t start = monotonic_ns();
    const std::size_t read_bytes = static_cast<std::size_t>(readback.width) * readback.height * 4u;
    const bool expand = readback.divisor > 1u;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, readback.buffer);
    const void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(read_bytes),
                                          GL_MAP_READ_BIT);
    if (mapped != nullptr) {
        if (expand) {
            readback.staging.resize(read_bytes);
            std::memcpy(readback.staging.data(), mapped, read_bytes);
        } else {
            std::memcpy(readback.data.data(), mapped, std::min(read_bytes, readback.data.size()));
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (mapped != nullptr && expand) {
        const unsigned d = readback.divisor;
        const std::size_t out_width = static_cast<std::size_t>(readback.width) * d;
        const std::size_t out_row = out_width * 4u;
        const std::size_t out_rows = readback.data.size() / out_row;
        for (std::size_t y = 0; y < out_rows; ++y) {
            std::uint8_t* out = readback.data.data() + y * out_row;
            if (y % d != 0u) {
                std::memcpy(out, out - out_row, out_row);
                continue;
            }
            const std::uint8_t* in = readback.staging.data() + (y / d) * readback.width * 4u;
            for (unsigned x = 0; x < readback.width; ++x) {
                std::uint32_t texel;
                std::memcpy(&texel, in + x * 4u, 4u);
                for (unsigned i = 0; i < d; ++i) {
                    std::memcpy(out + (static_cast<std::size_t>(x) * d + i) * 4u, &texel, 4u);
                }
            }
        }
    }
    readback.fence = 0u;
    window_stats_.readback_ns += monotonic_ns() - start;
    ++window_stats_.readbacks;
    return mapped != nullptr;
}

bool RendererGles::peek_efb(std::uint16_t x, std::uint16_t y, EfbPeekKind kind, std::uint32_t& value) {
    value = 0u;
    ++window_stats_.sync_peeks;
    const std::int64_t start = monotonic_ns();
    struct Timed {
        std::int64_t start;
        std::int64_t& total;
        ~Timed() { total += monotonic_ns() - start; }
    } timed{start, window_stats_.sync_peek_ns};
    if (pointer_depth_fence_ != 0u) {
        throw std::logic_error("GXPeek resources still own an unpublished pointer field");
    }
    if (x >= kEfbWidth || y >= kEfbHeight || !peek_target_ || !make_current()) {
        return false;
    }
    const EfbPeekSamplingGeometry sampling = compute_efb_peek_sampling_geometry(x, y, efb_scale_);
    record_peek_field(*peek_target_, kind, make_efb_peek_conversion_constants(sampling, kind), 1u, 1u, peek_readback_);
    if (!complete_readback(peek_readback_)) {
        return false;
    }
    const std::uint8_t* rgba = peek_readback_.data.data();
    const std::uint32_t rgb = (static_cast<std::uint32_t>(rgba[0]) << 16u) |
        (static_cast<std::uint32_t>(rgba[1]) << 8u) | rgba[2];
    value = kind == EfbPeekKind::Depth ? rgb : (static_cast<std::uint32_t>(rgba[3]) << 24u) | rgb;
    return true;
}

bool RendererGles::capture_pointer_depth(std::span<std::uint32_t> pixels) {
    ++window_stats_.sync_fields;
    if (pixels.size() != kEfbWidth * kEfbHeight) {
        throw std::invalid_argument("pointer depth field requires 640x528 pixels");
    }
    const std::uint64_t fence = submit_pointer_depth_capture();
    if (fence == 0u) {
        return false;
    }
    wait_for_fence_value(fence);
    const std::uint8_t* rgba = nullptr;
    if (!try_complete_pointer_depth_capture_in_place(rgba) || rgba == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = (static_cast<std::uint32_t>(rgba[i * 4u]) << 16u) |
            (static_cast<std::uint32_t>(rgba[i * 4u + 1u]) << 8u) | rgba[i * 4u + 2u];
    }
    return true;
}

std::uint64_t RendererGles::submit_pointer_depth_capture() {
    if (pointer_depth_fence_ != 0u) {
        throw std::logic_error("GXPeek resources still own an unpublished pointer field");
    }
    if (pointer_depth_in_frame_ || !field_target_ || !make_current()) {
        return 0u;
    }
    Readback& readback = pointer_depth_ring_[pointer_depth_next_];
    pointer_depth_next_ = (pointer_depth_next_ + 1u) % kPointerDepthRing;
    EfbConversionShaderConstants constants =
        make_efb_peek_conversion_constants(compute_efb_peek_sampling_geometry(0u, 0u, efb_scale_), EfbPeekKind::Depth);
    constants.values[15] = -1.0f;  // the whole field
    record_peek_field(*field_target_, EfbPeekKind::Depth, constants, kEfbWidth, kEfbHeight, readback);
    pointer_depth_readback_ = &readback;
    pointer_depth_fence_ = gles::Context::get().submit();
    readback.fence = pointer_depth_fence_;
    return pointer_depth_fence_;
}

bool RendererGles::record_pointer_depth_capture_in_frame() {
    if (pointer_depth_in_frame_ || pointer_depth_fence_ != 0u || !field_target_) {
        return false;
    }
    Readback& readback = pointer_depth_ring_[pointer_depth_next_];
    pointer_depth_next_ = (pointer_depth_next_ + 1u) % kPointerDepthRing;
    EfbConversionShaderConstants constants =
        make_efb_peek_conversion_constants(compute_efb_peek_sampling_geometry(0u, 0u, efb_scale_), EfbPeekKind::Depth);
    constants.values[15] = -1.0f;
    record_peek_field(*field_target_, EfbPeekKind::Depth, constants, kEfbWidth, kEfbHeight, readback);
    pointer_depth_readback_ = &readback;
    pointer_depth_in_frame_ = true;  // fenced at end_frame
    return true;
}

bool RendererGles::try_complete_pointer_depth_capture_in_place(const std::uint8_t*& rgba) {
    rgba = nullptr;
    if (pointer_depth_fence_ == 0u) {
        if (pointer_depth_in_frame_) {
            return false;
        }
        throw std::logic_error("pointer depth completion has no owned fence");
    }
    if (completed_fence_value() < pointer_depth_fence_ || !make_current()) {
        return false;
    }
    if (pointer_depth_readback_ != nullptr && complete_readback(*pointer_depth_readback_)) {
        rgba = pointer_depth_readback_->data.data();
    }
    pointer_depth_fence_ = 0u;
    return rgba != nullptr;
}

void RendererGles::ensure_async_field(AsyncField& field) {
    if (!field.target) {
        field.target = gles::Context::get().create_texture(kEfbWidth, kEfbHeight, 1u, GL_RGBA8, true,
                                                           "async peek field");
        if (!field.target) {
            throw std::runtime_error("async peek field target unavailable");
        }
    }
}

std::uint64_t RendererGles::submit_async_peek_field(std::size_t channel, EfbPeekKind kind, std::size_t slot) {
    AsyncField& field = async_fields_[channel];
    if (field.fence != 0u || slot >= kAsyncPeekFieldSlots || !make_current()) {
        return 0u;
    }
    ensure_async_field(field);
    EfbConversionShaderConstants constants =
        make_efb_peek_conversion_constants(compute_efb_peek_sampling_geometry(0u, 0u, efb_scale_), kind);
    constants.values[15] = -1.0f;
    record_peek_field(*field.target, kind, constants, kEfbWidth, kEfbHeight, field.readbacks[slot]);
    field.readbacks[slot].fence = gles::Context::get().submit();
    field.fence = field.readbacks[slot].fence;
    field.pending_slot = slot;
    return field.fence;
}

bool RendererGles::record_async_peek_field_in_frame(std::size_t channel, EfbPeekKind kind, std::size_t slot) {
    AsyncField& field = async_fields_[channel];
    if (field.fence != 0u || slot >= kAsyncPeekFieldSlots) {
        return false;
    }
    ensure_async_field(field);
    EfbConversionShaderConstants constants =
        make_efb_peek_conversion_constants(compute_efb_peek_sampling_geometry(0u, 0u, efb_scale_), kind);
    constants.values[15] = -1.0f;
    record_peek_field(*field.target, kind, constants, kEfbWidth, kEfbHeight, field.readbacks[slot]);
    field.fence = kAsyncPeekInFrame;  // fenced at end_frame
    field.pending_slot = slot;
    return true;
}

bool RendererGles::try_complete_async_peek_field(std::size_t channel) {
    AsyncField& field = async_fields_[channel];
    if (field.fence == 0u || field.fence == kAsyncPeekInFrame || completed_fence_value() < field.fence ||
        !make_current()) {
        return false;
    }
    complete_readback(field.readbacks[field.pending_slot]);
    field.fence = 0u;
    return true;
}

const std::uint8_t* RendererGles::async_peek_field_rgba(std::size_t channel, std::size_t slot) const noexcept {
    const AsyncField& field = async_fields_[channel];
    return slot < kAsyncPeekFieldSlots && !field.readbacks[slot].data.empty() ? field.readbacks[slot].data.data()
                                                                               : nullptr;
}

void RendererGles::set_debug_capture_paths(std::string efb_ppm_path, std::string backbuffer_ppm_path) {
    efb_capture_path_ = std::move(efb_ppm_path);
    backbuffer_capture_path_ = std::move(backbuffer_ppm_path);
}

bool RendererGles::debug_copy_efb_to_readback() {
    if (efb_capture_path_.empty() || !make_current()) {
        return false;
    }
    const unsigned w = efb_width();
    const unsigned h = efb_height();
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, efb_framebuffer_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    write_ppm(efb_capture_path_, rgba, w, h, false);
    return true;
}

void RendererGles::debug_log_readback() {}

void RendererGles::debug_log_backbuffer_readback() {}

}  // namespace galaxy::gx
