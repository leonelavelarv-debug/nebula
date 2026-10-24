// SPDX-License-Identifier: GPL-3.0-only
// GX pipelines on OpenGL ES (pipeline_cache_gles.h).

#include "galaxy/gx/gles/pipeline_cache_gles.h"

#include "galaxy/gx/host_window.h"
#include "galaxy/gx/vk/shader_compile_vk.h"
#include "galaxy/native_host.h"
#include "galaxy/windows_power.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace galaxy::gx {

using namespace gles::gl;

namespace {

constexpr std::uint32_t kKeyFileMagic = 0x4b454c47u;  // "GLEK"
constexpr std::uint32_t kKeyFileVersion = 1u;
constexpr std::uint32_t kMaxRecords = 65536u;
constexpr std::uint32_t kBinaryMagic = 0x42454c47u;  // "GLEB"

std::uint64_t elapsed_us(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

template <typename T>
bool read_pod(std::ifstream& in, T& value) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

template <typename T>
void write_pod(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename Write>
bool replace_file(const std::filesystem::path& path, Write&& write) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        write(out);
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    return !error;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

GLenum gx_src_blend(std::uint32_t f) {
    switch (f) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 2: return GL_DST_COLOR;
    case 3: return GL_ONE_MINUS_DST_COLOR;
    case 4: return GL_SRC_ALPHA;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return GL_DST_ALPHA;
    case 7: return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_ONE;
    }
}

GLenum gx_dst_blend(std::uint32_t f) {
    switch (f) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 2: return GL_SRC_COLOR;
    case 3: return GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return GL_DST_ALPHA;
    case 7: return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_ZERO;
    }
}

// Colour-only factors in the alpha equation take their alpha forms.
GLenum to_alpha_blend(GLenum factor) {
    switch (factor) {
    case GL_SRC_COLOR: return GL_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_ALPHA;
    case GL_DST_COLOR: return GL_DST_ALPHA;
    case GL_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_ALPHA;
    default: return factor;
    }
}

std::string uber_ps_hlsl(unsigned variant) {
    std::string ps;
    ps += uber_ps_variant_dual_source(variant) ? "#define UBER_DUAL_SRC 1\n" : "#define UBER_DUAL_SRC 0\n";
    ps += uber_ps_variant_export_depth(variant) ? "#define UBER_EXPORT_DEPTH 1\n" : "#define UBER_EXPORT_DEPTH 0\n";
    ps += uber_ps_variant_early_depth(variant) ? "#define UBER_EARLY_DEPTH 1\n" : "#define UBER_EARLY_DEPTH 0\n";
    ps += uber_ps_source();
    return ps;
}

std::string shader_log(GLuint object, bool program) {
    GLint length = 0;
    if (program) {
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
    } else {
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
    }
    std::string text(static_cast<std::size_t>(std::max(length, 1)), '\0');
    if (program) {
        glGetProgramInfoLog(object, length, nullptr, text.data());
    } else {
        glGetShaderInfoLog(object, length, nullptr, text.data());
    }
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

GLuint compile_shader(GLenum stage, const std::string& source, std::string& error) {
    const GLuint shader = glCreateShader(stage);
    const char* text = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        error = shader_log(shader, false);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// The locations of the program's active vertex attributes, as a mask.
std::uint32_t attribute_mask(GLuint program) {
    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
    std::uint32_t mask = 0;
    for (GLint i = 0; i < count; ++i) {
        char name[128];
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveAttrib(program, static_cast<GLuint>(i), sizeof(name), &length, &size, &type, name);
        const GLint location = glGetAttribLocation(program, name);
        if (location >= 0 && location < 32) {
            mask |= 1u << static_cast<unsigned>(location);
        }
    }
    return mask;
}

}  // namespace

PipelineCacheGles::~PipelineCacheGles() noexcept {
    try {
        shutdown();
    } catch (...) {
    }
}

bool PipelineCacheGles::initialize(gpu::Device*, const gles::GxLayout* layout, const std::filesystem::path& cache_dir,
                                   unsigned worker_count) {
    layout_ = layout;
    translate_options_.palette_storage_block = layout->palette_storage_block;
    generator_options_.scalar_byte_masks = true;  // Mali (device_quirks_vk.cpp)
    const gles::Capabilities& caps = gles::Context::get().caps();
    translate_options_.glsl_version = gles::glsl_version_for(caps.version_major, caps.version_minor);
    driver_key_ = caps.renderer + " | " + caps.version;
    const std::uint64_t driver_hash = fnv1a64(driver_key_.data(), driver_key_.size());
    char folder[40];
    std::snprintf(folder, sizeof(folder), "gles-%016llx", static_cast<unsigned long long>(
        combine64(combine64(driver_hash, gles::kTranslatorRevision), vk::kCompilerRevision)));
    cache_dir_ = cache_dir / folder;
    std::error_code error;
    std::filesystem::create_directories(cache_dir_, error);
    {
        std::ofstream note(cache_dir_ / "driver.txt", std::ios::trunc);
        note << driver_key_ << "\n";
    }
    binaries_ = caps.program_binary && std::getenv("GALAXY_GLES_NO_BINARIES") == nullptr;
    if (const char* dump = std::getenv("GALAXY_GLES_SHADER_DUMP")) {
        shader_dump_dir_ = dump;
        std::filesystem::create_directories(shader_dump_dir_, error);
    }
    {
        const char* value = std::getenv("GALAXY_GLES_PENDING");
        const std::string mode = value != nullptr ? value : "wait";
        pending_ = mode == "skip" ? Pending::Skip : mode == "uber" ? Pending::Uber : Pending::Wait;
    }
    load_disk();
    stopping_ = false;
    ready_ = true;
    // Workers with contexts of their own: the keys of earlier sessions at start, and the ones skip/uber hand over.
    const unsigned wanted = std::getenv("GALAXY_GLES_WORKERS") != nullptr
        ? static_cast<unsigned>(std::strtoul(std::getenv("GALAXY_GLES_WORKERS"), nullptr, 10))
        : std::clamp(worker_count, 1u, 2u);
    for (unsigned i = 0; i < wanted; ++i) {
        void* context = host_window::gles_create_worker_context();
        if (context == nullptr) {
            break;
        }
        worker_contexts_.push_back(context);
    }
    for (void* context : worker_contexts_) {
        workers_.emplace_back([this, context] { worker_main(context); });
    }
    std::size_t prewarm = 0;
    if (!workers_.empty()) {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        for (const Entry& entry : entries_) {
            const bool points = entry.key.render_state.primitive_topology == gpu::kTopologyTypePointValue;
            const PairKey pair = pair_for(entry.key, points);
            if (in_flight_.insert(pair).second) {
                enqueue({pair, entry.vs_key, entry.ps_key});
                ++prewarm;
            }
        }
    }
    if (pending_ == Pending::Uber) {
        // The uber programs now: every first use draws with one.
        const std::string vs(uber_vs_source());
        for (unsigned variant = 0; variant < kUberPsVariants; ++variant) {
            const std::string vs_glsl = translate(vs, gles::Stage::Vertex, "uber vertex shader");
            const std::string ps_glsl = translate(uber_ps_hlsl(variant), gles::Stage::Fragment, "uber pixel shader");
            if (!vs_glsl.empty() && !ps_glsl.empty()) {
                const std::uint64_t hash = combine64(fnv1a64(vs_glsl.data(), vs_glsl.size()),
                                                     fnv1a64(ps_glsl.data(), ps_glsl.size()));
                uber_programs_[variant] = link(vs_glsl, ps_glsl, hash, "an uber program");
            }
        }
    }
    std::fprintf(stderr, "[pipeline-cache-gles] keys=%zu prewarm=%zu workers=%zu pending=%s binaries=%s dir=%s\n",
                 entries_.size(), prewarm, workers_.size(),
                 pending_ == Pending::Wait ? "wait" : pending_ == Pending::Skip ? "skip" : "uber",
                 binaries_ ? "yes" : "no", cache_dir_.string().c_str());
    // The context goes to the render thread now (GxBackend starts it next): release it here.
    host_window::gles_make_current(nullptr, false);
    gles::Context::get().clear_owner_thread();
    return true;
}

void PipelineCacheGles::shutdown() {
    if (!ready_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    jobs_cv_.notify_all();
    for (std::thread& worker : workers_) {
        worker.join();
    }
    workers_.clear();
    for (void* context : worker_contexts_) {
        host_window::gles_destroy_worker_context(context);
    }
    worker_contexts_.clear();
    save_disk();
    const bool can_delete = gles::Context::get().on_owner_thread();
    if (can_delete) {
        for (auto& [pair, program] : programs_) {
            if (program.name != 0u) glDeleteProgram(program.name);
        }
        for (Program& program : uber_programs_) {
            if (program.name != 0u) glDeleteProgram(program.name);
        }
    }
    programs_.clear();
    published_.clear();
    uber_pipelines_.clear();
    memo_pipeline_ = nullptr;
    ready_ = false;
}

PipelineCacheGles::PairKey PipelineCacheGles::pair_for(const PsoKey& key, bool points) const noexcept {
    return PairKey{key.vs_hash, key.ps_hash, points ? 1u : 0u};
}

std::filesystem::path PipelineCacheGles::binary_path(std::uint64_t hash) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(hash));
    return cache_dir_ / name;
}

std::string PipelineCacheGles::translate(const std::string& hlsl, gles::Stage stage, const char* what) {
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    std::string glsl = gles::translate(hlsl, stage, gles::ShaderKind::Gx, translate_options_, error);
    compile_us_.fetch_add(elapsed_us(start), std::memory_order_relaxed);
    translations_.fetch_add(1u, std::memory_order_relaxed);
    if (glsl.empty()) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 6) {
            std::fprintf(stderr, "[pipeline-cache-gles] %s did not translate: %s\n", what, error.c_str());
            if (!shader_dump_dir_.empty()) {
                write_text(std::filesystem::path(shader_dump_dir_) / "failed.hlsl", hlsl);
            }
        }
        return {};
    }
    if (!shader_dump_dir_.empty()) {
        const std::uint64_t hash = fnv1a64(glsl.data(), glsl.size());
        char name[48];
        std::snprintf(name, sizeof(name), "%016llx.%s", static_cast<unsigned long long>(hash),
                      stage == gles::Stage::Vertex ? "vert" : "frag");
        write_text(std::filesystem::path(shader_dump_dir_) / name, glsl);
    }
    return glsl;
}

