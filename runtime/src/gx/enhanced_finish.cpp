// SPDX-License-Identifier: GPL-3.0-only
#include "galaxy/gx/enhanced_finish.h"

#include "enh_blur_eighth_cs.h"
#include "enh_blur_quarter_cs.h"
#include "enh_bright_cs.h"
#include "enh_down_cs.h"
#include "enh_finish_ps.h"
#include "enh_finish_vs.h"
#include "enh_half_depth_cs.h"
#include "enh_occlusion_blur_cs.h"
#include "enh_occlusion_cs.h"
#include "enh_occlusion_ps.h"
#include "enh_opaque_cs.h"
#include "enh_sky_cs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace galaxy::gx {
namespace {

using Microsoft::WRL::ComPtr;

float env_value(const char* name, float fallback, float low, float high) {
    char value[32]{};
    std::size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), name) != 0 || length <= 1u) {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    return end == value ? fallback : std::clamp(parsed, low, high);
}

constexpr D3D12_RESOURCE_STATES kResting =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr unsigned kStateFloats = 16u;

// Texture formats of EnhancedFinish::Texture.
constexpr DXGI_FORMAT kFormats[] = {
    DXGI_FORMAT_R32_FLOAT,        DXGI_FORMAT_R8_UNORM,         DXGI_FORMAT_R8_UNORM,
    DXGI_FORMAT_R11G11B10_FLOAT,  DXGI_FORMAT_R11G11B10_FLOAT,  DXGI_FORMAT_R11G11B10_FLOAT,
    DXGI_FORMAT_R11G11B10_FLOAT,  DXGI_FORMAT_R11G11B10_FLOAT,
};
// Their size as a power-of-two fraction of the EFB's.
constexpr unsigned kShifts[] = {1u, 1u, 1u, 2u, 2u, 2u, 3u, 3u};

std::uint32_t reduced(std::uint32_t extent, unsigned shift) {
    return std::max<std::uint32_t>((extent + (1u << shift) - 1u) >> shift, 1u);
}

}  // namespace

