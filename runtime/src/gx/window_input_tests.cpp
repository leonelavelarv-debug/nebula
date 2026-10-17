// The game window's input handling on its own thread, read the way the simulation thread's
// input poll reads it (poll_host_pointer_state, poll_host_pointer_transition, sampled_host_cursor):
// pointer coordinates and button edges posted to the window arrive unchanged, the time from the
// message to the poll seeing it, the cursor hidden over the client area while the game is active,
// and (--timer) how old the window thread's cursor sample is when the poll reads it.
#include "galaxy/gx_d3d12.h"
#include "galaxy/gx/window_owner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

double now_us() {
    return std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Spins on the reading thread, as the input poll would see successive reports, until `ready`
// holds or a second passes; returns the microseconds it took, or a negative value on timeout.
template <typename Ready>
double wait_until(Ready ready) {
    const double start = now_us();
    while (!ready()) {
        if (now_us() - start > 1'000'000.0) {
            return -1.0;
        }
        std::this_thread::yield();
    }
    return now_us() - start;
}

bool run_messages(HWND window, int width, int height) {
    bool passed = true;
    // Pointer moves: the poll reports each posted coordinate as it was sent.
    const POINT points[] = {{10, 20}, {width / 2, height / 2}, {width - 1, height - 1}, {0, 0}};
    double worst_us = 0.0;
    for (const POINT point : points) {
        passed &= expect(PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y)) != FALSE,
            "WM_MOUSEMOVE posted");
        galaxy::gx::HostPointerState state{};
        const double us = wait_until([&] {
            state = galaxy::gx::poll_host_pointer_state();
            return state.absolute_valid && state.client_x == point.x && state.client_y == point.y;
        });
        passed &= expect(us >= 0.0, "the poll reports the posted pointer coordinate");
        // inside_client is not checked: the real cursor is not over this hidden window, so the
        // system follows each tracked move with WM_MOUSELEAVE.
        passed &= expect(state.client_width == width && state.client_height == height,
            "the reported pointer comes with the window's client size");
        const std::uint64_t age_ms = GetTickCount64() - state.absolute_acquired_ms;
        passed &= expect(age_ms <= 32u, "the reported pointer's acquisition time is the message's");
        galaxy::gx::HostCursorSample sample{};
        passed &= expect(galaxy::gx::sampled_host_cursor(sample) &&
                sample.client_x == point.x && sample.client_y == point.y && sample.over_window &&
                sample.window == reinterpret_cast<std::uintptr_t>(window),
            "the window thread's cursor sample holds the message coordinate");
        worst_us = std::max(worst_us, us);
    }
    std::printf("pointer move: posted coordinate visible to the poll within %.0f us (worst of %zu)\n",
        worst_us, std::size(points));

    // With the cursor timer off nothing refreshes the sample, and one older than 250 ms is none.
    Sleep(320);
    galaxy::gx::HostCursorSample stale{};
    passed &= expect(!galaxy::gx::sampled_host_cursor(stale),
        "a cursor sample older than 250 ms counts as none");

    // Activation, then the cursor over the client area: the window thread hides it.
    (void)SendMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
    passed &= expect(SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
                         MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) == TRUE,
        "WM_SETCURSOR over the client area hides the cursor while the game is active");
    passed &= expect(galaxy::gx::poll_host_pointer_state().window_focused,
        "the poll reports the window focused after activation");

    // Button edges in order, each with the coordinate it was pressed or released at.
    const std::uint64_t before = galaxy::gx::poll_host_pointer_transition(0u).latest_sequence;
    passed &= expect(PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 100)) != FALSE &&
            PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(201, 101)) != FALSE,
        "button messages posted");
    galaxy::host::HostPointerTransitionPoll down{};
    const double down_us = wait_until([&] {
        down = galaxy::gx::poll_host_pointer_transition(before);
        return down.result == galaxy::host::HostPointerTransitionReadResult::Ready;
    });
    passed &= expect(down_us >= 0.0 &&
            down.transition.kind == galaxy::host::HostPointerTransitionKind::LeftDown &&
            down.transition.coordinates.client_x == 200 && down.transition.coordinates.client_y == 100,
        "the left press reaches the poll with its coordinate");
    galaxy::host::HostPointerTransitionPoll up{};
    const double up_us = wait_until([&] {
        up = galaxy::gx::poll_host_pointer_transition(down.transition.sequence);
        return up.result == galaxy::host::HostPointerTransitionReadResult::Ready;
    });
    passed &= expect(up_us >= 0.0 &&
            up.transition.kind == galaxy::host::HostPointerTransitionKind::LeftUp &&
            up.transition.coordinates.client_x == 201 && up.transition.coordinates.client_y == 101,
        "the left release follows it with its own coordinate");
    std::printf("buttons: press visible to the poll within %.0f us, release %.0f us after\n",
        down_us, up_us);

    // Deactivated, the window leaves WM_SETCURSOR to the system (the class has no cursor here).
    (void)SendMessageW(window, WM_ACTIVATEAPP, FALSE, 0);
    passed &= expect(SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
                         MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) == FALSE,
        "after deactivation the window no longer hides the cursor");
    return passed;
}