PipelineCacheGles::Program PipelineCacheGles::link(const std::string& vs_glsl, const std::string& ps_glsl,
                                                   std::uint64_t binary_hash, const char* what) {
    Program out;
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    const GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_glsl, error);
    const GLuint ps = vs != 0u ? compile_shader(GL_FRAGMENT_SHADER, ps_glsl, error) : 0u;
    GLuint program = 0;
    if (vs != 0u && ps != 0u) {
        program = glCreateProgram();
        if (binaries_) {
            glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        }
        glAttachShader(program, vs);
        glAttachShader(program, ps);
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            error = shader_log(program, true);
            glDeleteProgram(program);
            program = 0;
        } else {
            glDetachShader(program, vs);
            glDetachShader(program, ps);
        }
    }
    if (vs != 0u) glDeleteShader(vs);
    if (ps != 0u) glDeleteShader(ps);
    compile_us_.fetch_add(elapsed_us(start), std::memory_order_relaxed);
    links_.fetch_add(1u, std::memory_order_relaxed);
    if (program == 0u) {
        out.failed = true;
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 6) {
            std::fprintf(stderr, "[pipeline-cache-gles] the driver refused %s: %s\n", what, error.c_str());
            if (!shader_dump_dir_.empty()) {
                write_text(std::filesystem::path(shader_dump_dir_) / "refused.vert", vs_glsl);
                write_text(std::filesystem::path(shader_dump_dir_) / "refused.frag", ps_glsl);
            }
        }
        return out;
    }
    out.name = program;
    out.attributes = attribute_mask(program);
    if (binaries_ && binary_hash != 0u) {
        GLint length = 0;
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        if (length > 0) {
            std::vector<char> binary(static_cast<std::size_t>(length));
            GLenum format = 0;
            GLsizei written = 0;
            glGetProgramBinary(program, length, &written, &format, binary.data());
            if (written > 0) {
                replace_file(binary_path(binary_hash), [&](std::ofstream& file) {
                    write_pod(file, kBinaryMagic);
                    write_pod(file, static_cast<std::uint32_t>(format));
                    write_pod(file, out.attributes);
                    write_pod(file, static_cast<std::uint32_t>(written));
                    file.write(binary.data(), written);
                });
            }
        }
    }
    return out;
}