bool EnhancedFinish::initialize(ID3D12Device* device, std::uint64_t timestamp_frequency) {
    shutdown();
    enabled_ = env_value("GALAXY_ENHANCED_FINISH", 1.0f, 0.0f, 1.0f) != 0.0f;
    if (device == nullptr || !enabled_) {
        return false;
    }
    device_ = device;
    constants_.ao = env_value("GALAXY_ENHANCED_AO", 1.0f, 0.0f, 4.0f);
    constants_.ao_radius = env_value("GALAXY_ENHANCED_AO_RADIUS", 90.0f, 5.0f, 2000.0f);
    constants_.bloom = env_value("GALAXY_ENHANCED_BLOOM", 0.8f, 0.0f, 4.0f);
    constants_.threshold = env_value("GALAXY_ENHANCED_BLOOM_THRESHOLD", 0.85f, 0.0f, 0.99f);
    constants_.glow = env_value("GALAXY_ENHANCED_GLOW", 0.6f, 0.0f, 4.0f);
    constants_.haze = env_value("GALAXY_ENHANCED_HAZE", 0.10f, 0.0f, 1.0f);
    constants_.haze_distance = env_value("GALAXY_ENHANCED_HAZE_DISTANCE", 25000.0f, 100.0f, 1.0e6f);
    constants_.grade = env_value("GALAXY_ENHANCED_GRADE", 1.0f, 0.0f, 1.0f);
    view_ao_ = env_value("GALAXY_ENHANCED_AO_VIEW", 0.0f, 0.0f, 1.0f) != 0.0f;

    // b0 constants, t0-t1 the EFB's colour and depth, t2-t9 the finish's textures, u0-u2 outputs.
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2u;
    ranges[0].BaseShaderRegister = 0u;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 8u;
    ranges[1].BaseShaderRegister = 2u;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[2].NumDescriptors = 3u;
    ranges[2].BaseShaderRegister = 0u;
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(Constants) / 4u;
    for (unsigned i = 0; i < 3u; ++i) {
        params[i + 1u].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1u].DescriptorTable.NumDescriptorRanges = 1u;
        params[i + 1u].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 4u;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1u;
    desc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device_->CreateRootSignature(0u, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
        std::fprintf(stderr, "[enh-finish] root signature failed\n");
        shutdown();
        return false;
    }
    const auto compute = [&](const BYTE* code, std::size_t size, ComPtr<ID3D12PipelineState>& out) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.CS = {code, size};
        return SUCCEEDED(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&out)));
    };
    const auto graphics = [&](const BYTE* code, std::size_t size, bool multiply, ComPtr<ID3D12PipelineState>& out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.VS = {g_enh_finish_vs, sizeof(g_enh_finish_vs)};
        pd.PS = {code, size};
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pd.BlendState.RenderTarget[0];
        blend.BlendEnable = multiply ? TRUE : FALSE;
        blend.SrcBlend = D3D12_BLEND_ZERO;
        blend.DestBlend = D3D12_BLEND_SRC_COLOR;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        // The EFB's alpha is the game's.
        blend.RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1u;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1u;
        return SUCCEEDED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
    };
    if (!compute(g_enh_half_depth_cs, sizeof(g_enh_half_depth_cs), half_depth_) ||
        !compute(g_enh_occlusion_cs, sizeof(g_enh_occlusion_cs), occlusion_cs_) ||
        !compute(g_enh_occlusion_blur_cs, sizeof(g_enh_occlusion_blur_cs), occlusion_blur_) ||
        !compute(g_enh_opaque_cs, sizeof(g_enh_opaque_cs), opaque_) ||
        !compute(g_enh_bright_cs, sizeof(g_enh_bright_cs), bright_) ||
        !compute(g_enh_blur_quarter_cs, sizeof(g_enh_blur_quarter_cs), blur_quarter_) ||
        !compute(g_enh_blur_eighth_cs, sizeof(g_enh_blur_eighth_cs), blur_eighth_) ||
        !compute(g_enh_down_cs, sizeof(g_enh_down_cs), down_) ||
        !compute(g_enh_sky_cs, sizeof(g_enh_sky_cs), sky_) ||
        !graphics(g_enh_occlusion_ps, sizeof(g_enh_occlusion_ps), true, occlusion_ps_) ||
        !graphics(g_enh_finish_ps, sizeof(g_enh_finish_ps), false, finish_ps_)) {
        std::fprintf(stderr, "[enh-finish] pipelines failed\n");
        shutdown();
        return false;
    }
    {
        // The sky's colour, kept across frames.
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = kStateFloats * 4u;
        buffer.Height = 1u;
        buffer.DepthOrArraySize = 1u;
        buffer.MipLevels = 1u;
        buffer.SampleDesc.Count = 1u;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, kResting, nullptr,
                                                    IID_PPV_ARGS(&state_)))) {
            shutdown();
            return false;
        }
        state_->SetName(L"enhanced finish state");
    }
    timestamp_frequency_ = timestamp_frequency;
    if (timestamp_frequency != 0u && env_value("GALAXY_ENHANCED_PASS_TIMING", 0.0f, 0.0f, 1.0f) != 0.0f) {
        D3D12_QUERY_HEAP_DESC query{};
        query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query.Count = kPassEntries * 2u;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = kPassEntries * 2u * sizeof(std::uint64_t);
        buffer.Height = 1u;
        buffer.DepthOrArraySize = 1u;
        buffer.MipLevels = 1u;
        buffer.SampleDesc.Count = 1u;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps_))) ||
            FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback_)))) {
            timestamps_.Reset();
            readback_.Reset();
        }
    }
    std::fprintf(stderr,
                 "[enh-finish] ready ao=%.2f radius=%.0f bloom=%.2f threshold=%.2f glow=%.2f haze=%.2f/%.0f "
                 "grade=%.2f timing=%d\n",
                 constants_.ao, constants_.ao_radius, constants_.bloom, constants_.threshold, constants_.glow,
                 constants_.haze, constants_.haze_distance, constants_.grade, timestamps_ ? 1 : 0);
    return true;
}

void EnhancedFinish::shutdown() noexcept {
    root_.Reset();
    half_depth_.Reset();
    occlusion_cs_.Reset();
    occlusion_blur_.Reset();
    opaque_.Reset();
    bright_.Reset();
    blur_quarter_.Reset();
    blur_eighth_.Reset();
    down_.Reset();
    sky_.Reset();
    occlusion_ps_.Reset();
    finish_ps_.Reset();
    for (auto& texture : textures_) {
        texture.Reset();
    }
    copy_.Reset();
    state_.Reset();
    timestamps_.Reset();
    readback_.Reset();
    entries_ = {};
    width_ = height_ = 0u;
    device_.Reset();
}

