// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// RendererGles: the GX renderer on OpenGL ES 3.2 (docs/RG35XXH.md), the interface GxBackend uses on RendererVk.
//
// GL runs the commands in the order they are made, on the render thread that has the context current: an EFB copy
// or peek reads what the draws before it wrote, and a frame's end inserts a fence sync (gles::Context::submit) the
// rings and retired textures wait on, kFramesInFlight frames apart. The EFB is an RGBA8 colour texture and a 24-bit
// depth texture in one framebuffer, drawn top row first as D3D draws (the vertex shaders negate y,
// shader_translate_gles.h), so copies, peeks and textures keep D3D's orientation; only the present flips.
//
// Draw bindings: uniform blocks 0 (b0, the VS constants) and 1 (b1, the PS constants) at their ring offsets, the
// matrix palette as shader storage block 0 (or a texture buffer at unit 8), the eight textures at units 0-7 with
// sampler objects. Tables (gpu::Table) are ids of texture-name and sampler arrays the renderer keeps.
//
// Normal rendering only: the HUD split, temporal upscaling, interpolated redraw, ray tracing and the post-process
// chain report themselves unavailable, as on Vulkan for the ones Vulkan lacks.

#include "galaxy/gx/efb_copy.h"
#include "galaxy/gx/gles/gles_context.h"
#include "galaxy/gx/gpu_types.h"
#include "galaxy/gx/host_window.h"
#include "galaxy/gx/interp_gpu.h"
#include "galaxy/gx/interp_types.h"
#include "galaxy/gx/render_config.h"
#include "galaxy/gx/renderer_common.h"
#include "galaxy/gx/rt_materials.h"
#include "galaxy/gx/rt_sky_sun.h"
#include "galaxy/gx/rt_types.h"
#include "galaxy/gx/transform_history.h"
#include "galaxy/gx/ui_canvas.h"
#include "galaxy/gx/upload_ring.h"
#include "galaxy/gx/window_owner.h"
#include "galaxy/gx_d3d12.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace galaxy::gx {

class TextureCache;
struct TextureHandle;

namespace gles {

// A GX pipeline: a linked program and the render state it draws with (PipelineCacheGles builds them).
struct Pipeline {
    GLuint program = 0;
    GLenum topology = GL_TRIANGLES;
    std::uint32_t attributes = 0;  // vertex attribute locations the program reads (bit n: location n)
    bool depth_test = false;
    bool depth_write = false;
    GLenum depth_func = GL_ALWAYS;
    bool blend = false;
    GLenum blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO, blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
    GLenum blend_op_rgb = GL_FUNC_ADD, blend_op_alpha = GL_FUNC_ADD;
    bool write_rgb = true;
    bool write_alpha = true;
    GLenum cull = 0;  // 0: none, else GL_FRONT or GL_BACK
    bool uber = false;
};

// What PipelineCacheGles builds GX pipelines against (RendererGles::root_signature()).
struct GxLayout {
    bool palette_storage_block = true;
    bool framebuffer_fetch = false;
};

// One of the renderer's own programs: its uniform block at binding 0 takes 64 bytes of constants.
struct UtilityProgram {
    GLuint program = 0;
};

}  // namespace gles

class RendererGles final : public UiCanvas {
public:
    using SubmissionCallback = void (*)(void* context, std::uint64_t fence_value);
    static constexpr std::size_t kAsyncPeekFieldSlots = 3u;
    static constexpr std::size_t kAsyncPeekFieldChannels = 4u;

    bool initialize(int window_width, int window_height, unsigned efb_scale);
    [[nodiscard]] std::string startup_failure() const { return gles::Context::get().failure(); }
    void create_first_use_resources();
    void shutdown();
    [[nodiscard]] UploadBuffer create_upload_buffer(std::size_t bytes, const char* name);