PipelineCacheGles::Program PipelineCacheGles::build_program(const PairKey& pair, const VertexShaderKey& vs_key,
                                                            const PixelShaderKey& ps_key) {
    ShaderGenerator generator;
    generator.set_options(generator_options_);
    std::string vs_hlsl = generator.generate_vs(vs_key);
    if (pair.form == 1u) {
        vs_hlsl = vk::with_point_size(vs_hlsl);
    }
    const std::string ps_hlsl = generator.generate_ps(ps_key);
    const std::uint64_t hash = combine64(combine64(fnv1a64(vs_hlsl.data(), vs_hlsl.size()),
                                                   fnv1a64(ps_hlsl.data(), ps_hlsl.size())),
                                         translate_options_.palette_storage_block ? 1u : 2u);
    if (binaries_) {
        std::ifstream in(binary_path(hash), std::ios::binary);
        std::uint32_t magic = 0, format = 0, attributes = 0, length = 0;
        if (in && read_pod(in, magic) && magic == kBinaryMagic && read_pod(in, format) && read_pod(in, attributes) &&
            read_pod(in, length) && length != 0u && length < (64u << 20u)) {
            std::vector<char> binary(length);
            if (in.read(binary.data(), length)) {
                const GLuint program = glCreateProgram();
                glProgramBinary(program, static_cast<GLenum>(format), binary.data(), static_cast<GLsizei>(length));
                GLint ok = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &ok);
                if (ok == GL_TRUE) {
                    binary_loads_.fetch_add(1u, std::memory_order_relaxed);
                    return Program{program, attributes, false};
                }
                glDeleteProgram(program);
                binary_failures_.fetch_add(1u, std::memory_order_relaxed);
            }
        }
    }
    const std::string vs_glsl = translate(vs_hlsl, gles::Stage::Vertex, "a vertex shader");
    const std::string ps_glsl = vs_glsl.empty() ? std::string{} : translate(ps_hlsl, gles::Stage::Fragment,
                                                                             "a pixel shader");
    if (vs_glsl.empty() || ps_glsl.empty()) {
        Program failed;
        failed.failed = true;
        return failed;
    }
    return link(vs_glsl, ps_glsl, hash, "a GX program");
}