bool EnhancedFinish::sized_for(const ID3D12Resource* color) const {
    if (color == nullptr || !copy_) {
        return false;
    }
    const D3D12_RESOURCE_DESC desc = const_cast<ID3D12Resource*>(color)->GetDesc();
    return desc.Width == width_ && desc.Height == height_;
}

bool EnhancedFinish::allocate_resources(std::uint32_t width, std::uint32_t height) {
    for (auto& texture : textures_) {
        texture.Reset();
    }
    copy_.Reset();
    width_ = height_ = 0u;
    if (!device_ || width == 0u || height == 0u) {
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = 1u;
    desc.SampleDesc.Count = 1u;
    for (unsigned i = 0; i < kTextureCount; ++i) {
        desc.Width = reduced(width, kShifts[i]);
        desc.Height = reduced(height, kShifts[i]);
        desc.Format = kFormats[i];
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kResting, nullptr,
                                                    IID_PPV_ARGS(&textures_[i])))) {
            return false;
        }
        textures_[i]->SetName(L"enhanced finish texture");
    }
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kResting, nullptr,
                                                IID_PPV_ARGS(&copy_)))) {
        return false;
    }
    copy_->SetName(L"enhanced finish picture");
    width_ = width;
    height_ = height;
    constants_.size[0] = width;
    constants_.size[1] = height;
    constants_.half[0] = reduced(width, 1u);
    constants_.half[1] = reduced(height, 1u);
    constants_.quarter[0] = reduced(width, 2u);
    constants_.quarter[1] = reduced(height, 2u);
    constants_.eighth[0] = reduced(width, 3u);
    constants_.eighth[1] = reduced(height, 3u);
    return true;
}

void EnhancedFinish::begin_frame(std::uint64_t frame_index) {
    frame_index_ = frame_index;
    if (!readback_ || timestamp_frequency_ == 0u) {
        return;
    }
    const std::uint64_t* ticks = nullptr;
    const double to_ms = 1000.0 / static_cast<double>(timestamp_frequency_);
    for (unsigned index = 0; index < kPassEntries; ++index) {
        PassEntry& entry = entries_[index];
        if (!entry.pending || entry.frame + kResultDelay > frame_index) {
            continue;
        }
        entry.pending = false;
        if (ticks == nullptr) {
            void* mapped = nullptr;
            const D3D12_RANGE all{0, kPassEntries * 2u * sizeof(std::uint64_t)};
            if (FAILED(readback_->Map(0, &all, &mapped))) {
                return;
            }
            ticks = static_cast<const std::uint64_t*>(mapped);
        }
        const std::uint64_t begin = ticks[index * 2u];
        const std::uint64_t end = ticks[index * 2u + 1u];
        if (end >= begin && end - begin < timestamp_frequency_) {
            pass_ms_[entry.pass] += static_cast<double>(end - begin) * to_ms;
            ++passes_[entry.pass];
        }
    }
    if (ticks != nullptr) {
        const D3D12_RANGE no_write{0, 0};
        readback_->Unmap(0, &no_write);
    }
    if (++window_frames_ >= 300u) {
        // Per pass: one call's mean and calls per frame (sub-frames' replays included).
        const auto ms = [&](Pass p) { return passes_[p] != 0u ? pass_ms_[p] / static_cast<double>(passes_[p]) : 0.0; };
        const auto per = [&](Pass p) { return static_cast<double>(passes_[p]) / static_cast<double>(window_frames_); };
        double total = 0.0;
        for (unsigned p = 0; p < kPassCount; ++p) {
            total += pass_ms_[p];
        }
        std::fprintf(stderr,
                     "[enh-pass] frames=%llu size=%ux%u occlusion-ms=%.3f occlusions=%.2f opaque-ms=%.3f copy-ms=%.3f "
                     "bloom-ms=%.3f finish-ms=%.3f finishes=%.2f gpu-ms-per-frame=%.3f\n",
                     static_cast<unsigned long long>(window_frames_), width_, height_, ms(kPassOcclusion),
                     per(kPassOcclusion), ms(kPassOpaque), ms(kPassCopy), ms(kPassBloom), ms(kPassFinish),
                     per(kPassFinish), total / static_cast<double>(window_frames_));
        pass_ms_ = {};
        passes_ = {};
        window_frames_ = 0u;
    }
}

