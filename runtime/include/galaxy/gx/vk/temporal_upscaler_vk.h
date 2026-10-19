// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Vulkan renderer's temporal upscaler: Arm Accuracy Super Resolution (MIT, third_party/arm-asr), AMD FSR 2
// optimised for mobile GPUs, from the jittered scene, its motion vectors, reactive mask and depth into the output EFB.

#include <volk.h>

#include <cstdint>
#include <memory>

namespace galaxy::gx::vk {

struct TemporalImage {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// One upscale, recorded into `commands` outside any render pass. Colour, motion, reactive and output are in the
// colour-attachment layout, depth in the shader-read layout; each is left as it came. Sizes are the regions used.
struct TemporalUpscaleInputsVk {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    TemporalImage color;     // RGBA8, gamma encoded, jittered
    TemporalImage depth;     // R32F, reversed (near = 1)
    TemporalImage motion;    // RG16F, render pixels, current to previous
    TemporalImage reactive;  // R8, blended draws' alpha (optional)
    TemporalImage output;    // RGBA8 colour attachment
    unsigned render_width = 0;
    unsigned render_height = 0;
    unsigned output_width = 0;
    unsigned output_height = 0;
    float jitter_x = 0.0f;  // render pixels
    float jitter_y = 0.0f;
    float camera_near = 1.0f;
    float camera_far = 100000.0f;
    float fov_y = 1.0f;
    float frame_ms = 16.667f;
    bool reset = false;
};

class TemporalUpscalerVk {
public:
    virtual ~TemporalUpscalerVk() = default;
    // False when it could not be recorded (the caller resamples instead).
    virtual bool evaluate(const TemporalUpscaleInputsVk& inputs) = 0;
    [[nodiscard]] virtual unsigned jitter_phase_count(unsigned render_width, unsigned output_width) const = 0;
    // Sub-pixel offset (render pixels, -0.5..0.5) for frame `index`.
    virtual void jitter_offset(unsigned index, unsigned phase_count, float& x, float& y) const = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

// Null when the device lacks what Arm ASR's shaders need (Context::temporal_upscaling) or creation failed. The
// context is sized for these maxima. Thread-safe against the render thread's own Vulkan work.
[[nodiscard]] std::unique_ptr<TemporalUpscalerVk> create_arm_asr_upscaler(
    unsigned max_render_width, unsigned max_render_height, unsigned output_width, unsigned output_height);

}  // namespace galaxy::gx::vk
