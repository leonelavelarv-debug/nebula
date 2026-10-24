// SPDX-License-Identifier: GPL-3.0-only
// The game window and input on KNULLI (sdl_host.h, host_window.h and the window functions of gx_d3d12.h): SDL2 (the
// system's, with Mali's fbdev EGL behind it) for a full-screen window with an OpenGL ES 3.2 context, its events, the
// handheld's controls as an SDL game controller (PortMaster sets SDL_GAMECONTROLLERCONFIG), keys; EGL directly for
// the shader-build workers' contexts, which share the window context's objects.

#include "sdl_host.h"

#include "galaxy/gx/display_modes.h"
#include "galaxy/gx/gles/gles_context.h"
#include "galaxy/gx/host_window.h"
#include "galaxy/gx/render_config.h"
#include "galaxy/gx_d3d12.h"
#include "galaxy/native_host.h"
#include "galaxy/platform_paths.h"
#include "galaxy/platform_time.h"
#include "galaxy/virtual_keys.h"

#include <SDL.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace galaxy::sdl {
namespace {

std::mutex g_input_mutex;
InputState g_input;  // guarded by g_input_mutex
SDL_Window* g_window = nullptr;
SDL_GLContext g_context = nullptr;
std::atomic<bool> g_home_visible{false};
std::atomic<bool> g_menu_context{false};
std::thread::id g_sdl_thread;
std::atomic<std::uint32_t> g_rumble_pad{0};
std::atomic<std::uint32_t> g_rumble_level{0};
std::uint32_t g_rumble_sent_pad = 0;
std::uint32_t g_rumble_sent_level = 0;
std::uint64_t g_rumble_sent_ms = 0;
std::array<SDL_GameController*, kMaxPads> g_gamepads{};

// SDL scancodes to the runtime's key numbering (virtual_keys.h).
int virtual_key(SDL_Scancode code) noexcept {
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) return 'A' + (code - SDL_SCANCODE_A);
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) return '1' + (code - SDL_SCANCODE_1);
    if (code == SDL_SCANCODE_0) return '0';
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) return VK_F1 + (code - SDL_SCANCODE_F1);
    switch (code) {
    case SDL_SCANCODE_RETURN: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
    case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
    case SDL_SCANCODE_LALT: return VK_LMENU;
    case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
    case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
    case SDL_SCANCODE_RALT: return VK_RMENU;
    default: return 0;
    }
}

void set_key(int vk, bool down) {
    if (vk <= 0 || vk > 0xFF) {
        return;
    }
    g_input.keys.set_key(static_cast<std::uint8_t>(vk), down);
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) g_input.keys.set_key(VK_SHIFT, down);
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) g_input.keys.set_key(VK_CONTROL, down);
    if (vk == VK_LMENU || vk == VK_RMENU) g_input.keys.set_key(VK_MENU, down);
}

// The handheld's face buttons are Nintendo's (A right, B below): SDL's positional layout with the Nintendo scheme
// (input_bindings.h). NEBULA_PAD_SCHEME=xbox keeps the Xbox scheme.
galaxy::input::ControlScheme pad_scheme() noexcept {
    static const galaxy::input::ControlScheme scheme = [] {
        const char* value = std::getenv("NEBULA_PAD_SCHEME");
        return value != nullptr && std::strcmp(value, "xbox") == 0 ? galaxy::input::ControlScheme::Xbox
                                                                   : galaxy::input::ControlScheme::SwitchPro;
    }();
    return scheme;
}