bool run_timer() {
    // The window thread samples the OS cursor on a USER_TIMER_MINIMUM timer. Read as often as the
    // input poll would (every 10 ms, 100 reads), the sample's age shows the pointer latency the
    // timer adds when no WM_MOUSEMOVE arrives. Only reads the cursor; nothing moves it.
    std::vector<std::uint64_t> ages;
    for (int i = 0; i < 100; ++i) {
        Sleep(10);
        galaxy::gx::HostCursorSample sample{};
        if (galaxy::gx::sampled_host_cursor(sample)) {
            ages.push_back(GetTickCount64() - sample.sampled_ms);
        }
    }
    bool passed = expect(ages.size() == 100u, "every read finds a recent cursor sample");
    if (ages.empty()) {
        return false;
    }
    std::sort(ages.begin(), ages.end());
    std::printf("cursor sample age at the poll: median %llu ms, p90 %llu ms, max %llu ms (%zu reads)\n",
        static_cast<unsigned long long>(ages[ages.size() / 2]),
        static_cast<unsigned long long>(ages[ages.size() * 9 / 10]),
        static_cast<unsigned long long>(ages.back()), ages.size());
    passed &= expect(ages.back() <= 50u, "the timer keeps the cursor sample under 50 ms old");
    return passed;
}

}  // namespace

int main(int argc, char** argv) {
    const bool timer = argc == 2 && std::strcmp(argv[1], "--timer") == 0;
    // Settings live in a private directory; the timer test samples the cursor, the message test
    // must not (a sample of the real cursor would replace the posted coordinates).
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("galaxy_window_input_tests_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    _putenv_s("GALAXY_RENDER_CONFIG_PATH", (root / "render.ini").string().c_str());
    _putenv_s("GALAXY_RUNTIME_SETTINGS_PATH", (root / "runtime.ini").string().c_str());
    _putenv_s("GALAXY_HIDE_WINDOW", "");
    _putenv_s("GALAXY_HIDE_OS_CURSOR", "1");
    _putenv_s("GALAXY_MOUSE_IR_CURSOR_POLL", timer ? "auto" : "0");

    WNDCLASSW window_class{};
    window_class.lpfnWndProc =
        reinterpret_cast<WNDPROC>(galaxy::gx::native_window_procedure_for_tests());
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"GalaxyWindowInputTest";
    if (!RegisterClassW(&window_class)) {
        std::fprintf(stderr, "FAILED: register the window class\n");
        return 1;
    }
    bool passed = true;
    {
        galaxy::gx::WindowOwner owner;
        const HWND window = owner.start({window_class.lpszClassName, L"Window input test",
            WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 0, 0, 656, 519, false});
        passed &= expect(window != nullptr, "hidden game window created on its own thread");
        RECT client{};
        if (window != nullptr && GetClientRect(window, &client)) {
            passed &= timer ? run_timer()
                            : run_messages(window, client.right - client.left, client.bottom - client.top);
        }
        owner.stop();
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::printf(passed ? "window input: all checks passed\n" : "window input: FAILED\n");
    return passed ? 0 : 1;
}
