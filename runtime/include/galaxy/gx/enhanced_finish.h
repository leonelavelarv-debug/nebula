// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Enhanced lighting's finish of the 3D scene without the ray tracer (enhanced_finish.hlsl, shader
// model 5.0): ambient occlusion over the opaque scene (half resolution, multiplied into the EFB
// colour), then at the scene's end a bloom of its light sources, a mild haze under bright skies and
// a filmic grade. Settings from the environment (read once):
//   GALAXY_ENHANCED_FINISH=0          none of it
//   GALAXY_ENHANCED_AO=<strength>     occlusion strength (default 1; 0 none)
//   GALAXY_ENHANCED_AO_RADIUS=<units> its reach in eye units (default 90)
//   GALAXY_ENHANCED_BLOOM=<strength>  bloom (default 0.8), GALAXY_ENHANCED_BLOOM_THRESHOLD (0.85),
//   GALAXY_ENHANCED_GLOW=<share>      of what glows added over the opaque scene (default 0.6)
//   GALAXY_ENHANCED_HAZE=<share>      haze (default 0.10), GALAXY_ENHANCED_HAZE_DISTANCE (25000)
//   GALAXY_ENHANCED_GRADE=<0..1>      grade (default 1)
//   GALAXY_ENHANCED_AO_VIEW=1         the occlusion alone (diagnostics)
//   GALAXY_ENHANCED_PASS_TIMING=1     [enh-pass] GPU milliseconds per pass every 300 frames

#include "galaxy/gx/ray_tracing.h"

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

namespace galaxy::gx {

struct EnhancedDescriptors {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
};

// The EFB as the passes see it: colour (render target state) and depth (depth-write state), the
// conversion table of their SRVs (colour, then depth) and the colour's RTV.
struct EnhancedTarget {
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE views{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};

class EnhancedFinish {
public:
    using Allocate = EnhancedDescriptors (*)(void* context, unsigned count);

    bool initialize(ID3D12Device* device, std::uint64_t timestamp_frequency);
    void shutdown() noexcept;
    [[nodiscard]] bool ready() const noexcept { return root_ != nullptr; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Each frame: collects pass timings that completed.
    void begin_frame(std::uint64_t frame_index);
    // A new 3D scene: its camera is unknown until set_view; nothing applied yet.
    void begin_scene() noexcept;
    // The scene's camera (from its first perspective draw) and the split column (EFB pixels, 0
    // none): left of it the game's own picture.
    void set_view(const RtView& view, float split) noexcept;
    [[nodiscard]] bool view_known() const noexcept { return view_known_; }
    [[nodiscard]] bool occlusion_done() const noexcept { return occlusion_done_; }
    // Whether resources of the EFB's size exist (else the caller drains the GPU, then calls
    // allocate_resources).
    [[nodiscard]] bool sized_for(const ID3D12Resource* color) const;
    bool allocate_resources(std::uint32_t width, std::uint32_t height);

    // After the opaque scene: its occlusion multiplied into the EFB colour, and its picture at
    // quarter resolution (what the glows add is measured against it).
    void occlusion(ID3D12GraphicsCommandList* list, const EnhancedTarget& target, Allocate allocate, void* context);
    // At the scene's end: bloom, haze and grade (with the occlusion when it was not applied).
    void finish(ID3D12GraphicsCommandList* list, const EnhancedTarget& target, Allocate allocate, void* context);

private:
    enum Pass : unsigned {
        kPassOcclusion,  // half-resolution depth, occlusion, its blur, multiplied into the EFB
        kPassOpaque,     // the opaque picture at quarter resolution
        kPassCopy,       // the picture before the finish, the sky's colour
        kPassBloom,      // bright part, blurs, eighth resolution
        kPassFinish,     // the full-screen finish
        kPassCount,
    };
    struct Constants {
        float projection[4];
        float depth[4];
        float viewport[4];
        std::uint32_t size[2];
        std::uint32_t half[2];
        std::uint32_t quarter[2];
        std::uint32_t eighth[2];
        float split;
        float ao;
        float ao_radius;
        float bloom;
        float threshold;
        float glow;
        float haze;
        float haze_distance;
        float grade;
        std::uint32_t flags;
        float pad[2];
    };
    static_assert(sizeof(Constants) == 32u * 4u);
    enum Texture : unsigned {
        kHalfDepth,
        kAoRaw,
        kAo,
        kOpaque,
        kQuarterA,
        kQuarterB,
        kEighthA,
        kEighthB,
        kTextureCount,
    };
    struct Views {
        D3D12_GPU_DESCRIPTOR_HANDLE srv_a{};  // t2-t9 with the A ping-pong textures
        D3D12_GPU_DESCRIPTOR_HANDLE srv_b{};  // with the B ones
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kTextureCount> uav{};  // u0 the texture
        D3D12_GPU_DESCRIPTOR_HANDLE state_uav{};                       // u2 the state
    };

    bool create_views(const EnhancedTarget& target, Allocate allocate, void* context, Views& views);
    void transition(ID3D12GraphicsCommandList* list, std::initializer_list<ID3D12Resource*> resources,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, D3D12_GPU_DESCRIPTOR_HANDLE srv,
                  D3D12_GPU_DESCRIPTOR_HANDLE uav, std::uint32_t width, std::uint32_t height, std::uint32_t flags);
    void fullscreen(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const EnhancedTarget& target,
                    D3D12_GPU_DESCRIPTOR_HANDLE srv, std::uint32_t flags);
    int begin_pass(ID3D12GraphicsCommandList* list);
    void end_pass(ID3D12GraphicsCommandList* list, int entry, Pass pass);

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> half_depth_, occlusion_cs_, occlusion_blur_, opaque_, bright_,
        blur_quarter_, blur_eighth_, down_, sky_, occlusion_ps_, finish_ps_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kTextureCount> textures_;
    Microsoft::WRL::ComPtr<ID3D12Resource> copy_;
    Microsoft::WRL::ComPtr<ID3D12Resource> state_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    Constants constants_{};
    bool enabled_ = false;
    bool view_known_ = false;
    bool occlusion_done_ = false;
    bool opaque_known_ = false;
    bool view_ao_ = false;

    // GPU timing (GALAXY_ENHANCED_PASS_TIMING): pairs of timestamps per pass, read a few frames later.
    static constexpr unsigned kPassEntries = 64u;
    static constexpr std::uint64_t kResultDelay = 4u;
    struct PassEntry {
        std::uint64_t frame = 0;
        Pass pass = kPassOcclusion;
        bool pending = false;
    };
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamps_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    std::array<PassEntry, kPassEntries> entries_{};
    unsigned next_entry_ = 0;
    std::uint64_t frame_index_ = 0;
    std::uint64_t timestamp_frequency_ = 0;
    std::array<double, kPassCount> pass_ms_{};
    std::array<std::uint64_t, kPassCount> passes_{};
    std::uint64_t window_frames_ = 0;
};

}  // namespace galaxy::gx