// The handheld's control profile, from the launcher (which reads the custom firmware and the device from
// PortMaster): NEBULA_FACE_LAYOUT=labels when the firmware's SDL mapping names the face buttons by their labels
// (SDL A = the right button) instead of their positions; NEBULA_STICKS=2|1|0, the analog sticks the device has
// (1: the D-pad points; 0: the D-pad moves Mario and nothing points); NEBULA_QUIT_COMBO=menu|select|both, the
// firmware's quit chord with Start (MENU/Function, SELECT).
struct PadProfile {
    bool labels = false;
    int sticks = 2;
    bool quit_menu = true;
    bool quit_select = false;
};
const PadProfile& pad_profile() noexcept {
    static const PadProfile profile = [] {
        PadProfile p;
        const char* face = std::getenv("NEBULA_FACE_LAYOUT");
        p.labels = face != nullptr && std::strcmp(face, "labels") == 0;
        if (const char* sticks = std::getenv("NEBULA_STICKS"); sticks != nullptr && sticks[0] >= '0' && sticks[0] <= '2') {
            p.sticks = sticks[0] - '0';
        }
        if (const char* quit = std::getenv("NEBULA_QUIT_COMBO")) {
            p.quit_menu = std::strcmp(quit, "select") != 0;
            p.quit_select = std::strcmp(quit, "select") == 0 || std::strcmp(quit, "both") == 0;
        }
        std::fprintf(stderr, "[input] profile: face buttons by %s, %d stick(s), quit with %s + Start\n",
                     p.labels ? "label" : "position", p.sticks,
                     p.quit_menu && p.quit_select ? "MENU or SELECT" : (p.quit_select ? "SELECT" : "MENU"));
        return p;
    }();
    return profile;
}

float axis(SDL_GameController* pad, SDL_GameControllerAxis which) noexcept {
    return std::clamp(static_cast<float>(SDL_GameControllerGetAxis(pad, which)) / 32767.0f, -1.0f, 1.0f);
}