    // ---- Interpolated sub-frames: not on GLES.
    using ReplaySequence = gx::ReplaySequence;
    using SubframeTime = gx::SubframeTime;
    using CopyRedirectStats = gx::CopyRedirectStats;
    using StripeVertex = gx::StripeVertex;
    [[nodiscard]] std::uint64_t playing_serial() const noexcept { return 0u; }
    void prepare_vertex_blend(std::span<const std::int32_t>) {}
    [[nodiscard]] std::span<const std::int32_t> ps_record_pairs() const noexcept { return {}; }
    void record_scene_aa() {}
    [[nodiscard]] bool ambient_occlusion_ready() const noexcept { return false; }
    void set_scene_ao_view(const RtView&) noexcept {}
    void record_scene_ao() noexcept {}
    [[nodiscard]] unsigned replay_reject() const noexcept { return 0u; }
    void reset_subframe_gpu_ema() noexcept {}
    [[nodiscard]] double subframe_gpu_ema_ms() const noexcept { return 0.0; }
    void suspend_temporal_interpolation(double) {}
    [[nodiscard]] bool temporal_interpolation() const noexcept { return false; }
    bool begin_replay_sequence(const ReplaySequence&) { return false; }
    bool present_replay_subframe(const SubframeTime&, const Affine34&, const Affine34&) { return false; }
    void note_stripe_vertices(std::span<const StripeVertex>) {}
    [[nodiscard]] CopyRedirectStats take_copy_redirect_stats() noexcept { return {}; }
    void set_subframe_copy_redirect(int) noexcept {}
    [[nodiscard]] std::uint64_t take_log_gap_ops() noexcept { return 0u; }
    [[nodiscard]] std::uint64_t take_vertex_blend_draws() noexcept { return 0u; }
    void take_sequence_record_ms(double& total_ms, std::uint64_t& samples, std::uint64_t& replays) noexcept {
        total_ms = 0.0;
        samples = 0u;
        replays = 0u;
    }
    [[nodiscard]] std::uint64_t take_subframe_guarded() noexcept { return 0u; }
    void take_subframe_path_gpu_ms(std::array<double, 3>& total_ms, std::array<std::uint64_t, 3>& samples) noexcept {
        total_ms = {};
        samples = {};
    }
    [[nodiscard]] std::array<double, 3> take_subframe_phase_ms() noexcept { return {}; }
    void take_subframe_record_ms(double& total_ms, std::uint64_t& samples) noexcept {
        total_ms = 0.0;
        samples = 0u;
    }
    void set_capture_waits(bool) noexcept {}
    void set_resident_vertices(gpu::Address, std::uint64_t) noexcept {}
    void set_scene_view(const std::array<float, 12>*) noexcept {}
    [[nodiscard]] bool rt_active() const noexcept { return false; }
    [[nodiscard]] bool rt_engine() const noexcept { return false; }
    [[nodiscard]] bool rt_log_seq() const noexcept { return false; }
    [[nodiscard]] bool rt_scene_untraced() const noexcept { return false; }
    void rt_set_scene_untraced(bool) noexcept {}
    void begin_hdr_scene() noexcept {}
    void end_hdr_scene() noexcept {}
    [[nodiscard]] bool rt_scene_open() const noexcept { return false; }
    [[nodiscard]] bool rt_glow_open() const noexcept { return false; }
    [[nodiscard]] bool rt_water_open() const noexcept { return false; }
    [[nodiscard]] bool rt_late_open() const noexcept { return false; }
    [[nodiscard]] bool rt_sky_open() const noexcept { return false; }
    void rt_begin_sky() noexcept {}
    void rt_set_scene_up(const std::array<float, 3>&) noexcept {}
    [[nodiscard]] bool rt_lighting() const noexcept { return false; }
    [[nodiscard]] bool rt_ab_original() const noexcept { return false; }
    void rt_set_ab_original(bool) noexcept {}
    [[nodiscard]] bool rt_reflections_active() const noexcept { return false; }
    void rt_begin_scene() {}
    void rt_capture_picture() {}
    void rt_trace_scene() {}
    void rt_trace_water() {}
    void rt_hold_sun(bool) noexcept {}
    void rt_reset_sky() noexcept {}
    void rt_set_camera(const std::array<float, 12>*) noexcept {}
    void rt_set_view(const RtView&) noexcept {}
    [[nodiscard]] bool rt_physical() const noexcept { return false; }
    void rt_set_presents(std::uint64_t) noexcept {}
    void rt_set_key_light(bool, const std::array<float, 3>&, const std::array<float, 3>&, float, bool) noexcept {}
    void rt_set_stage_sun(const RtStageSun*) noexcept {}
    [[nodiscard]] const RtStageSun* rt_stage_sun() const noexcept { return nullptr; }
    [[nodiscard]] bool rt_sky_key(std::array<float, 3>&, float&) const noexcept { return false; }
    [[nodiscard]] bool rt_sky_sun(std::array<float, 3>&, std::array<float, 3>&) const noexcept { return false; }
    [[nodiscard]] bool rt_sun_direction(std::array<float, 3>&) const noexcept { return false; }
    void rt_note_draw(const DrawCall&, const RtDrawInfo&) {}
    [[nodiscard]] float rt_glow_exposure(bool = false) const noexcept { return 1.0f; }
    [[nodiscard]] float rt_glow_spill(bool) const noexcept { return 0.0f; }
    [[nodiscard]] const void* rt_light_texture() const noexcept { return nullptr; }
    [[nodiscard]] bool rt_light_texture_ready() const noexcept { return false; }
    [[nodiscard]] float rt_light_texture_scale() const noexcept { return 1.0f; }
    [[nodiscard]] unsigned replay_hold_segment() const noexcept { return ~0u; }
    void request_eval_readback() noexcept {}
    [[nodiscard]] bool take_eval_readback(std::vector<std::uint32_t>&, float&) { return false; }
    [[nodiscard]] bool temporal_mode() const { return false; }
    [[nodiscard]] bool copy_output_efb_to_texture(const EfbCopyParams& params, TextureCache& textures);

