// SPDX-License-Identifier: GPL-3.0-only
#include "galaxy/gx/vk/temporal_upscaler_vk.h"

#include "galaxy/gx/post_process_settings.h"
#include "galaxy/gx/vk/vk_context.h"

#include <host/ffxm_fsr2.h>
#include <host/backends/vk/ffxm_vk.h>

#include <iostream>
#include <vector>

namespace galaxy::gx {

// The Vulkan renderer's temporal upscaler (the FSR 3 choice of D3D12's menu): Arm ASR where the device has what its
// shaders need.
bool fsr3_available() noexcept {
    return vk::Context::get().temporal_upscaling();
}

namespace vk {
namespace {

arm::FfxmSurfaceFormat surface_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16G16_SFLOAT: return arm::FFXM_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R8_UNORM: return arm::FFXM_SURFACE_FORMAT_R8_UNORM;
    case VK_FORMAT_R32_SFLOAT: return arm::FFXM_SURFACE_FORMAT_R32_FLOAT;
    default: return arm::FFXM_SURFACE_FORMAT_R8G8B8A8_UNORM;
    }
}

arm::FfxmResource resource(const TemporalImage& image, const wchar_t* name, arm::FfxmResourceStates state,
                           arm::FfxmResourceUsage usage) {
    arm::FfxmResourceDescription description{};
    description.type = arm::FFXM_RESOURCE_TYPE_TEXTURE2D;
    description.format = surface_format(image.format);
    description.width = image.width;
    description.height = image.height;
    description.depth = 1u;
    description.mipCount = 1u;
    description.flags = arm::FFXM_RESOURCE_FLAGS_NONE;
    description.usage = usage;
    return arm::ffxmGetResourceVK(reinterpret_cast<void*>(image.image), description, const_cast<wchar_t*>(name),
                                  state);
}

class ArmAsrUpscaler final : public TemporalUpscalerVk {
public:
    ~ArmAsrUpscaler() override {
        if (context_created_) {
            arm::ffxmFsr2ContextDestroy(&context_);
        }
    }

    bool create(unsigned max_render_width, unsigned max_render_height, unsigned output_width,
                unsigned output_height) {
        const Context& vulkan = Context::get();
        scratch_.resize(arm::ffxmGetScratchMemorySizeVK(vulkan.physical_device(), FFXM_FSR2_CONTEXT_COUNT));
        arm::VkDeviceContext device{vulkan.device(), vulkan.physical_device(), vkGetDeviceProcAddr};
        arm::FfxmFsr2ContextDescription description{};
        if (arm::ffxmGetInterfaceVK(&description.backendInterface, arm::ffxmGetDeviceVK(&device), scratch_.data(),
                                    scratch_.size(), FFXM_FSR2_CONTEXT_COUNT) != arm::FFXM_OK) {
            std::cerr << "[arm-asr] backend creation failed\n";
            return false;
        }
        // The EFB is reversed-Z gamma RGBA8 without an exposure source.
        description.flags = arm::FFXM_FSR2_ENABLE_DEPTH_INVERTED | arm::FFXM_FSR2_ENABLE_AUTO_EXPOSURE;
        description.qualityMode = arm::FFXM_FSR2_SHADER_QUALITY_MODE_QUALITY;
        description.maxRenderSize = {max_render_width, max_render_height};
        description.displaySize = {output_width, output_height};
        const arm::FfxmErrorCode result = arm::ffxmFsr2ContextCreate(&context_, &description);
        if (result != arm::FFXM_OK) {
            std::cerr << "[arm-asr] context creation failed: " << result << '\n';
            return false;
        }
        context_created_ = true;
        return true;
    }

    bool evaluate(const TemporalUpscaleInputsVk& in) override {
        if (!context_created_ || in.commands == VK_NULL_HANDLE || in.color.image == VK_NULL_HANDLE ||
            in.depth.image == VK_NULL_HANDLE || in.output.image == VK_NULL_HANDLE) {
            return false;
        }
        arm::FfxmFsr2DispatchDescription d{};
        d.commandList = arm::ffxmGetCommandListVK(in.commands);
        d.color = resource(in.color, L"Nebula EFB color", arm::FFXM_RESOURCE_STATE_PIXEL_WRITE,
                           arm::FFXM_RESOURCE_USAGE_READ_ONLY);
        d.depth = resource(in.depth, L"Nebula EFB depth", arm::FFXM_RESOURCE_STATE_PIXEL_READ,
                           arm::FFXM_RESOURCE_USAGE_READ_ONLY);
        if (in.motion.image != VK_NULL_HANDLE) {
            d.motionVectors = resource(in.motion, L"Nebula motion vectors", arm::FFXM_RESOURCE_STATE_PIXEL_WRITE,
                                       arm::FFXM_RESOURCE_USAGE_READ_ONLY);
        }
        if (in.reactive.image != VK_NULL_HANDLE) {
            d.reactive = resource(in.reactive, L"Nebula reactive mask", arm::FFXM_RESOURCE_STATE_PIXEL_WRITE,
                                  arm::FFXM_RESOURCE_USAGE_READ_ONLY);
        }
        d.output = resource(in.output, L"Nebula upscaled scene", arm::FFXM_RESOURCE_STATE_PIXEL_WRITE,
                            arm::FFXM_RESOURCE_USAGE_RENDERTARGET);
        d.jitterOffset = {in.jitter_x, in.jitter_y};
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {in.render_width, in.render_height};
        d.enableSharpening = false;
        d.sharpness = 0.0f;
        d.frameTimeDelta = in.frame_ms;
        d.preExposure = 1.0f;
        d.reset = in.reset;
        d.cameraNear = in.camera_near;
        d.cameraFar = in.camera_far;
        d.cameraFovAngleVertical = in.fov_y;
        d.viewSpaceToMetersFactor = 1.0f;
        const arm::FfxmErrorCode result = arm::ffxmFsr2ContextDispatch(&context_, &d);
        if (result != arm::FFXM_OK) {
            if (!reported_failure_) {
                reported_failure_ = true;
                std::cerr << "[arm-asr] dispatch failed: " << result << '\n';
            }
            return false;
        }
        return true;
    }

    unsigned jitter_phase_count(unsigned render_width, unsigned output_width) const override {
        return static_cast<unsigned>(arm::ffxmFsr2GetJitterPhaseCount(static_cast<std::int32_t>(render_width),
                                                                       static_cast<std::int32_t>(output_width)));
    }

    void jitter_offset(unsigned index, unsigned phase_count, float& x, float& y) const override {
        x = 0.0f;
        y = 0.0f;
        arm::ffxmFsr2GetJitterOffset(&x, &y, static_cast<std::int32_t>(index), static_cast<std::int32_t>(phase_count));
    }

    const char* name() const noexcept override { return "Arm ASR"; }

private:
    std::vector<unsigned char> scratch_;
    arm::FfxmFsr2Context context_{};
    bool context_created_ = false;
    bool reported_failure_ = false;
};

}  // namespace

std::unique_ptr<TemporalUpscalerVk> create_arm_asr_upscaler(unsigned max_render_width, unsigned max_render_height,
                                                            unsigned output_width, unsigned output_height) {
    if (!Context::get().temporal_upscaling()) {
        return nullptr;
    }
    auto upscaler = std::make_unique<ArmAsrUpscaler>();
    if (!upscaler->create(max_render_width, max_render_height, output_width, output_height)) {
        return nullptr;
    }
    return upscaler;
}

}  // namespace vk
}  // namespace galaxy::gx