std::unique_ptr<gles::Pipeline> PipelineCacheGles::make_pipeline(const RenderStateKey& rs, const Program& program,
                                                                 bool, bool uber) {
    auto pipeline = std::make_unique<gles::Pipeline>();
    gles::Pipeline& p = *pipeline;
    p.program = program.name;
    p.attributes = program.attributes;
    p.uber = uber;
    p.topology = rs.primitive_topology == gpu::kTopologyTypePointValue ? GL_POINTS
        : rs.primitive_topology == gpu::kTopologyTypeLineValue        ? GL_LINES
                                                                       : GL_TRIANGLES;
    // GX front faces are the rasteriser's back faces (as D3D12's and Vulkan's pipelines swap them).
    switch (static_cast<CullMode>(rs.cull)) {
    case CullMode::Front: p.cull = GL_BACK; break;
    case CullMode::Back: p.cull = GL_FRONT; break;
    case CullMode::All: p.cull = GL_FRONT_AND_BACK; break;
    default: p.cull = 0; break;
    }
    if (p.topology != GL_TRIANGLES) {
        p.cull = 0;
    }
    // Reversed-Z against GX's z24, as D3D12's.
    static constexpr GLenum kDepthCompare[8] = {GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL,
                                                GL_LESS,  GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS};
    const std::uint32_t zb = rs.zmode_bits;
    if ((zb & 1u) != 0u) {
        p.depth_test = true;
        p.depth_func = kDepthCompare[(zb >> 1u) & 0x7u];
        p.depth_write = (zb & (1u << 4u)) != 0u;
    }
    const std::uint32_t bb = rs.blend_bits;
    p.write_rgb = (bb & (1u << 25u)) != 0u;
    p.write_alpha = (bb & (1u << 26u)) != 0u;
    if ((bb & 1u) != 0u) {
        p.blend = true;
        if ((bb & 2u) != 0u) {
            // GX subtract: dst - src, factors one.
            p.blend_src_rgb = GL_ONE;
            p.blend_dst_rgb = GL_ONE;
            p.blend_op_rgb = GL_FUNC_REVERSE_SUBTRACT;
        } else {
            p.blend_src_rgb = gx_src_blend((bb >> 2u) & 0x7u);
            p.blend_dst_rgb = gx_dst_blend((bb >> 5u) & 0x7u);
            p.blend_op_rgb = GL_FUNC_ADD;
            if (rs.pixfmt != 1u) {
                const auto no_dst_alpha = [](GLenum factor) {
                    return factor == GL_DST_ALPHA ? GL_ONE : factor == GL_ONE_MINUS_DST_ALPHA ? GL_ZERO : factor;
                };
                p.blend_src_rgb = no_dst_alpha(p.blend_src_rgb);
                p.blend_dst_rgb = no_dst_alpha(p.blend_dst_rgb);
            }
        }
        p.blend_src_alpha = to_alpha_blend(p.blend_src_rgb);
        p.blend_dst_alpha = to_alpha_blend(p.blend_dst_rgb);
        p.blend_op_alpha = p.blend_op_rgb;
        if ((bb & (1u << 28u)) != 0u) {  // constant destination alpha is independent
            p.blend_src_alpha = GL_ONE;
            p.blend_dst_alpha = GL_ZERO;
            p.blend_op_alpha = GL_FUNC_ADD;
        }
    }
    return pipeline;
}

