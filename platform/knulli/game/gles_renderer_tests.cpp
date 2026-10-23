// SPDX-License-Identifier: GPL-3.0-only
// RendererGles against the live driver through the KNULLI window layer (SDL2; SDL_VIDEODRIVER=offscreen off the
// handheld): EFB clears in rectangles, peeks of colour and depth, an EFB-to-texture copy, the display copy and the
// present, checked for D3D's orientation (the EFB's top row on the screen's top). Exit status 0 when all pass.

#include "sdl_host.h"

#include "galaxy/gx/gles/gles_context.h"
#include "galaxy/gx/gles/pipeline_cache_gles.h"
#include "galaxy/gx/gles/renderer_gles.h"
#include "galaxy/gx/uber_constants.h"
#include "galaxy/gx/vertex_loader.h"
#include "galaxy/gx/efb_copy.h"
#include "galaxy/gx/host_window.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace galaxy::gx;
using namespace galaxy::gx::gles::gl;

namespace {

int g_failures = 0;

void expect(bool ok, const char* what, std::uint32_t got, std::uint32_t want) {
    std::printf("%s %s: got 0x%08x want 0x%08x\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) {
        ++g_failures;
    }
}

std::uint32_t argb(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (std::uint32_t{a} << 24u) | (std::uint32_t{r} << 16u) | (std::uint32_t{g} << 8u) | b;
}

EfbCopyParams copy_params(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, bool xfb) {
    EfbCopyParams p{};
    p.src_x = x;
    p.src_y = y;
    p.src_width = w;
    p.src_height = h;
    p.dest_addr = 0x00400000u;
    p.dest_stride = w * 2u;
    p.y_scale_raw = 256u;
    p.target_format = xfb ? 0u : 6u;  // RGBA8 for the texture copy
    p.copy_to_xfb = xfb;
    p.filter0_raw = (21u << 12u) | (22u << 18u);  // the standard filter: (0, 64, 0)
    p.filter1_raw = 21u;
    return p;
}

}  // namespace