void EnhancedFinish::begin_scene() noexcept {
    view_known_ = false;
    occlusion_done_ = false;
    opaque_known_ = false;
}

void EnhancedFinish::set_view(const RtView& view, float split) noexcept {
    for (unsigned i = 0; i < 4u; ++i) {
        constants_.projection[i] = view.projection[i];
        constants_.viewport[i] = view.viewport[i];
    }
    constants_.depth[0] = view.z_scale;
    constants_.depth[1] = view.z_offset;
    constants_.depth[2] = view.min_depth;
    constants_.depth[3] = view.max_depth;
    constants_.split = split;
    view_known_ = view.projection[0] != 0.0f && view.projection[2] != 0.0f && view.viewport[2] > 0.0f &&
                  view.viewport[3] > 0.0f;
}

bool EnhancedFinish::create_views(const EnhancedTarget& target, Allocate allocate, void* context, Views& views) {
    // Two SRV tables (8 each), a UAV table (3) per texture and one for the state.
    constexpr unsigned kCount = 16u + 3u * (kTextureCount + 1u);
    const EnhancedDescriptors table = allocate(context, kCount);
    if (table.cpu.ptr == 0u || table.gpu.ptr == 0u || target.views.ptr == 0u) {
        return false;
    }
    const UINT stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    unsigned at = 0u;
    const auto cpu = [&](unsigned slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{table.cpu.ptr + static_cast<SIZE_T>(slot) * stride}; };
    const auto gpu = [&](unsigned slot) { return D3D12_GPU_DESCRIPTOR_HANDLE{table.gpu.ptr + static_cast<UINT64>(slot) * stride}; };
    const auto texture_srv = [&](ID3D12Resource* resource, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1u;
        device_->CreateShaderResourceView(resource, &srv, cpu(at++));
    };
    for (unsigned set = 0; set < 2u; ++set) {
        (set == 0u ? views.srv_a : views.srv_b) = gpu(at);
        texture_srv(copy_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        texture_srv(textures_[kHalfDepth].Get(), kFormats[kHalfDepth]);
        texture_srv(textures_[kAoRaw].Get(), kFormats[kAoRaw]);
        texture_srv(textures_[kAo].Get(), kFormats[kAo]);
        texture_srv(textures_[set == 0u ? kQuarterA : kQuarterB].Get(), kFormats[kQuarterA]);
        texture_srv(textures_[set == 0u ? kEighthA : kEighthB].Get(), kFormats[kEighthA]);
        texture_srv(textures_[kOpaque].Get(), kFormats[kOpaque]);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = kStateFloats;
        srv.Buffer.StructureByteStride = 4u;
        device_->CreateShaderResourceView(state_.Get(), &srv, cpu(at++));
    }
    const auto null_uav = [&]() {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(nullptr, nullptr, &uav, cpu(at++));
    };
    const auto state_uav = [&]() {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = kStateFloats;
        uav.Buffer.StructureByteStride = 4u;
        device_->CreateUnorderedAccessView(state_.Get(), nullptr, &uav, cpu(at++));
    };
    for (unsigned i = 0; i < kTextureCount; ++i) {
        views.uav[i] = gpu(at);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = kFormats[i];
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(textures_[i].Get(), nullptr, &uav, cpu(at++));
        null_uav();
        null_uav();
    }
    views.state_uav = gpu(at);
    null_uav();
    null_uav();
    state_uav();
    return true;
}

void EnhancedFinish::transition(ID3D12GraphicsCommandList* list, std::initializer_list<ID3D12Resource*> resources,
                                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    std::array<D3D12_RESOURCE_BARRIER, 8> barriers{};
    UINT count = 0u;
    for (ID3D12Resource* resource : resources) {
        D3D12_RESOURCE_BARRIER& b = barriers[count++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
    }
    list->ResourceBarrier(count, barriers.data());
}

void EnhancedFinish::dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso,
                              D3D12_GPU_DESCRIPTOR_HANDLE srv, D3D12_GPU_DESCRIPTOR_HANDLE uav, std::uint32_t width,
                              std::uint32_t height, std::uint32_t flags) {
    constants_.flags = flags;
    list->SetPipelineState(pso);
    list->SetComputeRoot32BitConstants(0u, sizeof(Constants) / 4u, &constants_, 0u);
    list->SetComputeRootDescriptorTable(2u, srv);
    list->SetComputeRootDescriptorTable(3u, uav);
    list->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);
}