gles::Pipeline* PipelineCacheGles::uber_pipeline(const RenderStateKey& rs, const PixelShaderKey& ps_key) {
    const unsigned variant = uber_ps_variant(ps_key);
    const Program& program = uber_programs_[variant];
    if (program.name == 0u) {
        return nullptr;
    }
    RenderStateKey state = rs;
    state.blend_bits &= ~(kRenderStateMotionTarget | kRenderStateReactiveTarget);
    const std::uint64_t id = combine64(PsoKey{0, 0, state}.hash(), program.name);
    if (const auto it = uber_pipelines_.find(id); it != uber_pipelines_.end()) {
        return it->second.get();
    }
    std::unique_ptr<gles::Pipeline> pipeline = make_pipeline(state, program, false, true);
    gles::Pipeline* const raw = pipeline.get();
    uber_pipelines_.emplace(id, std::move(pipeline));
    return raw;
}

gles::Pipeline* PipelineCacheGles::get(const PsoKey& key, const PixelShaderKey& ps_key, const VertexShaderKey& vs_key) {
    last_get_used_uber_ = false;
    if (memo_pipeline_ != nullptr && memo_key_ == key) {
        ++specialized_hits_;
        return memo_pipeline_;
    }
    if (const auto it = published_.find(key); it != published_.end()) {
        ++specialized_hits_;
        memo_key_ = key;
        memo_pipeline_ = it->second.get();
        return memo_pipeline_;
    }
    {
        std::lock_guard<std::mutex> lock(entries_mutex_);
        const Entry entry{key, vs_key, ps_key};
        if (entry_keys_.insert(key).second) {
            entries_.push_back(entry);
            unsaved_.store(true, std::memory_order_release);
        }
    }
    const bool points = key.render_state.primitive_topology == gpu::kTopologyTypePointValue;
    const PairKey pair = pair_for(key, points);
    auto program = programs_.find(pair);
    if (program == programs_.end()) {
        drain_completions();
        program = programs_.find(pair);
    }
    if (program == programs_.end()) {
        if (pending_ != Pending::Wait && !workers_.empty()) {
            if (in_flight_.insert(pair).second) {
                enqueue({pair, vs_key, ps_key});
            }
            if (pending_ == Pending::Uber) {
                if (gles::Pipeline* uber = uber_pipeline(key.render_state, ps_key)) {
                    ++uber_fallbacks_;
                    last_get_used_uber_ = true;
                    return uber;
                }
            }
            return nullptr;  // drawn once its program is linked
        }
        // Built here, now; a queued prewarm job for it is taken back.
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            const auto queued = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) { return job.pair == pair; });
            if (queued != jobs_.end()) {
                jobs_.erase(queued);
                in_flight_.erase(pair);
            }
        }
        if (in_flight_.contains(pair)) {
            // A worker is building it: wait for that one.
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(jobs_mutex_);
                    jobs_cv_.wait_for(lock, std::chrono::milliseconds(2));
                }
                drain_completions();
                program = programs_.find(pair);
                if (program != programs_.end() || !in_flight_.contains(pair)) {
                    break;
                }
            }
        }
        if (program == programs_.end()) {
            program = programs_.emplace(pair, build_program(pair, vs_key, ps_key)).first;
        }
    }
    if (program->second.failed || program->second.name == 0u) {
        published_.emplace(key, nullptr);
        return nullptr;
    }
    std::unique_ptr<gles::Pipeline> pipeline =
        make_pipeline(key.render_state, program->second, (ps_key.flags & 0x80u) != 0u, false);
    gles::Pipeline* const made = pipeline.get();
    published_.emplace(key, std::move(pipeline));
    memo_key_ = key;
    memo_pipeline_ = made;
    return made;
}