    [[nodiscard]] gpu::Table texture_table_for(const TextureHandle* handles, unsigned count, const TextureCache& cache,
                                               bool lit_table = false);
    void attach_texture_uploads(TextureCache& cache);
    void drain_gpu();
    void pump_messages() {}
    [[nodiscard]] bool quit_requested() const noexcept;

    void begin_frame(bool present_swap_chain = true);
    std::uint64_t end_frame(bool present_swap_chain = true, SubmissionCallback on_submitted = nullptr,
                            void* submission_context = nullptr);
    [[nodiscard]] std::uint64_t completed_fence_value() const;
    void wait_for_fence_value(std::uint64_t fence_value);

    void bind_efb(const float xf_viewport[6]);
    void clear_efb(std::uint32_t clear_ar, std::uint32_t clear_gb, std::uint32_t clear_z24, bool color_enable,
                   bool alpha_enable, bool depth_enable, unsigned rect_x = 0, unsigned rect_y = 0, unsigned rect_w = 0,
                   unsigned rect_h = 0, std::uint8_t pixel_format = 0);
    void draw(const DrawCall& call);
    void copy_efb_to_xfb(const EfbCopyParams& params, XfbTexture& dest);
    [[nodiscard]] bool copy_efb_to_texture(const EfbCopyParams& params, TextureCache& textures, bool is_depth_copy);
    bool present(const XfbTexture* source, bool capture_backbuffer = false, bool black = false);

    // The 2D layer at the window's resolution: not on GLES (one EFB).
    [[nodiscard]] bool begin_output_efb() { return false; }
    void end_output_efb() {}
    [[nodiscard]] bool output_efb_active() const noexcept { return false; }
    // The display copy about to run is followed by a clear of the whole EFB depth: a tiled GPU need not store
    // the depth it leaves behind.
    void discard_efb_depth_at_next_copy() noexcept { discard_depth_at_copy_ = true; }
    // The display copy about to run belongs to a skipped frame: the EFB has not changed since the last one.
    void repeat_next_display_copy() noexcept { xfb_repeat_ = true; }
    // The texture the last converted EFB copy wrote (tests).
    [[nodiscard]] const gpu::TextureRef& last_efb_copy() const noexcept { return last_efb_copy_; }
    [[nodiscard]] bool scene_temporal_begin(float&, float&) noexcept { return false; }
    [[nodiscard]] bool begin_motion_scene() noexcept { return false; }
    void record_motion_debug_view() noexcept {}
    void reset_temporal_history() noexcept {}
    [[nodiscard]] unsigned matrix_history_frames() const noexcept { return 0u; }
    [[nodiscard]] std::uint64_t matrix_segment_serial() const noexcept { return 0u; }
    [[nodiscard]] bool replay_available() const noexcept { return false; }
    bool start_replay_playback() { return false; }
    [[nodiscard]] bool replay_playing() const noexcept { return false; }
    [[nodiscard]] bool interpolation_mode() const { return false; }
    [[nodiscard]] std::uint64_t replay_serial() const noexcept { return 0u; }
    [[nodiscard]] std::uint32_t replay_xfb_addr() const noexcept { return 0u; }
    void note_replay_scene() noexcept {}
    [[nodiscard]] unsigned replay_failure() const noexcept { return 0u; }
    void note_replay_failure(unsigned) noexcept {}
    void set_subframe_capture(bool) noexcept {}
    void take_subframe_gpu_ms(double& total_ms, std::uint64_t& samples) noexcept {
        total_ms = 0.0;
        samples = 0u;
    }
    void set_scene_camera(float, float, float) noexcept {}

