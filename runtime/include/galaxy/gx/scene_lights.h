// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Enhanced lighting's scene point lights: the game's point lights (gx_d3d12.h game_point_lights,
// LightFunction::loadPointLightInfo), the Comet Observatory's star core and the light sources the
// material table names among the scene's draws (rt_materials.h emitter rows), in eye space, the
// strongest at the camera first. A light's colour is its light at the light itself (1: a full GX
// light); it halves at the half distance and is windowed to zero at the reach.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace galaxy::gx {

struct ScenePointLight {
    std::array<float, 3> position{};  // eye space (world space in SceneEmitters)
    std::array<float, 3> color{};
    float half = 0.0f;
    float reach = 0.0f;
    bool game = false;                 // the game's own point light (GX light 4 of the draws it lights)
    float importance = 0.0f;
};

// World-space light sources found among one scene's draws, for the next scene's list. A source
// near an earlier one of about the same colour (several draws of one object) joins it.
class SceneEmitters {
public:
    static constexpr std::size_t kCapacity = 64u;
    void clear() noexcept { count_ = 0u; }
    void add(const std::array<float, 3>& world, const std::array<float, 3>& color, float half,
             float reach) noexcept;
    [[nodiscard]] std::span<const ScenePointLight> lights() const noexcept {
        return {lights_.data(), count_};
    }

private:
    std::array<ScenePointLight, kCapacity> lights_{};
    std::size_t count_ = 0u;
};

struct ScenePointLightRules {
    float game_light = 1.0f;      // the game's point lights' strength
    float saturation = 0.6f;      // their colour: 0 white .. 1 their own
    float default_half = 800.0f;  // half distance of a game light without attenuation
    float core_light = 1.5f;      // the star core's light at scale 1 (0: none)
    float core_radius = 400.0f;   // its radius at scale 1
    std::uint64_t max_age_ms = 500u;
};

inline constexpr unsigned kScenePointLightMax = 16u;
// One record: texel 0 x the light count; per light (eye position, reach; the reach negative for a
// game light) and (colour, 1 / half distance^2).
inline constexpr unsigned kScenePointLightTexels = 1u + 2u * kScenePointLightMax;

// `view`: world to eye, three rows of four. The game's lights, the star core and `emitters` (world
// space) by importance, at most out.size(); lights whose reach lies wholly behind the camera are
// left out.
std::size_t scene_point_lights(const std::array<float, 12>& view, const ScenePointLightRules& rules,
                               std::span<const ScenePointLight> emitters, std::span<ScenePointLight> out);

void pack_scene_point_lights(std::span<const ScenePointLight> lights,
                             std::span<float, 4u * kScenePointLightTexels> out) noexcept;

// Rigid transforms as three rows of four.
[[nodiscard]] std::array<float, 3> affine_apply(const std::array<float, 12>& m, const std::array<float, 3>& p) noexcept;
[[nodiscard]] bool affine_invert(const std::array<float, 12>& m, std::array<float, 12>& out) noexcept;

}  // namespace galaxy::gx