void PipelineCacheGles::drain_completions() {
    std::vector<std::pair<PairKey, Program>> done;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        done.swap(completions_);
    }
    for (auto& [pair, program] : done) {
        in_flight_.erase(pair);
        if (!programs_.emplace(pair, program).second && program.name != 0u) {
            glDeleteProgram(program.name);
        }
    }
}

void PipelineCacheGles::enqueue(const Job& job) {
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        jobs_.push_back(job);
    }
    jobs_cv_.notify_one();
}

void PipelineCacheGles::worker_main(void* context) {
    galaxy::host::name_current_thread(L"Nebula GL Build");
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
    if (!host_window::gles_make_worker_current(context)) {
        std::fprintf(stderr, "[pipeline-cache-gles] a worker context could not be made current\n");
        return;
    }
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(jobs_mutex_);
            jobs_cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) {
                break;
            }
            job = jobs_.front();
            jobs_.pop_front();
        }
        Program program = build_program(job.pair, job.vs_key, job.ps_key);
        // Linked here: complete before the render thread's context uses it.
        glFinish();
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            completions_.emplace_back(job.pair, program);
        }
        jobs_cv_.notify_all();
    }
    host_window::gles_make_worker_current(nullptr);
}

void PipelineCacheGles::load_disk() {
    std::ifstream in(cache_dir_.parent_path() / "gles_keys.bin", std::ios::binary);
    std::uint32_t magic = 0, version = 0, count = 0;
    if (!in || !read_pod(in, magic) || !read_pod(in, version) || !read_pod(in, count) || magic != kKeyFileMagic ||
        version != kKeyFileVersion || count > kMaxRecords) {
        return;
    }
    std::lock_guard<std::mutex> lock(entries_mutex_);
    for (std::uint32_t i = 0; i < count; ++i) {
        Entry entry;
        if (!read_pod(in, entry)) {
            break;
        }
        if (entry.key.vs_hash != entry.vs_key.hash() || entry.key.ps_hash != entry.ps_key.hash()) {
            continue;
        }
        if (entry_keys_.insert(entry.key).second) {
            entries_.push_back(entry);
        }
    }
    entries_on_disk_ = entries_.size();
}

void PipelineCacheGles::save_disk() noexcept {
    try {
        std::vector<Entry> entries;
        {
            std::lock_guard<std::mutex> lock(entries_mutex_);
            entries = entries_;
            entries_on_disk_ = entries.size();
        }
        if (entries.size() > kMaxRecords) {
            entries.resize(kMaxRecords);
        }
        replace_file(cache_dir_.parent_path() / "gles_keys.bin", [&](std::ofstream& file) {
            write_pod(file, kKeyFileMagic);
            write_pod(file, kKeyFileVersion);
            write_pod(file, static_cast<std::uint32_t>(entries.size()));
            for (const Entry& entry : entries) {
                write_pod(file, entry);
            }
        });
        unsaved_.store(false, std::memory_order_release);
    } catch (...) {
    }
}

void PipelineCacheGles::flush_disk() {
    if (unsaved_.load(std::memory_order_acquire)) {
        save_disk();
    }
}

PipelineCacheGles::CompileStats PipelineCacheGles::compile_stats() const {
    CompileStats stats{};
    stats.pso_compiles = links_.load(std::memory_order_relaxed);
    stats.pso_library_loads = binary_loads_.load(std::memory_order_relaxed);
    stats.shader_pair_compiles = translations_.load(std::memory_order_relaxed) / 2u;
    stats.blobs_from_cache = binary_loads_.load(std::memory_order_relaxed);
    stats.blob_compile_us = compile_us_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(entries_mutex_);
    stats.pso_keys = entries_.size();
    stats.pso_keys_on_disk = entries_on_disk_;
    return stats;
}

}  // namespace galaxy::gx
