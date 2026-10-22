// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct RecompReport {
    int passed = 0, failed = 0;
    double sum_ns = 0, sum_native_ns = 0, dot_ns = 0, dot_native_ns = 0, hash_ns = 0, hash_native_ns = 0;
    std::string text;
};
RecompReport run_recomp_probes(unsigned scale);

struct GxReport {
    bool ok = false;
    std::size_t frame_bytes = 0;
    double ms_per_frame = 0, ns_per_draw = 0, mb_per_s = 0;
    char error[256] = {};
};
GxReport run_gx_fifo_bench(unsigned draws_per_frame, unsigned frames);

struct MemSnapshot {
    long mem_total_kib = -1, mem_available_kib = -1, swap_total_kib = -1, swap_free_kib = -1;
    long rss_kib = -1, hwm_kib = -1, pss_kib = -1;
};
MemSnapshot mem_snapshot();

struct GuestMemReport {
    bool reserve_4g = false, commit_mem1 = false, commit_mem2 = false, ring_alias = false, module_sim = false;
    double touch_ms = 0, ring_ms = 0;
    long pss_growth_kib = 0, available_after_kib = 0, available_with_module_kib = 0;
    std::string text;
};
GuestMemReport run_guest_memory_test(std::size_t module_estimate_mib);

struct CpuReport {
    std::string model;
    int cores = 0;
    long cur_khz[8] = {}, max_khz[8] = {};
    std::string governor;
    double memcpy_gbs = 0;
    double single_hash_ns = 0, multi_hash_ns = 0;  // per iteration, per thread
    int threads = 0;
    std::string thermal;
};
CpuReport run_cpu_info_and_bench();

struct VkReport {
    bool loader = false, instance = false;
    unsigned devices = 0;
    std::string text;
};
VkReport run_vulkan_probe();

std::string read_file(const char* path, std::size_t limit = 4096);
std::string sys_summary();