void sample_gamepads() {
    namespace pad = galaxy::input::pad;
    for (std::size_t i = 0; i < kMaxPads; ++i) {
        SDL_GameController* gamepad = g_gamepads[i];
        galaxy::input::PadSample& sample = g_input.pads[i];
        sample = {};
        g_input.pad_ids[i] = 0;
        if (gamepad == nullptr || SDL_GameControllerGetAttached(gamepad) != SDL_TRUE) {
            continue;
        }
        g_input.pad_ids[i] = static_cast<std::uint32_t>(
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad))) + 1u;
        sample.connected = true;
        sample.scheme = pad_scheme();
        const auto button = [gamepad](SDL_GameControllerButton b) {
            return SDL_GameControllerGetButton(gamepad, b) != 0;
        };
        std::uint32_t bits = 0;
        if (button(SDL_CONTROLLER_BUTTON_DPAD_UP)) bits |= pad::bit(pad::kDpadUpBit);
        if (button(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) bits |= pad::bit(pad::kDpadDownBit);
        if (button(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) bits |= pad::bit(pad::kDpadLeftBit);
        if (button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) bits |= pad::bit(pad::kDpadRightBit);
        if (button(SDL_CONTROLLER_BUTTON_START)) bits |= pad::bit(pad::kStartBit);
        if (button(SDL_CONTROLLER_BUTTON_BACK)) bits |= pad::bit(pad::kBackBit);
        if (button(SDL_CONTROLLER_BUTTON_LEFTSTICK)) bits |= pad::bit(pad::kLeftStickBit);
        if (button(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) bits |= pad::bit(pad::kRightStickBit);
        if (button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) bits |= pad::bit(pad::kLeftShoulderBit);
        if (button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) bits |= pad::bit(pad::kRightShoulderBit);
        if (button(SDL_CONTROLLER_BUTTON_GUIDE)) bits |= pad::bit(pad::kHomeBit);
        if (button(SDL_CONTROLLER_BUTTON_A)) bits |= pad::bit(pad::kSouthBit);
        if (button(SDL_CONTROLLER_BUTTON_B)) bits |= pad::bit(pad::kEastBit);
        if (button(SDL_CONTROLLER_BUTTON_X)) bits |= pad::bit(pad::kWestBit);
        if (button(SDL_CONTROLLER_BUTTON_Y)) bits |= pad::bit(pad::kNorthBit);
        const PadProfile& profile = pad_profile();
        if (profile.labels) {  // to positions: the right button is East, the bottom one South
            const std::uint32_t face = bits & (pad::bit(pad::kSouthBit) | pad::bit(pad::kEastBit) |
                                               pad::bit(pad::kWestBit) | pad::bit(pad::kNorthBit));
            bits &= ~face;
            if (face & pad::bit(pad::kSouthBit)) bits |= pad::bit(pad::kEastBit);
            if (face & pad::bit(pad::kEastBit)) bits |= pad::bit(pad::kSouthBit);
            if (face & pad::bit(pad::kWestBit)) bits |= pad::bit(pad::kNorthBit);
            if (face & pad::bit(pad::kNorthBit)) bits |= pad::bit(pad::kWestBit);
        }
        sample.buttons = bits;
        // The firmware's quit combination: MENU/Function (SDL's guide; KNULLI, muOS) or SELECT (ArkOS, dArkOS) +
        // Start ends the game cleanly (the launcher's gptokeyb kills on the same buttons if this does not answer).
        const bool quit_chord = button(SDL_CONTROLLER_BUTTON_START) &&
                                ((profile.quit_menu && button(SDL_CONTROLLER_BUTTON_GUIDE)) ||
                                 (profile.quit_select && button(SDL_CONTROLLER_BUTTON_BACK)));
        if (quit_chord && !galaxy::gx::host_window::quit_request.exchange(true, std::memory_order_acq_rel)) {
            std::fprintf(stderr, "[input] %s + Start: quit\n", button(SDL_CONTROLLER_BUTTON_GUIDE) ? "MENU" : "SELECT");
        }
        // SDL's Y axes grow downward; the unified layout's grow upward.
        sample.axes[0] = axis(gamepad, SDL_CONTROLLER_AXIS_LEFTX);
        sample.axes[1] = -axis(gamepad, SDL_CONTROLLER_AXIS_LEFTY);
        sample.axes[2] = axis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX);
        sample.axes[3] = -axis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY);
        sample.axes[4] = std::max(0.0f, axis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        sample.axes[5] = std::max(0.0f, axis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        if (profile.sticks < 2) {
            // Fewer sticks: the D-pad takes the missing one (the right stick, the pointer; or the left, Mario).
            const std::uint32_t dpad = pad::bit(pad::kDpadUpBit) | pad::bit(pad::kDpadDownBit) |
                                       pad::bit(pad::kDpadLeftBit) | pad::bit(pad::kDpadRightBit);
            const float x = float((sample.buttons & pad::bit(pad::kDpadRightBit)) != 0) -
                            float((sample.buttons & pad::bit(pad::kDpadLeftBit)) != 0);
            const float y = float((sample.buttons & pad::bit(pad::kDpadUpBit)) != 0) -
                            float((sample.buttons & pad::bit(pad::kDpadDownBit)) != 0);
            const float scale = x != 0.0f && y != 0.0f ? 0.7071f : 1.0f;
            const std::size_t ax = profile.sticks == 1 ? 2 : 0;
            if (x != 0.0f || y != 0.0f) {
                sample.axes[ax] = x * scale;
                sample.axes[ax + 1] = y * scale;
            }
            sample.buttons &= ~dpad;
        }
    }
}

void open_gamepad(int device_index) {
    if (SDL_IsGameController(device_index) != SDL_TRUE) {
        std::fprintf(stderr, "[input] joystick %d \"%s\" has no game controller mapping\n", device_index,
                     SDL_JoystickNameForIndex(device_index));
        return;
    }
    for (SDL_GameController*& slot : g_gamepads) {
        if (slot == nullptr) {
            slot = SDL_GameControllerOpen(device_index);
            if (slot != nullptr) {
                char guid[64] = {};
                SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device_index), guid, sizeof(guid));
                char* mapping = SDL_GameControllerMapping(slot);
                std::fprintf(stderr, "[input] gamepad \"%s\" connected (%s)\n", SDL_GameControllerName(slot),
                             mapping != nullptr ? mapping : guid);
                if (mapping != nullptr) SDL_free(mapping);
                (void)pad_profile();
            }
            return;
        }
    }
}