void EnhancedFinish::fullscreen(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso,
                                const EnhancedTarget& target, D3D12_GPU_DESCRIPTOR_HANDLE srv, std::uint32_t flags) {
    constants_.flags = flags;
    list->OMSetRenderTargets(1u, &target.rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    list->RSSetViewports(1u, &viewport);
    list->RSSetScissorRects(1u, &scissor);
    list->SetGraphicsRootSignature(root_.Get());
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(0u, sizeof(Constants) / 4u, &constants_, 0u);
    list->SetGraphicsRootDescriptorTable(1u, target.views);
    list->SetGraphicsRootDescriptorTable(2u, srv);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3u, 1u, 0u, 0u);
}

int EnhancedFinish::begin_pass(ID3D12GraphicsCommandList* list) {
    if (!timestamps_) {
        return -1;
    }
    const unsigned entry = next_entry_;
    if (entries_[entry].pending) {
        return -1;
    }
    next_entry_ = (entry + 1u) % kPassEntries;
    list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, entry * 2u);
    return static_cast<int>(entry);
}

void EnhancedFinish::end_pass(ID3D12GraphicsCommandList* list, int entry, Pass pass) {
    if (entry < 0) {
        return;
    }
    const auto index = static_cast<unsigned>(entry);
    list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2u + 1u);
    list->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2u, 2u, readback_.Get(),
                           index * 2u * sizeof(std::uint64_t));
    entries_[index] = PassEntry{frame_index_, pass, true};
}