int main() {
    setenv("NEBULA_WINDOWED", "1", 0);
    if (!galaxy::sdl::start()) {
        return 2;
    }
    RendererGles renderer;
    if (!renderer.initialize(640, 480, 1u)) {
        std::printf("FAIL renderer initialize\n");
        return 2;
    }
    renderer.create_first_use_resources();
    PipelineCacheGles pipelines;
    if (!pipelines.initialize(renderer.device(), renderer.root_signature(), "/tmp/claude-0/gles-test-cache", 1u)) {
        std::printf("FAIL pipeline cache initialize\n");
        return 2;
    }
    // The renderer runs on this thread too.
    renderer.begin_frame(true);
    // Black everywhere and farthest; then red above row 264, blue below; depth 0x123456 on the left half.
    renderer.clear_efb(0x0000u, 0x0000u, 0xFFFFFFu, true, true, true);
    renderer.clear_efb(0xFF00u | 0xFFu, 0x0000u, 0u, true, true, false, 0, 0, 640, 264, 1u);
    renderer.clear_efb(0xFF00u, 0x00FFu, 0u, true, true, false, 0, 264, 640, 264, 1u);
    renderer.clear_efb(0u, 0u, 0x123456u, false, false, true, 0, 0, 320, 528, 1u);
    std::uint32_t value = 0;
    (void)renderer.peek_efb(10, 10, EfbPeekKind::Color, value);
    expect(value == argb(0xFF, 0xFF, 0, 0), "peek colour (10,10) red", value, argb(0xFF, 0xFF, 0, 0));
    (void)renderer.peek_efb(10, 500, EfbPeekKind::Color, value);
    expect(value == argb(0xFF, 0, 0, 0xFF), "peek colour (10,500) blue", value, argb(0xFF, 0, 0, 0xFF));
    (void)renderer.peek_efb(10, 10, EfbPeekKind::Depth, value);
    expect(value == 0x123456u, "peek depth (10,10)", value, 0x123456u);
    (void)renderer.peek_efb(600, 10, EfbPeekKind::Depth, value);
    expect(value == 0xFFFFFFu || value == 0xFFFFFEu, "peek depth (600,10) far", value, 0xFFFFFFu);

    // The pointer's depth field: the whole EFB at once.
    std::vector<std::uint32_t> field(kEfbWidth * kEfbHeight);
    if (renderer.capture_pointer_depth(field)) {
        expect(field[10u * kEfbWidth + 10u] == 0x123456u, "depth field (10,10)", field[10u * kEfbWidth + 10u],
               0x123456u);
        expect(field[500u * kEfbWidth + 600u] >= 0xFFFFFEu, "depth field (600,500)", field[500u * kEfbWidth + 600u],
               0xFFFFFFu);
        // Both sides of the clears' edges (a divided EFB expands each physical texel on the CPU).
        expect(field[263u * kEfbWidth + 319u] == 0x123456u, "depth field (319,263)", field[263u * kEfbWidth + 319u],
               0x123456u);
        expect(field[527u * kEfbWidth + 0u] == 0x123456u, "depth field (0,527)", field[527u * kEfbWidth], 0x123456u);
        expect(field[264u * kEfbWidth + 320u] >= 0xFFFFFEu, "depth field (320,264)", field[264u * kEfbWidth + 320u],
               0xFFFFFFu);
        expect(field[527u * kEfbWidth + 639u] >= 0xFFFFFEu, "depth field (639,527)", field[527u * kEfbWidth + 639u],
               0xFFFFFFu);
    } else {
        expect(false, "depth field captured", 0, 1);
    }

    // An async colour field (the game's later GXPeekARGB answers): red above row 264, blue from it.
    {
        const std::uint64_t fence = renderer.submit_async_peek_field(0u, EfbPeekKind::Color, 1u);
        expect(fence != 0u, "async colour field submitted", fence != 0u, 1u);
        if (fence != 0u) {
            (void)galaxy::gx::gles::Context::get().wait(fence, UINT64_MAX);
            const bool completed = renderer.try_complete_async_peek_field(0u);
            expect(completed, "async colour field completed", completed ? 1u : 0u, 1u);
            const std::uint8_t* rgba = renderer.async_peek_field_rgba(0u, 1u);
            const auto pixel = [&](unsigned x, unsigned y) {
                const std::uint8_t* p = rgba + (static_cast<std::size_t>(y) * kEfbWidth + x) * 4u;
                return argb(p[3], p[0], p[1], p[2]);
            };
            if (rgba != nullptr) {
                expect(pixel(10, 263) == argb(0xFF, 0xFF, 0, 0), "async field (10,263) red", pixel(10, 263),
                       argb(0xFF, 0xFF, 0, 0));
                expect(pixel(639, 0) == argb(0xFF, 0xFF, 0, 0), "async field (639,0) red", pixel(639, 0),
                       argb(0xFF, 0xFF, 0, 0));
                expect(pixel(11, 264) == argb(0xFF, 0, 0, 0xFF), "async field (11,264) blue", pixel(11, 264),
                       argb(0xFF, 0, 0, 0xFF));
                expect(pixel(639, 527) == argb(0xFF, 0, 0, 0xFF), "async field (639,527) blue", pixel(639, 527),
                       argb(0xFF, 0, 0, 0xFF));
            }
        }
    }

    // The display copy and the present: red at the screen's top.
    XfbTexture xfb{};
    xfb.texture = galaxy::gx::gpu::create_render_texture(renderer.device(), 640u, 528u);
    xfb.width = 640u;
    xfb.height = 528u;
    renderer.copy_efb_to_xfb(copy_params(0, 0, 640, 528, true), xfb);
    renderer.present(&xfb, false, false);
    // The window as presented (before the swap): GL's rows from the bottom.
    int w = 0, h = 0;
    host_window::gles_drawable_size(galaxy::sdl::input_state().window_width ? nullptr : nullptr, w, h);
    w = static_cast<int>(host_window::backbuffer_width);
    h = static_cast<int>(host_window::backbuffer_height);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4u);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const auto at = [&](int x, int y_from_top) {
        const std::uint8_t* p = pixels.data() + (static_cast<std::size_t>(h - 1 - y_from_top) * w + x) * 4u;
        return argb(p[3], p[0], p[1], p[2]);
    };
    const std::uint32_t top = at(w / 2, h / 4);
    const std::uint32_t bottom = at(w / 2, h - h / 4);
    expect((top & 0xFFFFFFu) == 0xFF0000u, "screen top red", top, 0xFFFF0000u);
    expect((bottom & 0xFFFFFFu) == 0x0000FFu, "screen bottom blue", bottom, 0xFF0000FFu);
    for (int y = 0; y < h; y += h / 16) {
        std::printf("  row %4d: %08x\n", y, at(w / 2, y));
    }
    // The red/blue edge up close (the display copy's upscale and sharpen).
    for (int y = h / 2 - 5; y <= h / 2 + 5; ++y) {
        std::printf("  edge row %4d: %08x\n", y, at(w / 2, y));
    }
    renderer.end_frame(true);
    renderer.begin_frame(true);

    // A GX draw: the inline position matrix and projection identity, one triangle over the EFB's top-left quarter
    // in D3D's clip space ((-1, 1), (0, 1), (-1, 0): clockwise on the screen, D3D's front, GX's back) at z 0.25, depth
    // written always. Drawn with GX cull Front, culled with cull Back; the depth field shows where.
    const auto draw_triangle = [&](CullMode cull) {
        renderer.clear_efb(0u, 0u, 0xFFFFFFu, true, true, true);
        VertexShaderKey vs_key{};
        PixelShaderKey ps_key{};
        RenderStateKey rs{};
        rs.zmode_bits = 1u | (7u << 1u) | (1u << 4u);
        rs.blend_bits = (1u << 25u) | (1u << 26u);
        rs.cull = static_cast<std::uint8_t>(cull);
        rs.pixfmt = 1u;
        rs.primitive_topology = gpu::kTopologyTypeTriangle;
        const PsoKey key{vs_key.hash(), ps_key.hash(), rs};
        DrawCall call{};
        call.pipeline = pipelines.get(key, ps_key, vs_key);
        if (call.pipeline == nullptr) {
            expect(false, "pipeline built", 0, 1);
            return;
        }
        UploadRing::Allocation vs = renderer.constant_ring().allocate(sizeof(GxVsConstants), 256u);
        auto* constants = reinterpret_cast<GxVsConstants*>(vs.cpu);
        std::memset(constants, 0, sizeof(GxVsConstants));
        for (int i = 0; i < 4; ++i) constants->projection[i][i] = 1.0f;
        for (int i = 0; i < 3; ++i) constants->inline_pos_matrix[i][i] = 1.0f;
        UploadRing::Allocation ps = renderer.constant_ring().allocate(sizeof(GxPsConstants), 256u);
        std::memset(ps.cpu, 0, sizeof(GxPsConstants));
        UploadRing::Allocation palette = renderer.matrix_ring().allocate(GxState::kMatrixPaletteWords * 4u, 256u);
        std::memset(palette.cpu, 0, GxState::kMatrixPaletteWords * 4u);
        UploadRing::Allocation vertices = renderer.vertex_ring().allocate(3u * sizeof(GxVertexOut), 16u);
        auto* v = reinterpret_cast<GxVertexOut*>(vertices.cpu);
        std::memset(v, 0, 3u * sizeof(GxVertexOut));
        const float positions[3][3] = {{-1.0f, 1.0f, 0.25f}, {0.0f, 1.0f, 0.25f}, {-1.0f, 0.0f, 0.25f}};
        for (int i = 0; i < 3; ++i) std::memcpy(reinterpret_cast<std::byte*>(&v[i]), positions[i], 12u);
        call.vs_constants = vs.gpu;
        call.ps_constants = ps.gpu;
        call.matrix_palette = palette.gpu;
        call.vertex_view = gpu::vertex_view(vertices.gpu, 3u * sizeof(GxVertexOut), sizeof(GxVertexOut));
        call.index_count = 3u;
        call.topology = gpu::kTopologyTriangleList;
        call.scissor = gpu::rect(0, 0, 640, 528);
        renderer.bind_efb(nullptr);
        renderer.draw(call);
    };
    draw_triangle(CullMode::Front);
    (void)renderer.peek_efb(10, 10, EfbPeekKind::Depth, value);
    expect(value == 0xC00000u, "triangle covers (10,10) at z 0.25", value, 0xC00000u);
    (void)renderer.peek_efb(630, 10, EfbPeekKind::Depth, value);
    expect(value == 0xFFFFFFu, "triangle leaves (630,10)", value, 0xFFFFFFu);
    (void)renderer.peek_efb(10, 520, EfbPeekKind::Depth, value);
    expect(value == 0xFFFFFFu, "triangle leaves (10,520)", value, 0xFFFFFFu);
    (void)renderer.peek_efb(300, 240, EfbPeekKind::Depth, value);
    expect(value == 0xFFFFFFu, "triangle leaves (300,240), past its diagonal", value, 0xFFFFFFu);
    draw_triangle(CullMode::Back);
    (void)renderer.peek_efb(10, 10, EfbPeekKind::Depth, value);
    expect(value == 0xFFFFFFu, "GX back face culled (cull Back)", value, 0xFFFFFFu);
    renderer.end_frame(true);

    // A second frame: the slots and fences come round.
    renderer.begin_frame(true);
    renderer.clear_efb(0xFF00u | 0x40u, 0x8000u, 0xFFFFFFu, true, true, true);
    (void)renderer.peek_efb(320, 264, EfbPeekKind::Color, value);
    expect(value == argb(0xFF, 0x40, 0x80, 0x00), "frame 2 peek", value, argb(0xFF, 0x40, 0x80, 0x00));
    renderer.end_frame(true);
    renderer.begin_frame(true);
    renderer.end_frame(true);
    pipelines.shutdown();
    renderer.shutdown();
    std::printf("gles renderer tests: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