    [[nodiscard]] double measured_present_hz() const noexcept {
        return measured_present_hz_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }

    // GXPeek and the pointer's depth field: GL reads the EFB as the draws before left it.
    [[nodiscard]] bool peek_efb(std::uint16_t x, std::uint16_t y, EfbPeekKind kind, std::uint32_t& value);
    [[nodiscard]] bool capture_pointer_depth(std::span<std::uint32_t> pixels);
    [[nodiscard]] std::uint64_t submit_pointer_depth_capture();
    [[nodiscard]] bool record_pointer_depth_capture_in_frame();
    [[nodiscard]] std::uint64_t pointer_depth_capture_fence() const noexcept { return pointer_depth_fence_; }
    [[nodiscard]] bool try_complete_pointer_depth_capture_in_place(const std::uint8_t*& rgba);
    [[nodiscard]] std::uint64_t submit_async_peek_field(std::size_t channel, EfbPeekKind kind, std::size_t slot);
    [[nodiscard]] bool record_async_peek_field_in_frame(std::size_t channel, EfbPeekKind kind, std::size_t slot);
    [[nodiscard]] bool try_complete_async_peek_field(std::size_t channel);
    [[nodiscard]] const std::uint8_t* async_peek_field_rgba(std::size_t channel, std::size_t slot) const noexcept;
    [[nodiscard]] bool async_peek_field_pending(std::size_t channel) const noexcept {
        return async_fields_[channel].fence != 0u;
    }
    [[nodiscard]] bool any_async_peek_field_pending() const noexcept {
        for (const AsyncField& field : async_fields_) {
            if (field.fence != 0u) return true;
        }
        return false;
    }

    // Test captures: the EFB and the presented picture as PPM files (paths from set_debug_capture_paths).
    bool debug_copy_efb_to_readback();
    void debug_log_readback();
    void debug_log_backbuffer_readback();
    bool debug_copy_selected_xfb_to_readback(const XfbTexture&) noexcept { return false; }
    void debug_log_selected_xfb_readback() noexcept {}
    void set_debug_capture_paths(std::string efb_ppm_path, std::string backbuffer_ppm_path);

    [[nodiscard]] gpu::Device* device() const noexcept { return gles::Context::get().device(); }
    [[nodiscard]] unsigned efb_scale() const noexcept { return efb_scale_; }
    [[nodiscard]] std::uint8_t efb_samples_log2() const noexcept { return 0u; }
    [[nodiscard]] std::uint8_t efb_variant() const noexcept { return 0u; }
    [[nodiscard]] const gles::GxLayout* root_signature() const noexcept { return &gx_layout_; }
    [[nodiscard]] double display_refresh_hz() const noexcept { return display_refresh_hz_; }
    [[nodiscard]] bool vblank_grid(std::int64_t& vblank, std::int64_t& period) const noexcept;
    [[nodiscard]] UploadRing& vertex_ring() noexcept { return vertex_ring_; }
    [[nodiscard]] UploadRing& index_ring() noexcept { return index_ring_; }
    [[nodiscard]] UploadRing& constant_ring() noexcept { return constant_ring_; }
    [[nodiscard]] UploadRing& matrix_ring() noexcept { return matrix_ring_; }
    [[nodiscard]] unsigned frame_slot() const noexcept { return frame_slot_; }
    [[nodiscard]] std::uint64_t efb_copy_dest_bytes() const noexcept { return efb_copy_dests_bytes_; }
    [[nodiscard]] static std::uint64_t efb_copy_dest_budget_bytes() { return gx::efb_copy_dest_budget_bytes(); }
    [[nodiscard]] std::uint64_t last_latency_wait_us() const noexcept { return 0u; }
    [[nodiscard]] std::uint64_t last_frame_slot_wait_us() const noexcept { return last_frame_slot_wait_us_; }
    [[nodiscard]] std::uint64_t last_timestamp_read_us() const noexcept { return 0u; }
    [[nodiscard]] std::uint64_t last_begin_reset_us() const noexcept { return 0u; }
    [[nodiscard]] gpu::Table default_sampler_table() const noexcept { return default_sampler_table_; }
    [[nodiscard]] gpu::Table sampler_table_for(const std::uint32_t keys[8]);

