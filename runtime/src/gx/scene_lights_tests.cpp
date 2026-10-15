// SPDX-License-Identifier: GPL-3.0-only
#include "galaxy/gx/enhanced_lighting.h"
#include "galaxy/gx/rt_materials.h"
#include "galaxy/gx/scene_lights.h"
#include "galaxy/gx/shader_keys.h"

#include <array>
#include <cmath>
#include <iostream>

namespace {

using galaxy::gx::RtEmission;
using galaxy::gx::RtMaterial;
using galaxy::gx::ScenePointLight;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f * (1.0f + std::fabs(b)); }

bool affine() {
    // A camera turned a quarter about y and moved.
    const std::array<float, 12> view{0.0f, 0.0f, -1.0f, 10.0f, 0.0f, 1.0f, 0.0f, -20.0f, 1.0f, 0.0f, 0.0f, 30.0f};
    std::array<float, 12> inverse{};
    bool ok = expect(galaxy::gx::affine_invert(view, inverse), "a rigid transform inverts");
    const std::array<float, 3> world{3.0f, -4.0f, 5.0f};
    const std::array<float, 3> eye = galaxy::gx::affine_apply(view, world);
    const std::array<float, 3> back = galaxy::gx::affine_apply(inverse, eye);
    ok &= expect(near(eye[0], 5.0f) && near(eye[1], -24.0f) && near(eye[2], 33.0f), "world to eye");
    ok &= expect(near(back[0], 3.0f) && near(back[1], -4.0f) && near(back[2], 5.0f), "eye to world and back");
    std::array<float, 12> singular{};
    ok &= expect(!galaxy::gx::affine_invert(singular, inverse), "a singular transform does not invert");
    return ok;
}

bool emitters_join() {
    galaxy::gx::SceneEmitters emitters;
    emitters.add({0.0f, 0.0f, 0.0f}, {1.0f, 0.9f, 0.3f}, 80.0f, 320.0f);
    // Another draw of the same Luma: joins it.
    emitters.add({30.0f, 10.0f, 0.0f}, {1.0f, 0.9f, 0.3f}, 80.0f, 320.0f);
    // A blue Luma beside it: its own light.
    emitters.add({40.0f, 0.0f, 0.0f}, {0.3f, 0.5f, 1.0f}, 80.0f, 320.0f);
    // Far away: its own light.
    emitters.add({2000.0f, 0.0f, 0.0f}, {1.0f, 0.9f, 0.3f}, 80.0f, 320.0f);
    bool ok = expect(emitters.lights().size() == 3u, "draws of one light source join");
    for (std::size_t i = 0; i < galaxy::gx::SceneEmitters::kCapacity + 4u; ++i) {
        emitters.add({5000.0f * static_cast<float>(i + 1u), 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 10.0f, 40.0f);
    }
    ok &= expect(emitters.lights().size() == galaxy::gx::SceneEmitters::kCapacity, "at most the capacity");
    emitters.clear();
    ok &= expect(emitters.lights().empty(), "cleared");
    return ok;
}

bool packing() {
    std::array<ScenePointLight, 2> lights{};
    lights[0] = {{1.0f, 2.0f, -3.0f}, {0.5f, 0.6f, 0.7f}, 100.0f, 400.0f, false, 1.0f};
    lights[1] = {{4.0f, 5.0f, -6.0f}, {1.0f, 1.0f, 1.0f}, 50.0f, 600.0f, true, 0.5f};
    std::array<float, 4u * galaxy::gx::kScenePointLightTexels> record{};
    record.fill(9.0f);
    galaxy::gx::pack_scene_point_lights(lights, record);
    bool ok = expect(record[0] == 2.0f && record[1] == 0.0f, "texel 0: the count");
    ok &= expect(record[4] == 1.0f && record[6] == -3.0f && record[7] == 400.0f, "first light: position, reach");
    ok &= expect(record[8] == 0.5f && record[10] == 0.7f && near(record[11], 1.0f / 10000.0f),
                 "first light: colour, 1 / half^2");
    ok &= expect(record[15] == -600.0f, "a game light's reach is negative");
    ok &= expect(record[20] == 0.0f && record.back() == 0.0f, "the rest cleared");
    return ok;
}

bool table_lights() {
    RtMaterial luma{};
    luma.emission = RtEmission::Point;
    luma.intensity = 1.0f;
    luma.color[0] = 1.0f;
    luma.color[1] = 0.8f;
    luma.color[2] = 0.2f;
    float color[3]{};
    float half = 0.0f;
    float reach = 0.0f;
    bool ok = expect(galaxy::gx::enhanced_table_light(&luma, 0.0f, color, half, reach), "a Luma lights");
    ok &= expect(near(reach, 350.0f) && near(half, 87.5f) && near(color[1], 0.8f), "a Luma's reach and colour");
    RtMaterial bit = luma;
    bit.intensity = 0.15f;
    ok &= expect(galaxy::gx::enhanced_table_light(&bit, 0.0f, color, half, reach) && reach < 150.0f && reach > 120.0f,
                 "a star bit's reach grows with the square root of its light");
    RtMaterial ranged = luma;
    ranged.range = 900.0f;
    ok &= expect(galaxy::gx::enhanced_table_light(&ranged, 0.0f, color, half, reach) && near(reach, 900.0f),
                 "the table's range");
    RtMaterial skipped = luma;
    skipped.no_light = true;
    ok &= expect(!galaxy::gx::enhanced_table_light(&skipped, 0.0f, color, half, reach), "skip: no light");
    RtMaterial lava = luma;
    lava.emission = RtEmission::Area;
    ok &= expect(!galaxy::gx::enhanced_table_light(&lava, 0.0f, color, half, reach), "area emitters are no point");
    ok &= expect(!galaxy::gx::enhanced_table_light(nullptr, 0.0f, color, half, reach), "no material");
    // The uber header: light sources keep their own light; matte classes roughness 1.
    using namespace galaxy::gx;
    ok &= expect((enhanced_table_material(&luma) & kEnhancedLightSource) != 0u, "a light source's header");
    RtMaterial leaf{};
    leaf.matte = true;
    const std::uint32_t matte = enhanced_table_material(&leaf);
    ok &= expect((matte & kEnhancedTableKnown) != 0u && ((matte >> kEnhancedTableRoughShift) & 63u) == 63u &&
                     (matte & kEnhancedLightSource) == 0u,
                 "matte: roughness 1");
    ok &= expect(enhanced_table_material(nullptr) == 0u, "no material: no bits");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= affine();
    ok &= emitters_join();
    ok &= packing();
    ok &= table_lights();
    if (!ok) {
        return 1;
    }
    std::cout << "scene lights tests passed\n";
    return 0;
}
