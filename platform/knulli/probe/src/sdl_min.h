// SPDX-License-Identifier: GPL-3.0-only
// Minimal SDL2 binding loaded at run time (dlopen). The probe links against nothing but glibc, so the
// same binary runs on KNULLI, ROCKNIX, ArkOS or muOS with whatever SDL2 the firmware or PortMaster ships.
#pragma once
#include <cstdint>
#include <dlfcn.h>

namespace sdl {
using Uint8 = std::uint8_t;
using Uint32 = std::uint32_t;
struct Version { Uint8 major, minor, patch; };
struct DisplayMode { Uint32 format; int w; int h; int refresh_rate; void* driverdata; };
union Event { Uint32 type; Uint8 padding[56]; };
struct Window;
struct Joystick;
using GLContext = void*;

enum : Uint32 {
    INIT_TIMER = 0x1u, INIT_VIDEO = 0x20u, INIT_JOYSTICK = 0x200u, INIT_GAMECONTROLLER = 0x2000u, INIT_EVENTS = 0x4000u,
    WINDOW_FULLSCREEN = 0x1u, WINDOW_OPENGL = 0x2u, WINDOW_SHOWN = 0x4u, WINDOW_FULLSCREEN_DESKTOP = 0x1001u,
    WINDOWPOS_UNDEFINED = 0x1FFF0000u,
    QUIT = 0x100u, KEYDOWN = 0x300u, JOYBUTTONDOWN = 0x603u, CONTROLLERBUTTONDOWN = 0x650u,
};
enum GLattr : int {
    GL_RED_SIZE = 0, GL_GREEN_SIZE = 1, GL_BLUE_SIZE = 2, GL_ALPHA_SIZE = 3, GL_DOUBLEBUFFER = 5, GL_DEPTH_SIZE = 6,
    GL_STENCIL_SIZE = 7, GL_CONTEXT_MAJOR_VERSION = 17, GL_CONTEXT_MINOR_VERSION = 18, GL_CONTEXT_PROFILE_MASK = 21,
};
enum : int { GL_CONTEXT_PROFILE_ES = 0x4 };

struct Api {
    void* handle = nullptr;
    const char* loaded_from = nullptr;
#define SDLFN(ret, name, args) ret(*name) args = nullptr;
#include "sdl_min_fns.inc"
#undef SDLFN
    bool load() {
        static const char* names[] = {"libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so"};
        for (const char* n : names) {
            handle = dlopen(n, RTLD_NOW | RTLD_GLOBAL);
            if (handle) { loaded_from = n; break; }
        }
        if (!handle) return false;
        bool ok = true;
#define SDLFN(ret, name, args) name = reinterpret_cast<ret(*) args>(dlsym(handle, "SDL_" #name)); ok = ok && name != nullptr;
#include "sdl_min_fns.inc"
#undef SDLFN
        return ok;
    }
};
}  // namespace sdl