    // UiCanvas: the overlays are the platform's own screens on KNULLI (host_window_knulli.cpp draws nothing).
    void ensure_native_settings_corner_texture_loaded() override {}
    void draw_ui_textured_quad(int, int, int, int, float, float, float, float, const std::array<float, 4>&) override {}
    void draw_ui_solid_quad(int, int, int, int, const std::array<float, 4>&) override {}
    void draw_ui_glyph(int, int, int, std::uint64_t, const std::array<float, 4>&) override {}

private:
    struct Frame {
        std::uint64_t fence = 0;
        std::vector<gpu::TextureRef> retired;
    };
    struct EfbCopyDest {
        gpu::TextureRef texture;
        unsigned width = 0;
        unsigned height = 0;
        std::uint64_t bytes = 0;
        std::uint64_t last_used = 0;
    };
    // A pixel-pack buffer a field or capture is read into, mapped once its fence passed.
    struct Readback {
        GLuint buffer = 0;
        std::size_t bytes = 0;
        std::uint64_t fence = 0;
        std::vector<std::uint8_t> data;  // what the mapping held (a field: always 640x528)
        // A field read at the divided EFB size: the mapping holds width x height texels, copied to staging (the
        // mapping is uncached, so it is read once in bulk) and expanded into data (logical x -> texel x*num/den).
        bool expand = false;
        unsigned num = 1;
        unsigned den = 1;
        unsigned width = 0;
        unsigned height = 0;
        std::vector<std::uint8_t> staging;
    };
    struct AsyncField {
        gpu::TextureRef target;
        std::array<Readback, kAsyncPeekFieldSlots> readbacks;
        std::uint64_t fence = 0;
        std::size_t pending_slot = 0;
    };
    struct TextureTable {
        std::array<GLuint, 8> names{};
    };
    struct SamplerTable {
        std::array<GLuint, 8> samplers{};
    };

    bool make_current();
    [[nodiscard]] unsigned efb_width() const noexcept { return kEfbWidth * efb_scale_ * efb_num_ / efb_den_; }
    [[nodiscard]] unsigned efb_height() const noexcept { return kEfbHeight * efb_scale_ * efb_num_ / efb_den_; }
    [[nodiscard]] bool efb_divided() const noexcept { return efb_num_ != efb_den_; }
    // Physical texels per logical pixel (1, 3/4, 1/2...).
    [[nodiscard]] float efb_factor() const noexcept {
        return static_cast<float>(efb_num_) / static_cast<float>(efb_den_);
    }
    [[nodiscard]] std::array<int, 4> physical_rect(const std::array<int, 4>& rect) const noexcept;
    void to_physical(EfbConversionShaderConstants& constants, bool peek) const noexcept;
    bool create_efb(unsigned scale);
    void destroy_efb() noexcept;
    bool create_utility_programs();
    [[nodiscard]] GLuint build_utility_program(std::string_view vs_hlsl, std::string_view ps_hlsl, const char* what);
    void bind_efb_framebuffer();
    void reset_gx_bindings() noexcept;
    void apply_pipeline_state(const gles::Pipeline& pipeline);
    // The renderer's own state for a utility pass (no blend, no depth, every channel written, no cull, no scissor).
    void utility_state(bool depth_write = false, bool write_rgb = true, bool write_alpha = true);
    // 64 bytes at uniform block 0 from the constant ring.
    void bind_utility_constants(const void* data, std::size_t bytes);
    void refresh_default_sampler_table();
    [[nodiscard]] GLuint sampler_for(std::uint32_t key, const RenderConfig& cfg);
    void retire(gpu::TextureRef texture);
    [[nodiscard]] GLuint framebuffer_for(gles::Texture& texture);
    // The conversion shader into `dest` (a render texture), sampling the EFB colour or depth.
    void conversion_draw(GLuint framebuffer, unsigned dest_w, unsigned dest_h, bool depth,
                         const EfbConversionShaderConstants& constants);
    void record_conversion_blit(gles::Texture& dest, unsigned dest_w, unsigned dest_h, const EfbCopyParams& params,
                                bool is_depth_copy, bool intensity, bool keep_alpha, std::uint8_t copy_format,
                                float src_x, float src_y, float step_x, float step_y, float filter_row_offset);
    // The peek conversion of a whole 640x528 field (or one pixel) into `target`, read into `readback`.
    void record_peek_field(gles::Texture& target, EfbPeekKind kind, const EfbConversionShaderConstants& constants,
                           unsigned width, unsigned height, Readback& readback, bool reduced = false);
    bool complete_readback(Readback& readback);
    void ensure_readback(Readback& readback, std::size_t bytes, std::size_t data_bytes);
    void report_stats();

