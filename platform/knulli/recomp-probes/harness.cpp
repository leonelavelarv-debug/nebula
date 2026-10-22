// Runs PowerPC probes lowered by Nebula's translator (nebula-recomp-core::lower_ppc_code_range) on the host
// CPU, checks their results against native C references and times both. Build for x86-64 and aarch64.
#include "galaxy/native_api.h"
#include "galaxy/ppc_float.h"
#include <chrono>
#include <cinttypes>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace galaxy;
#define PROBE(name) \
    extern "C" __attribute__((noinline)) void probe_##name(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services) { \
        _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wunused-label\"")
#define END _Pragma("clang diagnostic pop") }
PROBE(sum_words)
#include "gen/sum_words.inc"
END
PROBE(dot_f32)
#include "gen/dot_f32.inc"
END
PROBE(hash_loop)
#include "gen/hash_loop.inc"
END
PROBE(div_edge)
#include "gen/div_edge.inc"
END
PROBE(memcpy_words)
#include "gen/memcpy_words.inc"
END

PROBE(ps_loop)
#include "gen/ps_loop.inc"
END
PROBE(fctiwz_one)
#include "gen/fctiwz_one.inc"
END

static void svc_fatal(void*, std::uint32_t pc, const char* m) { std::fprintf(stderr, "FATAL pc=%08X %s\n", pc, m); std::exit(3); }
static void svc_log(void*, LogLevelV1, const char* m) { std::fprintf(stderr, "log: %s\n", m); }
static std::uint64_t svc_tb(void*) { return 0; }
static std::uint64_t g_ckpt_counter = 0, g_ckpt_next = ~0ull, g_ckpt_slow = 0;
static std::atomic_uint32_t g_pending{0};
static void svc_checkpoint(void*, std::uint32_t, PpcContext*, GuestMemoryV1*) { ++g_ckpt_slow; }

static constexpr std::uint32_t kMem1 = 24u << 20;
static std::byte* g_mem1;
static GuestMemoryV1 g_mem;
static GuestMemoryRegionV1 g_region;
static NativeServicesV1 g_svc;

static std::uint32_t be32(std::uint32_t a) { std::uint32_t v; std::memcpy(&v, g_mem1 + (a - 0x80000000u), 4); return byte_swap_u32(v); }
static void wbe32(std::uint32_t a, std::uint32_t v) { v = byte_swap_u32(v); std::memcpy(g_mem1 + (a - 0x80000000u), &v, 4); }
static void wf32(std::uint32_t a, float f) { std::uint32_t v; std::memcpy(&v, &f, 4); wbe32(a, v); }
static float f1_single(const PpcContext& c) { double d; std::memcpy(&d, &c.fpr_bits[1], 8); return static_cast<float>(d); }

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Native references (what a hand port would do).
__attribute__((noinline)) static std::uint32_t ref_sum(const std::uint32_t* p, std::uint32_t n) { std::uint32_t s = 0; for (std::uint32_t i = 0; i < n; ++i) s += __builtin_bswap32(p[i]); return s; }
__attribute__((noinline)) static std::uint32_t ref_hash(std::uint32_t x, std::uint32_t n) { for (std::uint32_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x *= 0x9E3779B9u; } return x; }
__attribute__((noinline)) static float ref_dot(const std::uint32_t* x, const std::uint32_t* y, std::uint32_t n) { float s = 0; for (std::uint32_t i = 0; i < n; ++i) { std::uint32_t a = __builtin_bswap32(x[i]), b = __builtin_bswap32(y[i]); float fa, fb; std::memcpy(&fa, &a, 4); std::memcpy(&fb, &b, 4); s = fa * fb + s; } return s; }

