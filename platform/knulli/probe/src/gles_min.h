// SPDX-License-Identifier: GPL-3.0-only
// Minimal OpenGL ES 2.0/3.0 binding: the probe loads every entry point at run time, from the driver's
// libGLESv2 first and SDL_GL_GetProcAddress second, so no GLES headers or import libraries are needed.
#pragma once
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>

namespace gl {
using GLenum = unsigned int; using GLuint = unsigned int; using GLint = int; using GLsizei = int;
using GLboolean = unsigned char; using GLbitfield = unsigned int; using GLfloat = float; using GLchar = char;
using GLubyte = unsigned char; using GLintptr = std::intptr_t; using GLsizeiptr = std::intptr_t;

enum : GLenum {
    NO_ERROR = 0, VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02, EXTENSIONS = 0x1F03,
    SHADING_LANGUAGE_VERSION = 0x8B8C, NUM_EXTENSIONS = 0x821D, MAJOR_VERSION = 0x821B, MINOR_VERSION = 0x821C,
    MAX_TEXTURE_SIZE = 0x0D33, MAX_VERTEX_ATTRIBS = 0x8869, MAX_VERTEX_UNIFORM_VECTORS = 0x8DFB,
    MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD, MAX_TEXTURE_IMAGE_UNITS = 0x8872, MAX_RENDERBUFFER_SIZE = 0x84E8,
    MAX_SAMPLES = 0x8D57, MAX_DRAW_BUFFERS = 0x8824, MAX_UNIFORM_BLOCK_SIZE = 0x8A30, MAX_VARYING_VECTORS = 0x8DFC,
    DEPTH_BUFFER_BIT = 0x100, COLOR_BUFFER_BIT = 0x4000,
    TRIANGLES = 0x0004, TRIANGLE_STRIP = 0x0005,
    BLEND = 0x0BE2, DEPTH_TEST = 0x0B71, CULL_FACE = 0x0B44, SCISSOR_TEST = 0x0C11,
    SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303, ONE = 1, LEQUAL = 0x0203,
    FLOAT = 0x1406, UNSIGNED_BYTE = 0x1401, UNSIGNED_SHORT = 0x1403,
    ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, STATIC_DRAW = 0x88E4, DYNAMIC_DRAW = 0x88E8, STREAM_DRAW = 0x88E0,
    FRAGMENT_SHADER = 0x8B30, VERTEX_SHADER = 0x8B31, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84,
    TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800,
    TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, NEAREST = 0x2600, LINEAR = 0x2601, CLAMP_TO_EDGE = 0x812F, REPEAT = 0x2901,
    RGBA = 0x1908, RGB = 0x1907, LUMINANCE = 0x1909, ALPHA = 0x1906, RGBA8 = 0x8058, R8 = 0x8229, RED = 0x1903,
    UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05,
    FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9, RENDERBUFFER = 0x8D41,
    COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_ATTACHMENT = 0x8D00, FRAMEBUFFER_COMPLETE = 0x8CD5,
    DEPTH_COMPONENT16 = 0x81A5, DEPTH_COMPONENT24 = 0x81A6, DEPTH24_STENCIL8 = 0x88F0,
    HIGH_FLOAT = 0x8DF2,
};

struct Api {
#define GLFN(ret, name, args) ret(*name) args = nullptr;
#include "gles_min_fns.inc"
#undef GLFN
    int missing_core = 0;
    // `getproc` is SDL_GL_GetProcAddress.
    void load(void* (*getproc)(const char*)) {
        void* lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
        auto find = [&](const char* n) -> void* {
            void* p = lib ? dlsym(lib, n) : nullptr;
            return p ? p : getproc(n);
        };
#define GLFN(ret, name, args) name = reinterpret_cast<ret(*) args>(find("gl" #name));
#include "gles_min_fns.inc"
#undef GLFN
        void* core[] = {reinterpret_cast<void*>(GetString), reinterpret_cast<void*>(CreateShader),
                        reinterpret_cast<void*>(DrawArrays), reinterpret_cast<void*>(TexImage2D),
                        reinterpret_cast<void*>(BindFramebuffer), reinterpret_cast<void*>(ReadPixels)};
        for (void* p : core) missing_core += p == nullptr;
    }
};
}  // namespace gl