    WindowOwner window_owner_;
    host_window::NativeWindow window_ = nullptr;
    bool window_shown_ = false;
    gles::GxLayout gx_layout_{};
    std::array<Frame, kFramesInFlight> frames_{};
    unsigned frame_slot_ = 0;
    std::uint64_t frame_index_ = 0;
    bool current_ = false;
    bool presented_this_frame_ = false;

    // The EFB.
    unsigned efb_scale_ = 1;
    // GALAXY_GLES_RESOLUTION (percent) or GALAXY_GLES_EFB_DIVISOR: the EFB is num/den of the Wii's size a side.
    unsigned efb_num_ = 1;
    unsigned efb_den_ = 1;
    unsigned peek_field_num_ = 1;  // GALAXY_GLES_PEEK_FIELD: the async peek fields' size, num/den of the Wii's
    unsigned peek_field_den_ = 1;
    gpu::TextureRef efb_color_;
    gpu::TextureRef efb_depth_;
    GLuint efb_framebuffer_ = 0;

    // The renderer's own programs and objects.
    GLuint conversion_program_ = 0;
    GLuint xfb_program_ = 0;           // the display copy's conversion, sharpened (divided EFB), or 0
    GLuint conversion_override_ = 0;   // the program conversion_draw uses instead, while set
    // The last display copy, which a skipped frame's copy repeats (same EFB, same parameters).
    struct XfbKey {
        std::uint32_t src_x = 0, src_y = 0, src_width = 0, src_height = 0;
        std::uint32_t y_scale_raw = 0, filter0_raw = 0, filter1_raw = 0, flags = 0;
        std::uint32_t width = 0, height = 0;
    };
    gpu::TextureRef last_xfb_;
    gpu::TextureRef last_efb_copy_;
    XfbKey last_xfb_key_{};
    bool xfb_repeat_ = false;
    GLuint clear_program_ = 0;        // colour (masked by glColorMask) and depth clears as draws
    GLuint vao_ = 0;                  // the GX vertex layout (GxVertexOut) at binding 0
    GLuint empty_vao_ = 0;            // the renderer's attribute-less draws
    GLuint linear_clamp_ = 0;
    GLuint point_clamp_ = 0;
    GLuint read_framebuffer_ = 0;     // presents and readbacks bind textures here
    std::uint32_t enabled_attributes_ = 0;

    // GX draws.
    UploadRing vertex_ring_;
    UploadRing index_ring_;
    UploadRing constant_ring_;
    UploadRing matrix_ring_;
    std::unordered_map<std::uint64_t, GLuint> samplers_;  // sampler key << 32 | config key
    std::unordered_map<SamplerTableKey, std::uint64_t, SamplerTableKeyHasher> sampler_table_ids_;
    std::vector<SamplerTable> sampler_tables_;  // id - 1
    // Per frame slot (a table id is slot + 1 << 32 | index + 1), cleared when the slot's frame begins again.
    std::vector<std::vector<TextureTable>> texture_tables_;
    gpu::Table default_sampler_table_{};
    std::uint32_t default_sampler_config_ = 0;
    TextureCache* textures_ = nullptr;
    struct Bound {
        const gles::Pipeline* pipeline = nullptr;
        GLuint program = ~0u;
        bool state_valid = false;
        bool depth_test = false, depth_write = true, blend = false, write_rgb = true, write_alpha = true;
        GLenum depth_func = GL_LESS;
        GLenum blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO, blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
        GLenum blend_op_rgb = GL_FUNC_ADD, blend_op_alpha = GL_FUNC_ADD;
        GLenum cull = 0;
        gpu::Address vs_constants = 0, ps_constants = 0, palette = 0;
        std::array<GLuint, 3> buffers{};
        std::array<std::uint64_t, 3> offsets{};
        std::array<std::uint64_t, 3> sizes{};
        bool constants_valid = false;
        std::array<GLuint, 8> textures{};
        std::array<GLuint, 8> samplers{};
        bool textures_valid = false;
        bool samplers_valid = false;
        gpu::Address vertex_address = ~gpu::Address{0};
        GLuint index_buffer = ~0u;
        std::array<int, 4> viewport{};
        std::array<float, 2> depth_range{};
        bool viewport_valid = false;
        std::array<int, 4> scissor{};
        bool scissor_valid = false;
        gpu::Rect scissor_input{};
        bool scissor_input_valid = false;
        GLuint framebuffer = ~0u;
        GLuint palette_texture = 0;
    } bound_;
    std::array<int, 4> efb_viewport_{};  // bind_efb's
    GLuint palette_texture_ = 0;        // the texture-buffer palette's texture (no vertex SSBOs)