void close_gamepad(SDL_JoystickID id) {
    for (SDL_GameController*& slot : g_gamepads) {
        if (slot != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(slot)) == id) {
            SDL_GameControllerClose(slot);
            slot = nullptr;
        }
    }
}

void update_window_size() {
    int width = 0;
    int height = 0;
    if (g_window != nullptr) {
        SDL_GL_GetDrawableSize(g_window, &width, &height);
        if (width > 0 && height > 0) {
            g_input.window_width = width;
            g_input.window_height = height;
        }
    }
}

}  // namespace

bool start() noexcept {
    g_sdl_thread = std::this_thread::get_id();
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        std::fprintf(stderr, "[sdl] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    // PortMaster's controller database too (the firmware's own mapping comes in SDL_GAMECONTROLLERCONFIG).
    if (const char* db = std::getenv("NEBULA_GAMECONTROLLERDB"); db != nullptr && db[0] != '\0') {
        const int added = SDL_GameControllerAddMappingsFromFile(db);
        std::fprintf(stderr, "[input] %d mappings from %s\n", added, db);
    }
    // Sound and vibration are optional: the game runs silent without them.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[sdl] no sound: %s\n", SDL_GetError());
    }
    (void)SDL_InitSubSystem(SDL_INIT_HAPTIC);
    SDL_version version{};
    SDL_GetVersion(&version);
    std::fprintf(stderr, "[sdl] SDL %d.%d.%d, video driver %s, audio driver %s\n", version.major, version.minor,
                 version.patch, SDL_GetCurrentVideoDriver() != nullptr ? SDL_GetCurrentVideoDriver() : "-",
                 SDL_GetCurrentAudioDriver() != nullptr ? SDL_GetCurrentAudioDriver() : "-");
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        open_gamepad(i);
    }
    return true;
}

void run_requests() {
    const std::uint32_t pad = g_rumble_pad.load(std::memory_order_relaxed);
    const std::uint32_t level = g_rumble_level.load(std::memory_order_relaxed);
    const std::uint64_t now = galaxy::steady_ms();
    if (pad != g_rumble_sent_pad || level != g_rumble_sent_level || (level != 0u && now - g_rumble_sent_ms >= 50u)) {
        for (SDL_GameController* gamepad : g_gamepads) {
            if (gamepad == nullptr) {
                continue;
            }
            const std::uint32_t id =
                static_cast<std::uint32_t>(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad))) + 1u;
            if (id == pad || id == g_rumble_sent_pad) {
                const auto strength = static_cast<Uint16>(id == pad ? level : 0u);
                (void)SDL_GameControllerRumble(gamepad, strength, strength, strength != 0u ? 100u : 0u);
            }
        }
        g_rumble_sent_pad = pad;
        g_rumble_sent_level = level;
        g_rumble_sent_ms = now;
    }
}

void pump_events(int timeout_ms) noexcept {
    SDL_Event event;
    bool any = SDL_WaitEventTimeout(&event, timeout_ms) != 0;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        while (any) {
            switch (event.type) {
            case SDL_QUIT:
                galaxy::gx::host_window::quit_request.store(true, std::memory_order_release);
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    galaxy::gx::host_window::quit_request.store(true, std::memory_order_release);
                } else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    update_window_size();
                    galaxy::gx::host_window::pending_resize_extent.store(
                        (static_cast<std::uint64_t>(g_input.window_width) << 32u) |
                            static_cast<std::uint32_t>(g_input.window_height),
                        std::memory_order_release);
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    g_input.window_focused = true;
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    g_input.keys = {};
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                set_key(virtual_key(event.key.keysym.scancode), event.type == SDL_KEYDOWN);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                open_gamepad(event.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                close_gamepad(static_cast<SDL_JoystickID>(event.cdevice.which));
                break;
            default:
                break;
            }
            any = SDL_PollEvent(&event) != 0;
        }
        sample_gamepads();
    }
    run_requests();
}

