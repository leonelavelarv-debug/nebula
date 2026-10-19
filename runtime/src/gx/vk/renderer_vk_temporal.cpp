// SPDX-License-Identifier: GPL-3.0-only
// The Vulkan renderer's temporal upscaling (renderer_vk.h), RendererD3D12's FSR 3 path: the jittered scene draws its
// motion vectors and reactive mask into the EFB pass's extra targets; at the scene's end Arm ASR upscales it into the
// output EFB, where the 2D layer then draws.

#include "renderer_vk_internal.h"
#include "galaxy/gx/vk/renderer_vk.h"

#include "galaxy/gx/host_window.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace galaxy::gx {

namespace {

using namespace vk_detail;

// The scene's depth as one float per pixel (the upscaler's depth input).
constexpr char kDepthR32Ps[] = R"HLSL(
Texture2D<float> t0 : register(t0);
float main(float4 pos : SV_Position) : SV_Target0 {
    return t0.Load(int3(pos.xy, 0));
}
)HLSL";

}  // namespace

bool RendererVk::temporal_mode() const {
    return get_render_config().upscaler == 2u && vk::Context::get().temporal_upscaling() && !interpolation_mode();
}

// The temporal scene pass (GxLayout::aux_pass) and the depth copy's pass, with the device.
void RendererVk::create_aux_passes() {
    const VkDevice device = vk::Context::get().device();
    VkAttachmentDescription attachments[4]{};
    for (VkAttachmentDescription& attachment : attachments) {
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[1].format = depth_format_;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2].format = VK_FORMAT_R16G16_SFLOAT;
    attachments[3].format = VK_FORMAT_R8_UNORM;
    const VkAttachmentReference colors[3] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                             {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                             {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 3;
    subpass.pColorAttachments = colors;
    subpass.pDepthStencilAttachment = &depth;
    // As the EFB pass's (create_layouts).
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 4;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    vk::check(vkCreateRenderPass(device, &info, nullptr, &gx_layout_.aux_pass), "temporal scene pass");
    // A copy-clear clears the EFB's targets; the scene's motion and reactive values stay.
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    vk::check(vkCreateRenderPass(device, &info, nullptr, &gx_layout_.aux_pass_clear), "temporal scene clearing pass");

    // The depth copy: one R32F target written whole, ending shader-readable.
    VkAttachmentDescription target{};
    target.format = VK_FORMAT_R32_SFLOAT;
    target.samples = VK_SAMPLE_COUNT_1_BIT;
    target.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    target.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    target.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    target.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription copy{};
    copy.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    copy.colorAttachmentCount = 1;
    copy.pColorAttachments = &color;
    VkSubpassDependency copy_dependencies[2]{};
    copy_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    copy_dependencies[0].dstSubpass = 0;
    copy_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    copy_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    copy_dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    copy_dependencies[1].srcSubpass = 0;
    copy_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    copy_dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    copy_dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    copy_dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    copy_dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    info.attachmentCount = 1;
    info.pAttachments = &target;
    info.pSubpasses = &copy;
    info.pDependencies = copy_dependencies;
    vk::check(vkCreateRenderPass(device, &info, nullptr, &depth_r32_pass_), "depth copy pass");
}

// The motion, reactive and R32F depth targets at the EFB's scale, and the EFB's temporal-scene framebuffer.
bool RendererVk::ensure_motion_targets() {
    if (gx_layout_.aux_pass == VK_NULL_HANDLE) {
        return false;
    }
    if (efb_.aux_framebuffer != VK_NULL_HANDLE && motion_scale_ == efb_scale_) {
        return true;
    }
    vk::Context& context = vk::Context::get();
    const VkDevice device = context.device();
    const std::uint32_t width = kEfbWidth * efb_scale_;
    const std::uint32_t height = kEfbHeight * efb_scale_;
    if (motion_scale_ != efb_scale_ || !motion_target_) {
        retire(std::move(motion_target_));
        retire(std::move(reactive_target_));
        retire(std::move(depth_r32_));
        if (depth_r32_framebuffer_ != VK_NULL_HANDLE) {
            const VkFramebuffer framebuffer = std::exchange(depth_r32_framebuffer_, VK_NULL_HANDLE);
            context.defer([device, framebuffer] { vkDestroyFramebuffer(device, framebuffer, nullptr); });
        }
        motion_scale_ = 0u;
        constexpr VkImageUsageFlags kTarget = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        gpu::TextureRef motion = context.create_texture(width, height, 1u, VK_FORMAT_R16G16_SFLOAT, kTarget, true,
                                                        "motion vectors");
        gpu::TextureRef reactive = context.create_texture(width, height, 1u, VK_FORMAT_R8_UNORM, kTarget, true,
                                                          "reactive mask");
        gpu::TextureRef depth = context.create_texture(width, height, 1u, VK_FORMAT_R32_SFLOAT, kTarget, true,
                                                       "upscaler depth");
        if (!motion || !reactive || !depth) {
            std::cerr << "[Renderer] temporal targets " << width << 'x' << height << " allocation failed\n";
            return false;
        }
        run_one_time([&](VkCommandBuffer commands) {
            for (const vk::Texture* image : {motion.get(), reactive.get()}) {
                image_barrier(commands, image->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            }
        });
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = depth_r32_pass_;
        info.attachmentCount = 1;
        info.pAttachments = &depth->target_view;
        info.width = width;
        info.height = height;
        info.layers = 1;
        vk::check(vkCreateFramebuffer(device, &info, nullptr, &depth_r32_framebuffer_), "depth copy framebuffer");
        motion_target_ = std::move(motion);
        reactive_target_ = std::move(reactive);
        depth_r32_ = std::move(depth);
        motion_scale_ = efb_scale_;
    }
    if (efb_.aux_framebuffer != VK_NULL_HANDLE) {
        const VkFramebuffer framebuffer = std::exchange(efb_.aux_framebuffer, VK_NULL_HANDLE);
        context.defer([device, framebuffer] { vkDestroyFramebuffer(device, framebuffer, nullptr); });
    }
    const VkImageView views[4] = {efb_.color->target_view, efb_.depth->target_view, motion_target_->target_view,
                                  reactive_target_->target_view};
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = gx_layout_.aux_pass;
    info.attachmentCount = 4;
    info.pAttachments = views;
    info.width = width;
    info.height = height;
    info.layers = 1;
    vk::check(vkCreateFramebuffer(device, &info, nullptr, &efb_.aux_framebuffer), "temporal scene framebuffer");
    if (depth_r32_pipeline_ == VK_NULL_HANDLE) {
        depth_r32_pipeline_ = create_utility_pipeline(depth_r32_pass_, conversion_layout_, blit_vertex_shader_source(),
                                                      kDepthR32Ps, VK_COLOR_COMPONENT_R_BIT, false, false, false);
    }
    return true;
}

// Between frames: the upscaler for the current render and output sizes (made off the render thread; until it is
// ready the scene renders at the render scale and is resampled).
void RendererVk::prepare_temporal_upscaler() {
    const unsigned output_scale = upscale_output_scale_;
    if (!temporal_mode() || output_scale == 0u || efb_scale_ > output_scale || output_efb_active_) {
        if (temporal_ || temporal_pending_.valid()) {
            temporal_pending_ = {};
            drain_gpu();
            temporal_.reset();
        }
        temporal_render_w_ = temporal_render_h_ = 0u;
        temporal_output_w_ = temporal_output_h_ = 0u;
        temporal_history_valid_ = false;
        return;
    }
    const unsigned rw = kEfbWidth * efb_scale_;
    const unsigned rh = kEfbHeight * efb_scale_;
    const unsigned ow = kEfbWidth * output_scale;
    const unsigned oh = kEfbHeight * output_scale;
    // A failed creation is not retried until the sizes change.
    if (rw == temporal_render_w_ && rh == temporal_render_h_ && ow == temporal_output_w_ && oh == temporal_output_h_) {
        if (temporal_pending_.valid() &&
            temporal_pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            temporal_ = temporal_pending_.get();
            temporal_history_valid_ = false;
            if (temporal_) {
                std::cout << "[Renderer] " << temporal_->name() << ' ' << rw << 'x' << rh << " -> " << ow << 'x' << oh
                          << '\n';
            }
        }
        return;
    }
    temporal_pending_ = {};
    if (temporal_) {
        drain_gpu();
        temporal_.reset();
    }
    temporal_history_valid_ = false;
    temporal_render_w_ = rw;
    temporal_render_h_ = rh;
    temporal_output_w_ = ow;
    temporal_output_h_ = oh;
    temporal_pending_ = std::async(std::launch::async, [rw, rh, ow, oh] {
        return vk::create_arm_asr_upscaler(rw, rh, ow, oh);
    });
}

bool RendererVk::scene_temporal_begin(float& jitter_x, float& jitter_y) {
    jitter_x = 0.0f;
    jitter_y = 0.0f;
    scene_temporal_ = false;
    const unsigned rw = kEfbWidth * efb_scale_;
    if (!temporal_ || !temporal_mode() || output_efb_active_ || rw != temporal_render_w_ ||
        kEfbHeight * efb_scale_ != temporal_render_h_) {
        temporal_history_valid_ = false;
        return false;
    }
    const unsigned phases = std::max(temporal_->jitter_phase_count(rw, temporal_output_w_), 1u);
    temporal_->jitter_offset(static_cast<unsigned>(temporal_frame_ % phases), phases, jitter_x, jitter_y);
    ++temporal_frame_;
    scene_jitter_ = {jitter_x, jitter_y};
    scene_temporal_ = true;
    return true;
}

bool RendererVk::begin_motion_scene() {
    if (!scene_temporal_ || !ensure_motion_targets()) {
        return false;
    }
    end_efb_pass();
    efb_aux_active_ = true;
    reset_gx_bindings();
    begin_efb_pass();
    VkClearAttachment clears[2]{};
    clears[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clears[0].colorAttachment = 1;
    clears[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clears[1].colorAttachment = 2;
    const VkClearRect rect{{{0, 0}, {kEfbWidth * efb_scale_, kEfbHeight * efb_scale_}}, 0, 1};
    vkCmdClearAttachments(commands_, 2, clears, 1, &rect);
    return true;
}

bool RendererVk::record_temporal_upscale(unsigned output_scale) {
    if (!temporal_ || motion_scale_ != efb_scale_ || depth_r32_pipeline_ == VK_NULL_HANDLE) {
        return false;
    }
    const unsigned rw = kEfbWidth * efb_scale_;
    const unsigned rh = kEfbHeight * efb_scale_;
    const unsigned ow = kEfbWidth * output_scale;
    const unsigned oh = kEfbHeight * output_scale;
    // The context was made for exactly these sizes (a resolution change between the scene's start and its end would
    // dispatch past its maxima).
    if (rw != temporal_render_w_ || rh != temporal_render_h_ || ow != temporal_output_w_ || oh != temporal_output_h_) {
        return false;
    }
    if (!alt_efb_.color || alt_efb_scale_ != output_scale) {
        if (alt_efb_.color) {
            destroy_efb_set(alt_efb_);
        }
        if (!create_efb_set(alt_efb_, output_scale)) {
            alt_efb_scale_ = 0u;
            return false;
        }
        alt_efb_scale_ = output_scale;
    }
    end_efb_pass();
    const VkCommandBuffer commands = commands_;
    // The depth as R32F.
    transition_efb_for_sampling(commands, true);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = depth_r32_pass_;
    begin.framebuffer = depth_r32_framebuffer_;
    begin.renderArea = {{0, 0}, {rw, rh}};
    vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, static_cast<float>(rh), static_cast<float>(rw), -static_cast<float>(rh), 0.0f,
                              1.0f};
    const VkRect2D scissor{{0, 0}, {rw, rh}};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_r32_pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, conversion_layout_, 0, 1, &efb_.depth_source, 0,
                            nullptr);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    transition_efb_to_attachment(commands, true);

    const auto image = [](const vk::Texture& texture) {
        return vk::TemporalImage{texture.image, texture.format, texture.width, texture.height};
    };
    vk::TemporalUpscaleInputsVk in{};
    in.commands = commands;
    in.color = image(*efb_.color);
    in.depth = image(*depth_r32_);
    in.motion = image(*motion_target_);
    in.reactive = image(*reactive_target_);
    in.output = image(*alt_efb_.color);
    in.render_width = rw;
    in.render_height = rh;
    in.output_width = ow;
    in.output_height = oh;
    in.jitter_x = scene_jitter_[0];
    in.jitter_y = scene_jitter_[1];
    in.camera_near = scene_camera_[0];
    in.camera_far = scene_camera_[1];
    in.fov_y = scene_camera_[2];
    in.frame_ms = 1000.0f / 60.0f;
    in.reset = !temporal_history_valid_;
    const bool recorded = temporal_->evaluate(in);
    // The upscaler binds its own pipelines and sets.
    reset_gx_bindings();
    temporal_history_valid_ = recorded;
    if (!recorded) {
        return false;
    }
    swap_efb_sets();
    bind_efb(nullptr);
    clear_efb(0u, 0u, 0x00FFFFFFu, false, false, true);
    return true;
}

}  // namespace galaxy::gx