    // EFB copies.
    std::unordered_map<std::uint64_t, EfbCopyDest> efb_copy_dests_;
    std::uint64_t efb_copy_dests_bytes_ = 0;
    std::uint64_t efb_copy_dests_tick_ = 0;

    // Peeks.
    gpu::TextureRef peek_target_;
    gpu::TextureRef field_target_;
    Readback peek_readback_;
    static constexpr std::size_t kPointerDepthRing = 3u;
    std::array<Readback, kPointerDepthRing> pointer_depth_ring_;
    std::size_t pointer_depth_next_ = 0;
    Readback* pointer_depth_readback_ = nullptr;
    std::uint64_t pointer_depth_fence_ = 0;
    bool pointer_depth_in_frame_ = false;
    std::array<AsyncField, kAsyncPeekFieldChannels> async_fields_{};
    static constexpr std::uint64_t kAsyncPeekInFrame = ~std::uint64_t{0};
    void ensure_async_field(AsyncField& field);

    // Captures and telemetry.
    std::string efb_capture_path_;
    std::string backbuffer_capture_path_;
    std::atomic<double> measured_present_hz_{0.0};
    double display_refresh_hz_ = 0.0;
    std::uint64_t last_frame_slot_wait_us_ = 0;
    std::int64_t present_window_start_ns_ = 0;
    std::uint64_t present_window_count_ = 0;
    int drawable_width_ = 0;
    int drawable_height_ = 0;
    struct Window {
        std::uint64_t draws = 0;
        std::uint64_t copies = 0;
        std::uint64_t clears = 0;
        std::uint64_t peeks = 0;
        std::uint64_t pipeline_changes = 0;
        std::uint64_t skipped_draws = 0;
        std::uint64_t sync_peeks = 0;
        std::uint64_t sync_fields = 0;
        std::int64_t draw_ns = 0;
        std::int64_t sync_peek_ns = 0;
        std::int64_t present_wait_ns = 0;
        std::int64_t frame_wait_ns = 0;
        std::int64_t readback_ns = 0;
        std::uint64_t readbacks = 0;
        double gpu_ms = 0.0;
        double gpu_max_ms = 0.0;
        unsigned gpu_frames = 0;
        std::array<double, 5> gpu_part_ms{};  // GpuPart order
        std::uint64_t vertices = 0;
        std::uint64_t depth_discards = 0;
        std::uint64_t xfb_repeats = 0;
    } window_stats_;
    // GPU time by part of the frame: one GL_TIME_ELAPSED query per stretch of commands of one part (a frame's
    // queries sum to its time), read when the frame slot comes round.
    enum GpuPart : int { kGpuScene = 0, kGpuCopies, kGpuXfb, kGpuPeeks, kGpuPresent, kGpuNone = -1 };
    struct GpuSegments {
        std::vector<GLuint> queries;
        std::vector<std::uint8_t> parts;
        std::size_t used = 0;
        bool pending = false;
    };
    void gpu_part(int part);
    void read_gpu_parts(GpuSegments& segments);
    std::array<GpuSegments, kFramesInFlight> gpu_segments_{};
    int gpu_part_ = kGpuNone;
    bool gpu_in_frame_ = false;
    bool timer_queries_ = false;
    bool discard_depth_at_copy_ = false;
};

using Renderer = RendererGles;

}  // namespace galaxy::gx