void show_fatal(const char* message) noexcept {
    std::fprintf(stderr, "[fatal] %s\n", message);
    if (const std::filesystem::path data = galaxy::nebula_data_directory(); !data.empty()) {
        if (std::FILE* file = galaxy::open_path(data / "last_error.txt", "w")) {
            std::fputs(message, file);
            std::fclose(file);
        }
    }
}

void set_menu_context(bool menu) noexcept {
    g_menu_context.store(menu, std::memory_order_relaxed);
}

bool in_background() noexcept {
    return false;
}

InputState input_state() noexcept {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    return g_input;
}

void rumble(std::uint32_t pad_id, float strength) noexcept {
    g_rumble_level.store(static_cast<std::uint32_t>(std::clamp(strength, 0.0f, 1.0f) * 65535.0f),
                         std::memory_order_relaxed);
    g_rumble_pad.store(pad_id, std::memory_order_relaxed);
}

// No HOME Menu screen here: a choice of "return" at once (the PortMaster hotkey quits).
void home_menu_show() noexcept {
    g_home_visible.store(true, std::memory_order_release);
}

void home_menu_hide() noexcept {
    g_home_visible.store(false, std::memory_order_release);
}

int home_menu_choice() noexcept {
    return g_home_visible.load(std::memory_order_acquire) ? 0 : -1;
}

}  // namespace galaxy::sdl

// ---------------------------------------------------------------------------------------------------------------------
// The renderer's window (host_window.h)
// ---------------------------------------------------------------------------------------------------------------------