void EnhancedFinish::occlusion(ID3D12GraphicsCommandList* list, const EnhancedTarget& target, Allocate allocate,
                               void* context) {
    Views views{};
    if (!ready() || !view_known_ || width_ == 0u || list == nullptr || target.color == nullptr ||
        target.depth == nullptr || !create_views(target, allocate, context, views)) {
        return;
    }
    occlusion_done_ = true;
    list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootDescriptorTable(1u, target.views);
    const std::uint32_t hw = constants_.half[0];
    const std::uint32_t hh = constants_.half[1];
    // The EFB depth as a shader resource for the occlusion passes.
    transition(list, {target.depth}, D3D12_RESOURCE_STATE_DEPTH_WRITE, kResting);
    const bool ao = constants_.ao > 0.0f && !view_ao_;
    if (ao) {
        const int timed = begin_pass(list);
        ID3D12Resource* half = textures_[kHalfDepth].Get();
        ID3D12Resource* raw = textures_[kAoRaw].Get();
        ID3D12Resource* blurred = textures_[kAo].Get();
        transition(list, {half, raw, blurred}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(list, half_depth_.Get(), views.srv_a, views.uav[kHalfDepth], hw, hh, 0u);
        transition(list, {half}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        dispatch(list, occlusion_cs_.Get(), views.srv_a, views.uav[kAoRaw], hw, hh, 0u);
        transition(list, {raw}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        dispatch(list, occlusion_blur_.Get(), views.srv_a, views.uav[kAo], hw, hh, 0u);
        transition(list, {blurred}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        fullscreen(list, occlusion_ps_.Get(), target, views.srv_a, 0u);
        end_pass(list, timed, kPassOcclusion);
    }
    transition(list, {target.depth}, kResting, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (constants_.bloom > 0.0f && constants_.glow > 0.0f) {
        // The opaque picture after its occlusion.
        const int timed = begin_pass(list);
        ID3D12Resource* opaque = textures_[kOpaque].Get();
        transition(list, {target.color}, D3D12_RESOURCE_STATE_RENDER_TARGET, kResting);
        transition(list, {opaque}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(root_.Get());
        list->SetComputeRootDescriptorTable(1u, target.views);
        dispatch(list, opaque_.Get(), views.srv_a, views.uav[kOpaque], constants_.quarter[0], constants_.quarter[1], 0u);
        transition(list, {opaque}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        transition(list, {target.color}, kResting, D3D12_RESOURCE_STATE_RENDER_TARGET);
        end_pass(list, timed, kPassOpaque);
        opaque_known_ = true;
    }
}

void EnhancedFinish::finish(ID3D12GraphicsCommandList* list, const EnhancedTarget& target, Allocate allocate,
                            void* context) {
    Views views{};
    if (!ready() || !view_known_ || width_ == 0u || list == nullptr || target.color == nullptr ||
        target.depth == nullptr || !create_views(target, allocate, context, views)) {
        return;
    }
    // Without the opaque point (no phase markers), the occlusion joins the finish.
    const bool late_ao = (!occlusion_done_ && constants_.ao > 0.0f) || view_ao_;
    {
        const int timed = begin_pass(list);
        transition(list, {target.color}, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list, {copy_.Get()}, kResting, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(copy_.Get(), target.color);
        transition(list, {copy_.Get()}, D3D12_RESOURCE_STATE_COPY_DEST, kResting);
        transition(list, {target.color}, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(list, {target.depth}, D3D12_RESOURCE_STATE_DEPTH_WRITE, kResting);
        list->SetComputeRootSignature(root_.Get());
        list->SetComputeRootDescriptorTable(1u, target.views);
        if (constants_.haze > 0.0f) {
            transition(list, {state_.Get()}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            constants_.flags = 0u;
            list->SetPipelineState(sky_.Get());
            list->SetComputeRoot32BitConstants(0u, sizeof(Constants) / 4u, &constants_, 0u);
            list->SetComputeRootDescriptorTable(2u, views.srv_a);
            list->SetComputeRootDescriptorTable(3u, views.state_uav);
            list->Dispatch(1u, 1u, 1u);
            transition(list, {state_.Get()}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        }
        end_pass(list, timed, kPassCopy);
    }
    if (late_ao) {
        const int timed = begin_pass(list);
        const std::uint32_t hw = constants_.half[0];
        const std::uint32_t hh = constants_.half[1];
        ID3D12Resource* half = textures_[kHalfDepth].Get();
        ID3D12Resource* raw = textures_[kAoRaw].Get();
        ID3D12Resource* blurred = textures_[kAo].Get();
        transition(list, {half, raw, blurred}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(list, half_depth_.Get(), views.srv_a, views.uav[kHalfDepth], hw, hh, 0u);
        transition(list, {half}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        dispatch(list, occlusion_cs_.Get(), views.srv_a, views.uav[kAoRaw], hw, hh, 0u);
        transition(list, {raw}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        dispatch(list, occlusion_blur_.Get(), views.srv_a, views.uav[kAo], hw, hh, 0u);
        transition(list, {blurred}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        end_pass(list, timed, kPassOcclusion);
    }
    {
        // Bloom: the bright part (A), across into B, down into A; halved into eighth A, across into
        // eighth B, down into eighth A.
        const int timed = begin_pass(list);
        ID3D12Resource* qa = textures_[kQuarterA].Get();
        ID3D12Resource* qb = textures_[kQuarterB].Get();
        ID3D12Resource* ea = textures_[kEighthA].Get();
        ID3D12Resource* eb = textures_[kEighthB].Get();
        const std::uint32_t qw = constants_.quarter[0];
        const std::uint32_t qh = constants_.quarter[1];
        const std::uint32_t ew = constants_.eighth[0];
        const std::uint32_t eh = constants_.eighth[1];
        const std::uint32_t glow = opaque_known_ ? 2u : 0u;
        transition(list, {qa}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(list, bright_.Get(), views.srv_a, views.uav[kQuarterA], qw, qh, glow);
        transition(list, {qa}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        transition(list, {qb}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(list, blur_quarter_.Get(), views.srv_a, views.uav[kQuarterB], qw, qh, 0u);
        transition(list, {qb}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        transition(list, {qa}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(list, blur_quarter_.Get(), views.srv_b, views.uav[kQuarterA], qw, qh, 4u);
        transition(list, {qa}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        transition(list, {ea}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(list, down_.Get(), views.srv_a, views.uav[kEighthA], ew, eh, 0u);
        transition(list, {ea}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        transition(list, {eb}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(list, blur_eighth_.Get(), views.srv_a, views.uav[kEighthB], ew, eh, 0u);
        transition(list, {eb}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        transition(list, {ea}, kResting, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(list, blur_eighth_.Get(), views.srv_b, views.uav[kEighthA], ew, eh, 4u);
        transition(list, {ea}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kResting);
        end_pass(list, timed, kPassBloom);
    }
    {
        const int timed = begin_pass(list);
        const std::uint32_t flags = (late_ao ? 1u : 0u) | (view_ao_ ? 8u : 0u);
        fullscreen(list, finish_ps_.Get(), target, views.srv_a, flags);
        end_pass(list, timed, kPassFinish);
    }
    transition(list, {target.depth}, kResting, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

}  // namespace galaxy::gx
