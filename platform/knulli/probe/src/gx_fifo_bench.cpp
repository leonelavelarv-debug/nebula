// SPDX-License-Identifier: GPL-3.0-only
// CPU cost of Nebula's GX front end on this device: a synthetic frame (TEV/BP register writes, XF matrix
// loads, draws) goes through Nebula's own FifoParser and GxState, and each draw builds the vertex, pixel and
// render-state shader keys the backend uses to pick a pipeline. Nothing here is an imitation: these are the
// translation units runtime/src/gx/{fifo_parser,gx_state,shader_keys}.cpp compiled for aarch64.
#include "probe.h"

#include "galaxy/gx/fifo_parser.h"
#include "galaxy/gx/shader_keys.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace {
using galaxy::gx::FifoCursor;
using galaxy::gx::FifoParser;
using galaxy::gx::FifoSink;
using galaxy::gx::GxState;
using galaxy::gx::PrimitiveClass;

constexpr std::size_t kVertexBytes = 16;  // indexed position + colour + two texcoords, a typical J3D format

struct KeySink final : FifoSink {
    GxState* state = nullptr;
    std::uint64_t draws = 0, vertices = 0, key_hash = 0;
    [[nodiscard]] std::size_t draw_payload_size(std::uint8_t, std::uint16_t count) const override { return count * kVertexBytes; }
    void on_draw(PrimitiveClass, std::uint8_t, FifoCursor& cursor) override {
        const std::uint16_t count = cursor.read_u16();
        const auto payload = cursor.take(count * kVertexBytes);
        ++draws; vertices += count;
        // What the backend does per draw before it can bind a pipeline.
        const auto vs = galaxy::gx::build_vertex_shader_key(*state);
        const auto ps = galaxy::gx::build_pixel_shader_key(*state, 1u, false);
        const auto rs = galaxy::gx::build_render_state_key(*state);
        key_hash += sizeof(vs) + sizeof(ps) + sizeof(rs) + static_cast<unsigned>(payload.size());
    }
    void on_efb_copy(std::uint32_t) override {}
    void on_pe_finish() override {}
    void on_pe_token(std::uint16_t, bool) override {}
    void on_invalidate_textures() override {}
    void on_invalidate_vertex_cache() override {}
};

void bp(std::vector<std::byte>& b, std::uint8_t reg, std::uint32_t v) {
    b.push_back(std::byte{0x61}); b.push_back(std::byte{reg});
    b.push_back(std::byte(v >> 16)); b.push_back(std::byte(v >> 8)); b.push_back(std::byte(v));
}
void xf(std::vector<std::byte>& b, std::uint16_t base, const std::uint32_t* v, unsigned n) {
    b.push_back(std::byte{0x10});
    b.push_back(std::byte((n - 1) >> 8)); b.push_back(std::byte(n - 1));
    b.push_back(std::byte(base >> 8)); b.push_back(std::byte(base));
    for (unsigned i = 0; i < n; ++i) { b.push_back(std::byte(v[i] >> 24)); b.push_back(std::byte(v[i] >> 16)); b.push_back(std::byte(v[i] >> 8)); b.push_back(std::byte(v[i])); }
}
void draw(std::vector<std::byte>& b, std::uint8_t opcode, std::uint16_t count) {
    b.push_back(std::byte{opcode}); b.push_back(std::byte(count >> 8)); b.push_back(std::byte(count));
    for (std::size_t i = 0; i < count * kVertexBytes; ++i) b.push_back(std::byte(i * 7));
}
}  // namespace

GxReport run_gx_fifo_bench(unsigned draws_per_frame, unsigned frames) {
    GxReport r;
    std::vector<std::byte> frame;
    const std::uint32_t one = 0x3F800000u, zero = 0;
    for (unsigned d = 0; d < draws_per_frame; ++d) {
        const std::uint32_t m[12] = {one, zero, zero, d * 0x100u, zero, one, zero, zero, zero, zero, one, zero};
        xf(frame, static_cast<std::uint16_t>((d % 10) * 12), m, 12);       // position matrix
        bp(frame, static_cast<std::uint8_t>(0xC0 + 2 * (d % 4)), 0x08F2F0u + d);  // TEV colour env
        bp(frame, static_cast<std::uint8_t>(0xC1 + 2 * (d % 4)), 0x08F2F0u);      // TEV alpha env
        bp(frame, 0x28, 0x3C0040u + (d & 7));                               // TEV order
        draw(frame, static_cast<std::uint8_t>(0x98 | (d % 2)), 24);          // triangle strip, VAT 0/1
    }
    r.frame_bytes = frame.size();
    FifoParser parser; parser.set_dump_dir("/tmp");
    GxState state; KeySink sink; sink.state = &state;
    try {
        parser.run(frame, nullptr, sink, state);  // warm-up
        const auto t0 = std::chrono::steady_clock::now();
        for (unsigned f = 0; f < frames; ++f) parser.run(frame, nullptr, sink, state);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        r.ok = sink.draws == std::uint64_t(draws_per_frame) * (frames + 1);
        r.ms_per_frame = 1e3 * s / frames;
        r.ns_per_draw = 1e9 * s / (double(frames) * draws_per_frame);
        r.mb_per_s = double(frame.size()) * frames / s / 1e6;
        std::snprintf(r.error, sizeof r.error, "draws=%llu hash=%llu", (unsigned long long)sink.draws, (unsigned long long)sink.key_hash);
    } catch (const std::exception& e) {
        r.ok = false; std::snprintf(r.error, sizeof r.error, "%s", e.what());
    }
    return r;
}