namespace galaxy::gx::host_window {

unsigned backbuffer_width = 0;
unsigned backbuffer_height = 0;
std::atomic<double> measured_present_hz{0.0};
std::string native_settings_content_root;
std::atomic<bool> borderless_active{true};
std::atomic<bool> exclusive_fullscreen_active{false};
std::atomic<unsigned> efb_scale_rejected{0u};
std::atomic<bool> move_size_active{false};
std::atomic<bool> quit_request{false};
std::atomic<std::uint64_t> pending_resize_extent{0u};
std::atomic<bool> surface_withdrawn{false};
std::timed_mutex surface_use;
std::atomic<bool> follow_window_rearm{false};

unsigned display_efb_scale_cap() {
    SDL_DisplayMode mode{};
    if (SDL_GetCurrentDisplayMode(0, &mode) != 0 || mode.w <= 0 || mode.h <= 0) {
        return kMaxEfbScale;
    }
    return efb_scale_covering(static_cast<unsigned>(mode.w), static_cast<unsigned>(mode.h), kMaxEfbScale);
}

bool flush_native_settings_if_dirty() {
    return true;
}

void poll_render_config_file() {}

bool hidden_window_mode_enabled() {
    const char* value = std::getenv("GALAXY_HIDE_WINDOW");
    return value != nullptr && value[0] == '1';
}

OpenedWindow open_game_window(WindowOwner& owner, int window_width, int window_height) {
    SDL_DisplayMode mode{};
    const bool have_mode = SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0;
    // The handheld's screen whole (the fbdev driver has one window, the display's size).
    const int width = have_mode ? mode.w : std::max(window_width, 320);
    const int height = have_mode ? mode.h : std::max(window_height, 240);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (std::getenv("NEBULA_WINDOWED") == nullptr) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_Window* window = SDL_CreateWindow("Nebula", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height,
                                          flags);
    if (window == nullptr) {
        std::fprintf(stderr, "[window] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return {};
    }
    SDL_ShowCursor(SDL_DISABLE);
    {
        std::lock_guard<std::mutex> lock(galaxy::sdl::g_input_mutex);
        galaxy::sdl::g_window = window;
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        galaxy::sdl::g_input.window_width = w;
        galaxy::sdl::g_input.window_height = h;
        backbuffer_width = static_cast<unsigned>(w);
        backbuffer_height = static_cast<unsigned>(h);
    }
    owner.window = window;
    std::fprintf(stderr, "[window] %dx%d%s\n", width, height, (flags & SDL_WINDOW_FULLSCREEN) != 0u ? " full screen" : "");
    return {window, true};
}

void close_game_window(WindowOwner& owner) noexcept {
    if (owner.window != nullptr) {
        SDL_DestroyWindow(static_cast<SDL_Window*>(owner.window));
        owner.window = nullptr;
    }
    galaxy::sdl::g_window = nullptr;
}

void show_window(NativeWindow window) noexcept {
    SDL_ShowWindow(static_cast<SDL_Window*>(window));
}

double display_refresh_hz(NativeWindow) noexcept {
    SDL_DisplayMode mode{};
    if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.refresh_rate > 1) {
        return static_cast<double>(mode.refresh_rate);
    }
    return 60.0;
}

bool vblank_grid(NativeWindow, std::int64_t& vblank, std::int64_t& period) noexcept {
    return galaxy::sdl::vsync_grid(vblank, period);
}

bool present_grid(std::int64_t& retrace_ns, std::int64_t& period_ns, std::int64_t& vsync_ns,
                  std::int64_t& now_ns) noexcept {
    return galaxy::sdl::display_grid(retrace_ns, period_ns, vsync_ns, now_ns);
}

void draw_native_settings_overlay(UiCanvas&) {}
void draw_home_menu(UiCanvas&) {}
void draw_native_settings_hint(UiCanvas&) {}

std::string driver_log() {
    return {};
}

// ---- OpenGL ES.

bool gles_create_context(NativeWindow window) {
    auto* sdl_window = static_cast<SDL_Window*>(window);
    // 3.2, else 3.1 (the renderer needs 3.1).
    for (const int minor : {2, 1}) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        galaxy::sdl::g_context = SDL_GL_CreateContext(sdl_window);
        if (galaxy::sdl::g_context != nullptr) {
            break;
        }
        std::fprintf(stderr, "[window] no OpenGL ES 3.%d context: %s\n", minor, SDL_GetError());
    }
    if (galaxy::sdl::g_context == nullptr) {
        return false;
    }
    return SDL_GL_MakeCurrent(sdl_window, galaxy::sdl::g_context) == 0;
}

