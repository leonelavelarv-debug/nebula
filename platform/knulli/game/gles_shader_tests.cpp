// SPDX-License-Identifier: GPL-3.0-only
// The GLES shader translation against the live driver (no window: an EGL context without a surface): the renderer's
// own programs, every uber variant and a spread of ShaderGenerator keys, translated (shader_translate_gles.h) and
// linked, for both forms of the matrix palette. GALAXY_GLES_TEST_DUMP=<dir> writes the GLSL; GALAXY_GLES_TEST_KEYS=
// <gles_keys.bin> adds the keys a game session recorded. Exit status 0 when everything linked.

#include "galaxy/gx/gles/gles_context.h"
#include "galaxy/gx/gles/shader_translate_gles.h"
#include "galaxy/gx/renderer_common.h"
#include "galaxy/gx/shader_gen.h"
#include "galaxy/gx/shader_keys.h"
#include "galaxy/gx/uber_shaders.h"
#include "galaxy/gx/vk/shader_compile_vk.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace galaxy::gx;
using namespace galaxy::gx::gles::gl;

namespace {

void* g_egl = nullptr;
PFNEGLGETPROCADDRESSPROC g_get_proc = nullptr;

void* load_gl(const char* name) {
    void* fn = g_get_proc != nullptr ? reinterpret_cast<void*>(g_get_proc(name)) : nullptr;
    return fn;
}

bool start_egl() {
    g_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_egl == nullptr) {
        g_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (g_egl == nullptr) {
        std::fprintf(stderr, "no libEGL\n");
        return false;
    }
    g_get_proc = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(dlsym(g_egl, "eglGetProcAddress"));
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(g_get_proc("eglGetPlatformDisplayEXT"));
    auto initialize = reinterpret_cast<PFNEGLINITIALIZEPROC>(dlsym(g_egl, "eglInitialize"));
    auto bind_api = reinterpret_cast<PFNEGLBINDAPIPROC>(dlsym(g_egl, "eglBindAPI"));
    auto choose_config = reinterpret_cast<PFNEGLCHOOSECONFIGPROC>(dlsym(g_egl, "eglChooseConfig"));
    auto create_context = reinterpret_cast<PFNEGLCREATECONTEXTPROC>(dlsym(g_egl, "eglCreateContext"));
    auto make_current = reinterpret_cast<PFNEGLMAKECURRENTPROC>(dlsym(g_egl, "eglMakeCurrent"));
    auto get_display = reinterpret_cast<PFNEGLGETDISPLAYPROC>(dlsym(g_egl, "eglGetDisplay"));
    EGLDisplay display = get_platform_display != nullptr
        ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
        : EGL_NO_DISPLAY;
    if (display == EGL_NO_DISPLAY) {
        display = get_display(EGL_DEFAULT_DISPLAY);
    }
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !initialize(display, &major, &minor)) {
        std::fprintf(stderr, "eglInitialize failed\n");
        return false;
    }
    bind_api(EGL_OPENGL_ES_API);
    const EGLint config_attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!choose_config(display, config_attributes, &config, 1, &count) || count == 0) {
        const EGLint any[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
        if (!choose_config(display, any, &config, 1, &count) || count == 0) {
            std::fprintf(stderr, "no ES3 EGL config\n");
            return false;
        }
    }
    // 3.2, else 3.1 (an OpenGL ES 3.1 driver such as Panfrost: the shaders are then GLSL ES 3.10).
    for (const EGLint minor : {2, 1}) {
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
        EGLContext context = create_context(display, config, EGL_NO_CONTEXT, context_attributes);
        if (context != EGL_NO_CONTEXT && make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
            return true;
        }
    }
    std::fprintf(stderr, "no surfaceless ES 3.1 or 3.2 context\n");
    return false;
}

std::string g_dump;
int g_failures = 0;
int g_linked = 0;
double g_translate_ms = 0.0;
double g_link_ms = 0.0;