int main(int argc, char** argv) {
    const std::uint32_t scale = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1;
    g_mem1 = static_cast<std::byte*>(std::calloc(kMem1, 1));
    g_region = {0x80000000u, kMem1, g_mem1};
    g_mem.region_count = 1; g_mem.regions = &g_region;
    g_mem.fast_regions[8] = {kMem1, g_mem1};
    g_svc.fatal = svc_fatal; g_svc.log = svc_log; g_svc.time_base_ticks = svc_tb;
    g_svc.branch_checkpoint = svc_checkpoint; g_svc.checkpoint_counter = &g_ckpt_counter; g_svc.checkpoint_next_slow = &g_ckpt_next; g_svc.pending_event_mask = &g_pending;
    int failures = 0;
    auto check = [&](const char* what, bool ok, const char* detail) { std::printf("%-34s %s %s\n", what, ok ? "PASS" : "FAIL", detail); failures += !ok; };
    char d[160];
    PpcContext c{};
    c.gpr[1] = 0x81700000u;  // stack in MEM1
    // sum_words
    const std::uint32_t N = 4096, A = 0x80100000u;
    for (std::uint32_t i = 0; i < N; ++i) wbe32(A + 4 * i, i * 2654435761u);
    c.gpr[3] = A; c.gpr[4] = N; probe_sum_words(&c, &g_mem, &g_svc);
    std::uint32_t expect = ref_sum(reinterpret_cast<std::uint32_t*>(g_mem1 + 0x100000), N);
    std::snprintf(d, sizeof d, "got=%08X want=%08X", c.gpr[3], expect); check("sum_words (lwzu big-endian)", c.gpr[3] == expect, d);
    // dot_f32
    const std::uint32_t X = 0x80200000u, Y = 0x80300000u;
    for (std::uint32_t i = 0; i < N; ++i) { wf32(X + 4 * i, 0.001f * float(i % 97) - 0.03f); wf32(Y + 4 * i, 1.0f / float(1 + i % 13)); }
    c.gpr[3] = X; c.gpr[4] = Y; c.gpr[5] = N; probe_dot_f32(&c, &g_mem, &g_svc);
    const float got = f1_single(c);
    const float want = ref_dot(reinterpret_cast<std::uint32_t*>(g_mem1 + 0x200000), reinterpret_cast<std::uint32_t*>(g_mem1 + 0x300000), N);
    std::uint32_t gb, wb; std::memcpy(&gb, &got, 4); std::memcpy(&wb, &want, 4);
    std::snprintf(d, sizeof d, "got=%.9g (%08X) fused-ref=%.9g (%08X)", got, gb, want, wb);
    check("dot_f32 (fmadds, single)", got == want, d);
    // hash
    c.gpr[3] = 12345; c.gpr[4] = 1000; probe_hash_loop(&c, &g_mem, &g_svc);
    std::snprintf(d, sizeof d, "got=%08X want=%08X", c.gpr[3], ref_hash(12345, 1000)); check("hash_loop (rlwinm/xor/mullw)", c.gpr[3] == ref_hash(12345, 1000), d);
    // divw edge cases: INT_MIN/-1 and x/0 must not trap on any host
    struct { std::uint32_t a, b; } cases[] = {{100, 7}, {0xFFFFFF9Cu, 7}, {0x80000000u, 0xFFFFFFFFu}, {5, 0}, {0xFFFFFFFBu, 0}};
    for (auto& k : cases) { c.gpr[3] = k.a; c.gpr[4] = k.b; probe_div_edge(&c, &g_mem, &g_svc);
        std::snprintf(d, sizeof d, "%08X / %08X = %08X", k.a, k.b, c.gpr[3]); check("divw", true, d); }
    // memcpy
    c.gpr[3] = 0x80400000u; c.gpr[4] = A; c.gpr[5] = N; probe_memcpy_words(&c, &g_mem, &g_svc);
    check("memcpy_words (lwzu/stwu)", std::memcmp(g_mem1 + 0x400000, g_mem1 + 0x100000, 4 * N) == 0, "");

    // paired singles (ps_madd/ps_mul/ps_add/ps_sub): printed for a cross-architecture comparison
    {
        c.hid2 = 0xE0000000u;
        double z = 0.0; std::uint64_t zb; std::memcpy(&zb, &z, 8);
        c.fpr_bits[1] = c.ps1_bits[1] = c.fpr_bits[3] = c.ps1_bits[3] = zb;
        c.gpr[3] = X; c.gpr[4] = Y; c.gpr[5] = 1024; probe_ps_loop(&c, &g_mem, &g_svc);
        std::printf("XARCH ps_loop f1=%016llX/%016llX f3=%016llX/%016llX\n", (unsigned long long)c.fpr_bits[1], (unsigned long long)c.ps1_bits[1], (unsigned long long)c.fpr_bits[3], (unsigned long long)c.ps1_bits[3]);
    }
    {
        const double vals[] = {__builtin_nan(""), __builtin_inf(), -__builtin_inf(), 3e9, -3e9, -2147483648.5, 2147483647.9, -0.5, 1.5, -2147483649.0, -7.9};
        const std::uint32_t want[] = {0x80000000u, 0x7FFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000000u, 0x80000000u, 0x7FFFFFFFu, 0u, 1u, 0x80000000u, 0xFFFFFFF9u};
        bool all = true; std::string got;
        for (int i = 0; i < 11; ++i) {
            std::uint64_t bits; std::memcpy(&bits, &vals[i], 8);
            wbe32(0x80500000u, std::uint32_t(bits >> 32)); wbe32(0x80500004u, std::uint32_t(bits));
            c.gpr[3] = 0x80500000u; c.gpr[4] = 0x80500010u; probe_fctiwz_one(&c, &g_mem, &g_svc);
            std::uint32_t lo; std::memcpy(&lo, g_mem1 + 0x500014, 4); lo = byte_swap_u32(lo);
            char b[16]; std::snprintf(b, sizeof b, "%08X ", lo); got += b; all = all && lo == want[i];
        }
        check("fctiwz (NaN/inf/overflow)", all, got.c_str());
    }
    // Timing: recompiled guest code vs. the native reference, same host.
    auto bench = [&](const char* name, auto&& guest, auto&& native, double units) {
        double t0 = now(); guest(); double t1 = now(); native(); double t2 = now();
        std::printf("bench %-12s guest=%8.3f ms native=%8.3f ms ratio=%5.2fx  guest=%6.2f ns/iter\n", name, 1e3 * (t1 - t0), 1e3 * (t2 - t1), (t1 - t0) / (t2 - t1), 1e9 * (t1 - t0) / units);
    };
    volatile std::uint32_t sink = 0; volatile float fsink = 0;
    const std::uint32_t reps = 200 * scale;
    bench("sum_words", [&] { for (std::uint32_t r = 0; r < reps; ++r) { c.gpr[3] = A; c.gpr[4] = N; probe_sum_words(&c, &g_mem, &g_svc); sink = sink + c.gpr[3]; } },
          [&] { for (std::uint32_t r = 0; r < reps; ++r) { asm volatile("" ::: "memory"); sink = sink + ref_sum(reinterpret_cast<std::uint32_t*>(g_mem1 + 0x100000), N); } }, double(reps) * N);
    bench("dot_f32", [&] { for (std::uint32_t r = 0; r < reps; ++r) { c.gpr[3] = X; c.gpr[4] = Y; c.gpr[5] = N; probe_dot_f32(&c, &g_mem, &g_svc); fsink = fsink + f1_single(c); } },
          [&] { for (std::uint32_t r = 0; r < reps; ++r) { asm volatile("" ::: "memory"); fsink = fsink + ref_dot(reinterpret_cast<std::uint32_t*>(g_mem1 + 0x200000), reinterpret_cast<std::uint32_t*>(g_mem1 + 0x300000), N); } }, double(reps) * N);
    const std::uint32_t hn = 1000000 * scale;
    bench("hash_loop", [&] { c.gpr[3] = 1; c.gpr[4] = hn; probe_hash_loop(&c, &g_mem, &g_svc); sink = c.gpr[3]; }, [&] { sink = ref_hash(1, hn); }, hn);
    std::printf("checkpoints: counter=%" PRIu64 " slow-path calls=%" PRIu64 "\n", g_ckpt_counter, g_ckpt_slow);
    std::printf("%s\n", failures ? "PROBES FAILED" : "ALL PROBES PASSED");
    return failures != 0;
}