bool gles_make_current(NativeWindow window, bool current) noexcept {
    if (!current) {
        return SDL_GL_MakeCurrent(galaxy::sdl::g_window, nullptr) == 0;
    }
    auto* sdl_window = window != nullptr ? static_cast<SDL_Window*>(window) : galaxy::sdl::g_window;
    if (SDL_GL_MakeCurrent(sdl_window, galaxy::sdl::g_context) != 0) {
        std::fprintf(stderr, "[window] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void gles_destroy_context(NativeWindow) noexcept {
    if (galaxy::sdl::g_context != nullptr) {
        SDL_GL_MakeCurrent(galaxy::sdl::g_window, nullptr);
        SDL_GL_DeleteContext(galaxy::sdl::g_context);
        galaxy::sdl::g_context = nullptr;
    }
}

void* gles_proc_address(const char* name) noexcept {
    return SDL_GL_GetProcAddress(name);
}

void gles_swap_buffers(NativeWindow window) noexcept {
    SDL_GL_SwapWindow(static_cast<SDL_Window*>(window));
}

void gles_set_swap_interval(int interval) noexcept {
    if (SDL_GL_SetSwapInterval(interval) != 0) {
        std::fprintf(stderr, "[window] swap interval %d refused: %s\n", interval, SDL_GetError());
    }
}

void gles_drawable_size(NativeWindow window, int& width, int& height) noexcept {
    SDL_GL_GetDrawableSize(static_cast<SDL_Window*>(window), &width, &height);
}

namespace {

// EGL as SDL loaded it (the same library and display).
struct Egl {
    void* library = nullptr;
    PFNEGLGETCURRENTDISPLAYPROC get_current_display = nullptr;
    PFNEGLGETCURRENTCONTEXTPROC get_current_context = nullptr;
    PFNEGLQUERYCONTEXTPROC query_context = nullptr;
    PFNEGLCHOOSECONFIGPROC choose_config = nullptr;
    PFNEGLCREATECONTEXTPROC create_context = nullptr;
    PFNEGLDESTROYCONTEXTPROC destroy_context = nullptr;
    PFNEGLCREATEPBUFFERSURFACEPROC create_pbuffer = nullptr;
    PFNEGLDESTROYSURFACEPROC destroy_surface = nullptr;
    PFNEGLMAKECURRENTPROC make_current = nullptr;
    PFNEGLBINDAPIPROC bind_api = nullptr;
    PFNEGLGETERRORPROC get_error = nullptr;
    PFNEGLQUERYSTRINGPROC query_string = nullptr;
    PFNEGLRELEASETHREADPROC release_thread = nullptr;
    bool ok = false;
};

const Egl& egl() {
    static const Egl loaded = [] {
        Egl e;
        for (const char* name : {"libEGL.so.1", "libEGL.so"}) {
            e.library = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
            if (e.library == nullptr) {
                e.library = dlopen(name, RTLD_NOW);
            }
            if (e.library != nullptr) {
                break;
            }
        }
        if (e.library == nullptr) {
            return e;
        }
        const auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(e.library, name));
            return fn != nullptr;
        };
        e.ok = load(e.get_current_display, "eglGetCurrentDisplay") &&
            load(e.get_current_context, "eglGetCurrentContext") && load(e.query_context, "eglQueryContext") &&
            load(e.choose_config, "eglChooseConfig") && load(e.create_context, "eglCreateContext") &&
            load(e.destroy_context, "eglDestroyContext") && load(e.create_pbuffer, "eglCreatePbufferSurface") &&
            load(e.destroy_surface, "eglDestroySurface") && load(e.make_current, "eglMakeCurrent") &&
            load(e.bind_api, "eglBindAPI") && load(e.get_error, "eglGetError") &&
            load(e.query_string, "eglQueryString");
        load(e.release_thread, "eglReleaseThread");
        return e;
    }();
    return loaded;
}

struct WorkerContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
};

}  // namespace

void* gles_create_worker_context() noexcept {
    if (std::getenv("NEBULA_NO_GL_WORKERS") != nullptr) {
        return nullptr;
    }
    const Egl& e = egl();
    if (!e.ok) {
        std::fprintf(stderr, "[window] no EGL for shader workers\n");
        return nullptr;
    }
    const EGLDisplay display = e.get_current_display();
    const EGLContext shared = e.get_current_context();
    if (display == EGL_NO_DISPLAY || shared == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "[window] the window's context is not an EGL one: no shader workers\n");
        return nullptr;
    }
    EGLint config_id = 0;
    e.query_context(display, shared, EGL_CONFIG_ID, &config_id);
    // A pbuffer-capable configuration like the window's (Mali's fbdev EGL needs a surface to make a context current).
    EGLConfig config = nullptr;
    EGLint count = 0;
    const EGLint by_id[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
    e.choose_config(display, by_id, &config, 1, &count);
    const EGLint pbuffer_config[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                     EGL_NONE};
    EGLConfig pbuffer = nullptr;
    EGLint pbuffer_count = 0;
    e.choose_config(display, pbuffer_config, &pbuffer, 1, &pbuffer_count);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    auto* worker = new WorkerContext{};
    worker->display = display;
    // The worker's context shares objects only with a context of a compatible configuration: the window's first.
    for (EGLConfig candidate : {count > 0 ? config : nullptr, pbuffer_count > 0 ? pbuffer : nullptr}) {
        if (candidate == nullptr) {
            continue;
        }
        worker->context = e.create_context(display, candidate, shared, context_attributes);
        if (worker->context == EGL_NO_CONTEXT) {
            continue;
        }
        const EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        worker->surface = e.create_pbuffer(display, candidate, size);
        const char* extensions = e.query_string(display, EGL_EXTENSIONS);
        const bool surfaceless = extensions != nullptr && std::strstr(extensions, "EGL_KHR_surfaceless_context");
        if (worker->surface != EGL_NO_SURFACE || surfaceless) {
            return worker;
        }
        e.destroy_context(display, worker->context);
        worker->context = EGL_NO_CONTEXT;
    }
    std::fprintf(stderr, "[window] no shared EGL context for a shader worker (EGL error 0x%x)\n", e.get_error());
    delete worker;
    return nullptr;
}