GLuint compile(GLenum stage, const std::string& source, const char* what) {
    const GLuint shader = glCreateShader(stage);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[4096]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "FAIL compile %s (%s):\n%s\n", what, stage == GL_VERTEX_SHADER ? "vs" : "ps", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool check_pair(const std::string& vs_hlsl, const std::string& ps_hlsl, gles::ShaderKind kind,
                const gles::TranslateOptions& options, const std::string& what) {
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string vs = gles::translate(vs_hlsl, gles::Stage::Vertex, kind, options, error);
    if (vs.empty()) {
        std::fprintf(stderr, "FAIL translate %s vs: %s\n", what.c_str(), error.c_str());
        ++g_failures;
        return false;
    }
    const std::string ps = gles::translate(ps_hlsl, gles::Stage::Fragment, kind, options, error);
    if (ps.empty()) {
        std::fprintf(stderr, "FAIL translate %s ps: %s\n", what.c_str(), error.c_str());
        if (!g_dump.empty()) {
            std::ofstream(g_dump + "/failed_ps.hlsl") << gles::prepare_hlsl(ps_hlsl, kind, options);
        }
        ++g_failures;
        return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (!g_dump.empty()) {
        std::string name = what;
        for (char& c : name) {
            if (c == ' ' || c == '/' || c == ',') c = '_';
        }
        std::ofstream(g_dump + "/" + name + ".vert") << vs;
        std::ofstream(g_dump + "/" + name + ".frag") << ps;
    }
    const GLuint v = compile(GL_VERTEX_SHADER, vs, what.c_str());
    const GLuint p = v != 0u ? compile(GL_FRAGMENT_SHADER, ps, what.c_str()) : 0u;
    bool ok = false;
    if (v != 0u && p != 0u) {
        const GLuint program = glCreateProgram();
        glAttachShader(program, v);
        glAttachShader(program, p);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            char log[4096]{};
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::fprintf(stderr, "FAIL link %s:\n%s\n", what.c_str(), log);
        } else {
            ok = true;
        }
        glDeleteProgram(program);
    }
    if (v != 0u) glDeleteShader(v);
    if (p != 0u) glDeleteShader(p);
    const auto t2 = std::chrono::steady_clock::now();
    g_translate_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    g_link_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
    if (ok) {
        ++g_linked;
    } else {
        ++g_failures;
    }
    return ok;
}

std::string uber_ps_hlsl(unsigned variant) {
    std::string ps;
    ps += uber_ps_variant_dual_source(variant) ? "#define UBER_DUAL_SRC 1\n" : "#define UBER_DUAL_SRC 0\n";
    ps += uber_ps_variant_export_depth(variant) ? "#define UBER_EXPORT_DEPTH 1\n" : "#define UBER_EXPORT_DEPTH 0\n";
    ps += uber_ps_variant_early_depth(variant) ? "#define UBER_EARLY_DEPTH 1\n" : "#define UBER_EARLY_DEPTH 0\n";
    ps += uber_ps_source();
    return ps;
}

struct KeyRecord {
    PsoKey key{};
    VertexShaderKey vs_key{};
    PixelShaderKey ps_key{};
};

}  // namespace

int main() {
    if (!start_egl()) {
        return 2;
    }
    if (!gles::Context::get().initialize(&load_gl)) {
        return 2;
    }
    if (const char* dump = std::getenv("GALAXY_GLES_TEST_DUMP")) {
        g_dump = dump;
        std::filesystem::create_directories(g_dump);
    }
    const int glsl = gles::glsl_version_for(gles::Context::get().caps().version_major,
                                            gles::Context::get().caps().version_minor);
    std::fprintf(stderr, "GLSL ES %d\n", glsl);
    gles::TranslateOptions utility{};
    utility.glsl_version = glsl;
    check_pair(std::string(blit_vertex_shader_source()), std::string(efb_conversion_shader_source()),
               gles::ShaderKind::Utility, utility, "conversion");
    check_pair(std::string(blit_vertex_shader_source()), std::string(blit_pixel_shader_source()),
               gles::ShaderKind::Utility, utility, "blit");

    ShaderGenOptions generator_options;
    generator_options.scalar_byte_masks = true;
    ShaderGenerator generator;
    generator.set_options(generator_options);
    std::vector<KeyRecord> keys;
    {
        KeyRecord simple{};
        simple.vs_key.num_texgens = 1;
        simple.vs_key.num_chans = 1;
        keys.push_back(simple);
        KeyRecord dual = simple;
        dual.ps_key.flags |= 0x80u;
        keys.push_back(dual);
        KeyRecord ztex = simple;
        ztex.ps_key.flags |= 0x03u;
        keys.push_back(ztex);
        keys.push_back(KeyRecord{});
    }
    if (const char* path = std::getenv("GALAXY_GLES_TEST_KEYS")) {
        std::ifstream in(path, std::ios::binary);
        std::uint32_t magic = 0, version = 0, count = 0;
        in.read(reinterpret_cast<char*>(&magic), 4);
        in.read(reinterpret_cast<char*>(&version), 4);
        in.read(reinterpret_cast<char*>(&count), 4);
        for (std::uint32_t i = 0; i < count && in; ++i) {
            KeyRecord record;
            if (in.read(reinterpret_cast<char*>(&record), sizeof(record))) {
                keys.push_back(record);
            }
        }
        std::fprintf(stderr, "%u recorded keys\n", count);
    }
    for (const bool storage : {true, false}) {
        gles::TranslateOptions options{};
        options.palette_storage_block = storage;
        options.glsl_version = glsl;
        const char* form = storage ? "ssbo" : "texbuf";
        const std::string vs(uber_vs_source());
        for (unsigned variant = 0; variant < kUberPsVariants; ++variant) {
            check_pair(vs, uber_ps_hlsl(variant), gles::ShaderKind::Gx, options,
                       std::string("uber ") + form + " " + std::to_string(variant));
        }
        std::size_t index = 0;
        for (const KeyRecord& record : keys) {
            const bool points = record.key.render_state.primitive_topology == 1u;
            std::string vs_hlsl = generator.generate_vs(record.vs_key);
            if (points) {
                vs_hlsl = vk::with_point_size(vs_hlsl);
            }
            check_pair(vs_hlsl, generator.generate_ps(record.ps_key), gles::ShaderKind::Gx, options,
                       std::string("key ") + form + " " + std::to_string(index++));
            if (!storage && index > 40u) {
                break;  // the texture-buffer form on a sample of the keys
            }
        }
    }
    std::printf("gles shader tests: %d linked, %d failed; translate %.1f ms, driver %.1f ms\n", g_linked, g_failures,
                g_translate_ms, g_link_ms);
    return g_failures == 0 ? 0 : 1;
}
