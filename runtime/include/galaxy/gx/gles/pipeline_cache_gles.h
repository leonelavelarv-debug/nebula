// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// PipelineCacheGles: GX pipelines on OpenGL ES (docs/RG35XXH.md), the interface GxBackend uses on PipelineCacheVk.
//
// A pipeline is a program (its keys' ShaderGenerator HLSL, translated to GLSL ES by shader_translate_gles.h and
// linked) and the render state it draws with; pipelines of one shader pair share the program. Linked programs are
// kept as driver binaries (glGetProgramBinary) in cache_dir/gles/, one file a program, under the driver's identity;
// the keys seen (gles_keys.bin) are built again at the next start, on workers with contexts of their own sharing the
// renderer's objects, from the binaries when they are there.
//
// A key whose program is not linked yet (GALAXY_GLES_PENDING): wait (default) links it on the render thread now;
// skip leaves the draw out until a worker linked it; uber draws it with the uber program of its render state.
// GALAXY_GLES_SHADER_DUMP=<dir> writes every translated shader there.

#include "galaxy/gx/gles/renderer_gles.h"
#include "galaxy/gx/gles/shader_translate_gles.h"
#include "galaxy/gx/shader_gen.h"
#include "galaxy/gx/shader_keys.h"
#include "galaxy/gx/uber_shaders.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace galaxy::gx {

class PipelineCacheGles {
public:
    PipelineCacheGles() = default;
    PipelineCacheGles(const PipelineCacheGles&) = delete;
    PipelineCacheGles& operator=(const PipelineCacheGles&) = delete;
    ~PipelineCacheGles() noexcept;

    bool initialize(gpu::Device* device, const gles::GxLayout* layout, const std::filesystem::path& cache_dir,
                    unsigned worker_count);
    void shutdown();

    // Render thread only.
    [[nodiscard]] gles::Pipeline* get(const PsoKey& key, const PixelShaderKey& ps_key, const VertexShaderKey& vs_key);
    void drain_completions();

    void flush_disk();
    void flush_disk_async() { flush_disk(); }
    void poll_disk_flush() {}
    [[nodiscard]] bool has_unsaved_records() const noexcept { return unsaved_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t specialized_hits() const noexcept { return specialized_hits_; }
    [[nodiscard]] std::uint64_t uber_fallbacks() const noexcept { return uber_fallbacks_; }
    [[nodiscard]] bool last_get_used_uber() const noexcept { return last_get_used_uber_; }

    struct CompileStats {
        std::uint64_t pso_compiles = 0;
        std::uint64_t pso_library_loads = 0;  // programs from driver binaries
        std::uint64_t shader_pair_compiles = 0;
        std::uint64_t blobs_from_cache = 0;
        std::uint64_t blob_compile_us = 0;
        std::size_t shader_records = 0;
        std::size_t shader_records_on_disk = 0;
        std::size_t pso_keys = 0;
        std::size_t pso_keys_on_disk = 0;
        bool shader_cache_at_capacity = false;
        bool pso_cache_at_capacity = false;
    };
    [[nodiscard]] CompileStats compile_stats() const;

    // The program of a shader pair: built (or loaded) on the calling thread, which has a context current. 0 when it
    // fails (logged).
    struct ProgramSource {
        std::string vs_hlsl;
        std::string ps_hlsl;
    };

private:
    struct Entry {
        PsoKey key{};
        VertexShaderKey vs_key{};
        PixelShaderKey ps_key{};
    };
    static_assert(std::has_unique_object_representations_v<Entry>);
    struct PairKey {
        std::uint64_t vs = 0;
        std::uint64_t ps = 0;
        std::uint32_t form = 0;  // point size, uber variant + 1
        bool operator==(const PairKey&) const = default;
    };
    struct PairKeyHasher {
        std::size_t operator()(const PairKey& key) const noexcept {
            return static_cast<std::size_t>(combine64(combine64(key.vs, key.ps), key.form));
        }
    };
    struct Program {
        GLuint name = 0;
        std::uint32_t attributes = 0;
        bool failed = false;
    };
    struct Job {
        PairKey pair{};
        VertexShaderKey vs_key{};
        PixelShaderKey ps_key{};
    };
    enum class Pending { Wait, Skip, Uber };

    [[nodiscard]] PairKey pair_for(const PsoKey& key, bool points) const noexcept;
    [[nodiscard]] Program build_program(const PairKey& pair, const VertexShaderKey& vs_key,
                                        const PixelShaderKey& ps_key);
    [[nodiscard]] Program link(const std::string& vs_glsl, const std::string& ps_glsl, std::uint64_t binary_hash,
                               const char* what);
    [[nodiscard]] std::string translate(const std::string& hlsl, gles::Stage stage, const char* what);
    [[nodiscard]] std::unique_ptr<gles::Pipeline> make_pipeline(const RenderStateKey& rs, const Program& program,
                                                                bool dual_source, bool uber);
    [[nodiscard]] gles::Pipeline* uber_pipeline(const RenderStateKey& rs, const PixelShaderKey& ps_key);
    void enqueue(const Job& job);
    void worker_main(void* context);
    void load_disk();
    void save_disk() noexcept;
    [[nodiscard]] std::filesystem::path binary_path(std::uint64_t hash) const;

    bool ready_ = false;
    const gles::GxLayout* layout_ = nullptr;
    std::filesystem::path cache_dir_;
    std::string driver_key_;
    gles::TranslateOptions translate_options_{};
    ShaderGenOptions generator_options_{};
    Pending pending_ = Pending::Wait;
    std::string shader_dump_dir_;
    bool binaries_ = false;

    // Render thread.
    std::unordered_map<PsoKey, std::unique_ptr<gles::Pipeline>, PsoKeyHasher> published_;
    std::unordered_map<PairKey, Program, PairKeyHasher> programs_;
    std::unordered_set<PairKey, PairKeyHasher> in_flight_;
    std::unordered_map<std::uint64_t, std::unique_ptr<gles::Pipeline>> uber_pipelines_;
    std::array<Program, kUberPsVariants> uber_programs_{};
    PsoKey memo_key_{};
    gles::Pipeline* memo_pipeline_ = nullptr;
    bool last_get_used_uber_ = false;
    std::uint64_t specialized_hits_ = 0;
    std::uint64_t uber_fallbacks_ = 0;

    // Keys seen (persisted), workers and their queue.
    mutable std::mutex entries_mutex_;
    std::vector<Entry> entries_;
    std::unordered_set<PsoKey, PsoKeyHasher> entry_keys_;
    std::size_t entries_on_disk_ = 0;
    std::atomic<bool> unsaved_{false};
    std::mutex jobs_mutex_;
    std::condition_variable jobs_cv_;
    std::deque<Job> jobs_;
    std::vector<std::pair<PairKey, Program>> completions_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
    std::vector<void*> worker_contexts_;

    std::atomic<std::uint64_t> links_{0};
    std::atomic<std::uint64_t> binary_loads_{0};
    std::atomic<std::uint64_t> translations_{0};
    std::atomic<std::uint64_t> compile_us_{0};
    std::atomic<std::uint64_t> binary_failures_{0};
};

using PipelineCache = PipelineCacheGles;

}  // namespace galaxy::gx
