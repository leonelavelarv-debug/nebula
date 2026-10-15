// SPDX-License-Identifier: GPL-3.0-only
#include "galaxy/gx/scene_lights.h"

#include "galaxy/gx_d3d12.h"

#include <algorithm>
#include <cmath>

namespace galaxy::gx {

void SceneEmitters::add(const std::array<float, 3>& world, const std::array<float, 3>& color, float half,
                        float reach) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        ScenePointLight& l = lights_[i];
        const float dx = l.position[0] - world[0];
        const float dy = l.position[1] - world[1];
        const float dz = l.position[2] - world[2];
        const float join = 0.35f * std::max(l.reach, reach);
        if (dx * dx + dy * dy + dz * dz < join * join &&
            std::abs(l.color[0] - color[0]) + std::abs(l.color[1] - color[1]) + std::abs(l.color[2] - color[2]) <
                0.3f * (color[0] + color[1] + color[2] + 0.1f)) {
            return;
        }
    }
    if (count_ < lights_.size()) {
        lights_[count_++] = {world, color, half, reach, false, 0.0f};
    }
}

std::array<float, 3> affine_apply(const std::array<float, 12>& m, const std::array<float, 3>& p) noexcept {
    std::array<float, 3> e{};
    for (unsigned r = 0; r < 3u; ++r) {
        e[r] = m[r * 4u] * p[0] + m[r * 4u + 1u] * p[1] + m[r * 4u + 2u] * p[2] + m[r * 4u + 3u];
    }
    return e;
}

bool affine_invert(const std::array<float, 12>& m, std::array<float, 12>& out) noexcept {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float co0 = e * i - f * h;
    const float co1 = f * g - d * i;
    const float co2 = d * h - e * g;
    const float det = a * co0 + b * co1 + c * co2;
    if (!std::isfinite(det) || std::abs(det) < 1e-12f) {
        return false;
    }
    const float s = 1.0f / det;
    const std::array<float, 9> r{co0 * s, (c * h - b * i) * s, (b * f - c * e) * s,
                                 co1 * s, (a * i - c * g) * s, (c * d - a * f) * s,
                                 co2 * s, (b * g - a * h) * s, (a * e - b * d) * s};
    for (unsigned row = 0; row < 3u; ++row) {
        out[row * 4u] = r[row * 3u];
        out[row * 4u + 1u] = r[row * 3u + 1u];
        out[row * 4u + 2u] = r[row * 3u + 2u];
        out[row * 4u + 3u] = -(r[row * 3u] * m[3] + r[row * 3u + 1u] * m[7] + r[row * 3u + 2u] * m[11]);
    }
    return true;
}

std::size_t scene_point_lights(const std::array<float, 12>& view, const ScenePointLightRules& rules,
                               std::span<const ScenePointLight> emitters, std::span<ScenePointLight> out) {
    std::array<ScenePointLight, 32u + 1u + SceneEmitters::kCapacity> candidates{};
    std::size_t used = 0u;
    const auto offer = [&](ScenePointLight light) {
        const auto& e = light.position;
        // A sphere of light wholly behind the camera lights nothing it sees.
        if (light.reach <= 0.0f || e[2] > light.reach) {
            return;
        }
        const float l = 0.2126f * light.color[0] + 0.7152f * light.color[1] + 0.0722f * light.color[2];
        const float d2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        const float r2 = light.reach * light.reach;
        light.importance = l * r2 / (r2 + d2);
        if (light.importance > 0.0f && used < candidates.size()) {
            candidates[used++] = light;
        }
    };
    if (rules.game_light > 0.0f) {
        std::array<GameLight, 32> lights{};
        const std::size_t count = game_point_lights(lights, rules.max_age_ms);
        for (std::size_t i = 0; i < count; ++i) {
            // Its colour less saturated (light mixes towards white on what it reaches); its falloff
            // the game's own (halved at half_distance), else the rules' default.
            const auto& c0 = lights[i].color;
            const float l0 = 0.2126f * c0[0] + 0.7152f * c0[1] + 0.0722f * c0[2];
            std::array<float, 3> rgb{};
            for (unsigned k = 0; k < 3u; ++k) {
                rgb[k] = (l0 + (c0[k] - l0) * rules.saturation) * rules.game_light;
            }
            const float half = lights[i].half_distance > 1.0f ? lights[i].half_distance : rules.default_half;
            offer({affine_apply(view, lights[i].position), rgb, half, 12.0f * half, true, 0.0f});
        }
    }
    {
        // The star core: the brightest light of the Comet Observatory, growing with it.
        std::array<float, 3> core{};
        float scale = 0.0f;
        if (rules.core_light > 0.0f && star_core(core, scale, rules.max_age_ms)) {
            const float grow = std::clamp(0.4f + 0.35f * scale, 0.5f, 2.5f);
            const float l = rules.core_light * grow;
            const float half = rules.core_radius * scale * 2.5f;
            ScenePointLight light{affine_apply(view, core), {1.0f * l, 0.72f * l, 0.38f * l}, half, 12.0f * half,
                                  false, 0.0f};
            offer(light);
        }
    }
    for (const ScenePointLight& emitter : emitters) {
        ScenePointLight light = emitter;
        light.position = affine_apply(view, emitter.position);
        offer(light);
    }
    const std::size_t n = std::min(used, out.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(n),
                      candidates.begin() + static_cast<std::ptrdiff_t>(used),
                      [](const ScenePointLight& a, const ScenePointLight& b) { return a.importance > b.importance; });
    std::copy_n(candidates.begin(), n, out.begin());
    return n;
}

void pack_scene_point_lights(std::span<const ScenePointLight> lights,
                             std::span<float, 4u * kScenePointLightTexels> out) noexcept {
    std::fill(out.begin(), out.end(), 0.0f);
    const std::size_t n = std::min<std::size_t>(lights.size(), kScenePointLightMax);
    out[0] = static_cast<float>(n);
    for (std::size_t i = 0; i < n; ++i) {
        float* a = out.data() + 4u * (1u + 2u * i);
        float* b = a + 4u;
        const ScenePointLight& l = lights[i];
        const float half = std::max(l.half, 1.0f);
        a[0] = l.position[0];
        a[1] = l.position[1];
        a[2] = l.position[2];
        a[3] = l.game ? -std::max(l.reach, 1.0f) : std::max(l.reach, 1.0f);
        b[0] = l.color[0];
        b[1] = l.color[1];
        b[2] = l.color[2];
        b[3] = 1.0f / (half * half);
    }
}

}  // namespace galaxy::gx
