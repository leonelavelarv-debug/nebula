// SPDX-License-Identifier: GPL-3.0-only
// System, memory and CPU measurements for the Nebula -> RG35XX H feasibility probe.
#include "probe.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
long field_kib(const std::string& text, const char* key) {
    const auto at = text.find(key);
    if (at == std::string::npos) return -1;
    return std::strtol(text.c_str() + at + std::strlen(key), nullptr, 10);
}
}  // namespace

std::string read_file(const char* path, std::size_t limit) {
    std::string out;
    if (FILE* f = std::fopen(path, "rb")) {
        char buf[4096];
        std::size_t n;
        while (out.size() < limit && (n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    if (out.size() > limit) out.resize(limit);
    return out;
}

MemSnapshot mem_snapshot() {
    MemSnapshot s;
    const std::string mi = read_file("/proc/meminfo");
    s.mem_total_kib = field_kib(mi, "MemTotal:");
    s.mem_available_kib = field_kib(mi, "MemAvailable:");
    if (s.mem_available_kib < 0) s.mem_available_kib = field_kib(mi, "MemFree:") + field_kib(mi, "Cached:");
    s.swap_total_kib = field_kib(mi, "SwapTotal:");
    s.swap_free_kib = field_kib(mi, "SwapFree:");
    const std::string st = read_file("/proc/self/status");
    s.rss_kib = field_kib(st, "VmRSS:");
    s.hwm_kib = field_kib(st, "VmHWM:");
    std::string roll = read_file("/proc/self/smaps_rollup");
    if (!roll.empty()) {
        s.pss_kib = field_kib(roll, "\nPss:");
    } else {  // kernels before 4.14 (the H700 BSP kernel may be one): sum smaps
        const std::string sm = read_file("/proc/self/smaps", 64u << 20);
        long total = 0; std::size_t pos = 0;
        while ((pos = sm.find("\nPss:", pos)) != std::string::npos) { total += std::strtol(sm.c_str() + pos + 5, nullptr, 10); pos += 5; }
        s.pss_kib = total;
    }
    return s;
}

// Nebula maps the Wii's address space flat: one reservation, MEM1 (24 MiB) and MEM2 (64 MiB) committed at their
// console offsets, and the GX command ring (16 MiB) mapped twice back to back so a command never wraps.
GuestMemReport run_guest_memory_test(std::size_t module_estimate_mib) {
    GuestMemReport r;
    char line[256];
    const MemSnapshot before = mem_snapshot();
    const std::size_t k4g = std::size_t{1} << 32;
    void* base = mmap(nullptr, k4g, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    r.reserve_4g = base != MAP_FAILED;
    std::snprintf(line, sizeof line, "  reserva 4 GiB de direcciones (PROT_NONE): %s\n", r.reserve_4g ? "OK" : std::strerror(errno));
    r.text += line;
    if (r.reserve_4g) {
        auto* b = static_cast<unsigned char*>(base);
        r.commit_mem1 = mprotect(b, 24u << 20, PROT_READ | PROT_WRITE) == 0;
        r.commit_mem2 = mprotect(b + 0x10000000u, 64u << 20, PROT_READ | PROT_WRITE) == 0;
        const double t0 = now();
        if (r.commit_mem1) for (std::size_t i = 0; i < (24u << 20); i += 4096) b[i] = 1;
        if (r.commit_mem2) for (std::size_t i = 0; i < (64u << 20); i += 4096) b[0x10000000u + i] = 1;
        r.touch_ms = 1e3 * (now() - t0);
        std::snprintf(line, sizeof line, "  MEM1 24 MiB %s, MEM2 64 MiB %s, primer toque %.1f ms\n",
                      r.commit_mem1 ? "OK" : "FALLO", r.commit_mem2 ? "OK" : "FALLO", r.touch_ms);
        r.text += line;
    }
    // Double-mapped ring.
    const std::size_t ring = 16u << 20;
    const double t1 = now();
    int fd = static_cast<int>(syscall(SYS_memfd_create, "nebula-gx-ring", 0u));
    if (fd >= 0 && ftruncate(fd, static_cast<off_t>(ring)) == 0) {
        void* span = mmap(nullptr, 2 * ring, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (span != MAP_FAILED) {
            auto* s = static_cast<unsigned char*>(span);
            void* a = mmap(s, ring, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
            void* c = mmap(s + ring, ring, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
            if (a != MAP_FAILED && c != MAP_FAILED) {
                for (std::size_t i = 0; i < ring; i += 4096) s[i] = 2;
                s[ring - 1] = 0x5A; s[0] = 0xA5;
                r.ring_alias = s[2 * ring - 1] == 0x5A && s[ring] == 0xA5;
            }
            r.ring_ms = 1e3 * (now() - t1);
            const MemSnapshot mid = mem_snapshot();
            r.pss_growth_kib = mid.pss_kib - before.pss_kib;
            r.available_after_kib = mid.mem_available_kib;
            // Generated game module: tens of MiB of code. Its text is file-backed and evictable on the device; an
            // anonymous touched block is the pessimistic stand-in.
            if (module_estimate_mib > 0) {
                const std::size_t msize = module_estimate_mib << 20;
                void* m = mmap(nullptr, msize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (m != MAP_FAILED) {
                    for (std::size_t i = 0; i < msize; i += 4096) static_cast<unsigned char*>(m)[i] = 3;
                    r.module_sim = true;
                    r.available_with_module_kib = mem_snapshot().mem_available_kib;
                    munmap(m, msize);
                }
            }
            munmap(span, 2 * ring);
        }
    }
    if (fd >= 0) close(fd);
    std::snprintf(line, sizeof line, "  anillo GX 16 MiB doble mapeo (memfd): %s, %.1f ms\n", r.ring_alias ? "OK" : "FALLO", r.ring_ms);
    r.text += line;
    std::snprintf(line, sizeof line, "  PSS +%ld KiB; MemAvailable despues: %ld MiB; con modulo simulado de %zu MiB: %ld MiB\n",
                  r.pss_growth_kib, r.available_after_kib / 1024, module_estimate_mib, r.available_with_module_kib / 1024);
    r.text += line;
    if (r.reserve_4g) munmap(base, k4g);
    return r;
}

namespace {
__attribute__((noinline)) std::uint32_t hash_iter(std::uint32_t x, std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x *= 0x9E3779B9u; }
    return x;
}
}  // namespace

CpuReport run_cpu_info_and_bench() {
    CpuReport r;
    const std::string ci = read_file("/proc/cpuinfo", 16384);
    for (const char* key : {"model name", "Hardware", "CPU part"}) {
        const auto at = ci.find(key);
        if (at != std::string::npos) {
            const auto colon = ci.find(':', at), end = ci.find('\n', at);
            r.model += (r.model.empty() ? "" : " / ") + std::string(key) + "=" + ci.substr(colon + 2, end - colon - 2);
        }
    }
    r.cores = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    for (int c = 0; c < r.cores && c < 8; ++c) {
        char p[128];
        std::snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c); r.cur_khz[c] = std::atol(read_file(p).c_str());
        std::snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); r.max_khz[c] = std::atol(read_file(p).c_str());
    }
    r.governor = read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    while (!r.governor.empty() && (r.governor.back() == '\n')) r.governor.pop_back();
    for (int z = 0; z < 6; ++z) {
        char p[96]; std::snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        std::string t = read_file(p);
        if (!t.empty()) { char b[32]; std::snprintf(b, sizeof b, "%s%.1fC", r.thermal.empty() ? "" : " ", std::atol(t.c_str()) / 1000.0); r.thermal += b; }
    }
    // memcpy bandwidth, 32 MiB.
    const std::size_t n = 32u << 20;
    std::vector<unsigned char> a(n, 1), b(n, 2);
    const double t0 = now();
    for (int i = 0; i < 8; ++i) { a[i] = static_cast<unsigned char>(i); std::memcpy(b.data(), a.data(), n); asm volatile("" : : "r"(b.data()), "r"(a.data()) : "memory"); }
    r.memcpy_gbs = 8.0 * n / (now() - t0) / 1e9;
    volatile unsigned char keep = b[n / 2]; (void)keep;
    // Single thread, then every core at once.
    const std::uint32_t iters = 20000000;
    double t1 = now(); volatile std::uint32_t sink = hash_iter(1, iters); (void)sink;
    r.single_hash_ns = 1e9 * (now() - t1) / iters;
    r.threads = r.cores;
    std::vector<std::thread> pool;
    std::atomic<std::uint32_t> acc{0};
    const double t2 = now();
    for (int t = 0; t < r.threads; ++t) pool.emplace_back([&, t] { acc += hash_iter(t + 1, iters); });
    for (auto& th : pool) th.join();
    r.multi_hash_ns = 1e9 * (now() - t2) / iters;
    return r;
}

VkReport run_vulkan_probe() {
    VkReport r;
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    r.loader = lib != nullptr;
    if (!lib) { r.text = "  libvulkan.so.1 no existe: el backend Vulkan de Nebula no es opcion; usar GLES.\n"; return r; }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "nebula-probe", 1, "nebula", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    VkInstance inst = VK_NULL_HANDLE;
    const VkResult res = create ? create(&ci, nullptr, &inst) : VK_ERROR_INITIALIZATION_FAILED;
    r.instance = res == VK_SUCCESS;
    char line[512];
    if (!r.instance) { std::snprintf(line, sizeof line, "  cargador Vulkan presente, vkCreateInstance fallo (%d)\n", int(res)); r.text = line; return r; }
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    std::uint32_t count = 0;
    if (enumerate) enumerate(inst, &count, nullptr);
    std::vector<VkPhysicalDevice> devs(count);
    if (count) enumerate(inst, &count, devs.data());
    r.devices = count;
    std::snprintf(line, sizeof line, "  Vulkan: %u dispositivo(s)\n", count); r.text = line;
    for (auto d : devs) {
        VkPhysicalDeviceProperties p{}; props(d, &p);
        std::snprintf(line, sizeof line, "    %s  API %u.%u.%u  tipo %d\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                      VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), int(p.deviceType));
        r.text += line;
    }
    if (destroy) destroy(inst, nullptr);
    return r;
}

std::string sys_summary() {
    std::string s;
    utsname u{};
    if (uname(&u) == 0) { s += std::string("  kernel: ") + u.sysname + " " + u.release + " " + u.machine + "\n"; }
    const std::string osr = read_file("/etc/os-release", 2048);
    const auto at = osr.find("PRETTY_NAME=");
    if (at != std::string::npos) s += "  sistema: " + osr.substr(at + 12, osr.find('\n', at) - at - 12) + "\n";
    if (const char* cfw = std::getenv("CFW_NAME")) s += std::string("  CFW (PortMaster): ") + cfw + "\n";
    if (const char* dev = std::getenv("DEVICE_NAME")) s += std::string("  dispositivo (PortMaster): ") + dev + "\n";
    const std::string model = read_file("/proc/device-tree/model", 256);
    if (!model.empty()) s += "  device-tree: " + std::string(model.c_str()) + "\n";
    return s;
}