bool gles_make_worker_current(void* context) noexcept {
    const Egl& e = egl();
    if (!e.ok) {
        return false;
    }
    e.bind_api(EGL_OPENGL_ES_API);
    if (context == nullptr) {
        const EGLDisplay display = e.get_current_display();
        if (display != EGL_NO_DISPLAY) {
            e.make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (e.release_thread != nullptr) {
            e.release_thread();
        }
        return true;
    }
    auto* worker = static_cast<WorkerContext*>(context);
    if (e.make_current(worker->display, worker->surface, worker->surface, worker->context) != EGL_TRUE) {
        return false;
    }
    // The worker's GL entry points are the ones the renderer loaded (one driver, one share group).
    return true;
}

void gles_destroy_worker_context(void* context) noexcept {
    auto* worker = static_cast<WorkerContext*>(context);
    if (worker == nullptr) {
        return;
    }
    const Egl& e = egl();
    if (e.ok) {
        if (worker->surface != EGL_NO_SURFACE) {
            e.destroy_surface(worker->display, worker->surface);
        }
        if (worker->context != EGL_NO_CONTEXT) {
            e.destroy_context(worker->display, worker->context);
        }
    }
    delete worker;
}

}  // namespace galaxy::gx::host_window

// ---------------------------------------------------------------------------------------------------------------------
// The window functions the host and the backend call (gx_d3d12.h)
// ---------------------------------------------------------------------------------------------------------------------

namespace galaxy::gx {

void set_native_settings_content_root(const std::string& utf8_path) {
    host_window::native_settings_content_root = utf8_path;
}

HostClientExtent current_host_client_extent() noexcept {
    const galaxy::sdl::InputState state = galaxy::sdl::input_state();
    return {state.window_width, state.window_height};
}

HostClientExtent aspect_latch_extent() noexcept {
    return current_host_client_extent();
}

HostPointerState poll_host_pointer_state() noexcept {
    const galaxy::sdl::InputState state = galaxy::sdl::input_state();
    HostPointerState pointer{};
    pointer.window_focused = state.window_focused;
    pointer.inside_client = true;
    pointer.absolute_valid = false;
    pointer.client_width = state.window_width;
    pointer.client_height = state.window_height;
    return pointer;
}

host::HostPointerTransitionPoll poll_host_pointer_transition(std::uint64_t) noexcept {
    return {};
}

bool native_settings_overlay_visible() noexcept {
    return false;
}

bool native_window_input_captured() noexcept {
    return false;
}

void home_menu_open() noexcept {
    galaxy::sdl::home_menu_show();
}

int home_menu_poll() noexcept {
    return galaxy::sdl::home_menu_choice();
}

void home_menu_close() noexcept {
    galaxy::sdl::home_menu_hide();
}

bool home_menu_visible() noexcept {
    return galaxy::sdl::g_home_visible.load(std::memory_order_acquire);
}

void native_settings_post_key(std::uintptr_t) noexcept {}

}  // namespace galaxy::gx
