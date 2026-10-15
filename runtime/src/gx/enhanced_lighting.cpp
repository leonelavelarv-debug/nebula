// SPDX-License-Identifier: GPL-3.0-only
#include "galaxy/gx/enhanced_lighting.h"

#include "galaxy/gx/gx_bitfields.h"
#include "galaxy/gx/rt_materials.h"
#include "galaxy/gx/rt_surfaces.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <string>

namespace galaxy::gx {
namespace {

struct TuningField {
    const char* name;
    float EnhancedLightingTuning::*value;
};
constexpr TuningField kTuningFields[] = {
    {"ramp", &EnhancedLightingTuning::ramp},
    {"hemi", &EnhancedLightingTuning::hemi},
    {"rim", &EnhancedLightingTuning::rim},
    {"knee", &EnhancedLightingTuning::knee},
    {"sheen", &EnhancedLightingTuning::sheen},
    {"sheen_rough", &EnhancedLightingTuning::sheen_rough},
    {"metal", &EnhancedLightingTuning::metal},
    {"metal_rough", &EnhancedLightingTuning::metal_rough},
    {"metal_f0", &EnhancedLightingTuning::metal_f0},
    {"env_base", &EnhancedLightingTuning::env_base},
    {"env_graze", &EnhancedLightingTuning::env_graze},
    {"point", &EnhancedLightingTuning::point},
    {"point_cap", &EnhancedLightingTuning::point_cap},
};

EnhancedLightingTuning parse_tuning() {
    EnhancedLightingTuning t{};
    char text[512]{};
    std::size_t length = 0;
    if (getenv_s(&length, text, sizeof(text), "GALAXY_ENHANCED_LIGHTING_TUNE") != 0 || length <= 1u) {
        return t;
    }
    const std::string s(text);
    for (std::size_t at = 0; at < s.size();) {
        std::size_t end = s.find_first_of(",;", at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        const std::size_t eq = item.find('=');
        for (const TuningField& field : kTuningFields) {
            if (eq != std::string::npos && item.compare(0, eq, field.name) == 0 &&
                std::char_traits<char>::length(field.name) == eq) {
                t.*field.value = std::strtof(item.c_str() + eq + 1u, nullptr);
            }
        }
        at = end + 1u;
    }
    t.knee = std::clamp(t.knee, 0.05f, 0.99f);
    t.sheen_rough = std::clamp(t.sheen_rough, 0.05f, 1.0f);
    t.metal_rough = std::clamp(t.metal_rough, 0.05f, 1.0f);
    // A changed tuning keys its own shaders (PixelShaderKey::ind_reserved bits 24-31).
    const EnhancedLightingTuning defaults{};
    std::uint64_t hash = 0xCBF29CE484222325ull;
    bool changed = false;
    for (const TuningField& field : kTuningFields) {
        changed |= t.*field.value != defaults.*field.value;
        hash = (hash ^ std::bit_cast<std::uint32_t>(t.*field.value)) * 0x100000001B3ull;
    }
    t.hash = changed ? (static_cast<std::uint32_t>((hash >> 32u) ^ hash) & 0xFFu) | 1u : 0u;
    return t;
}

}  // namespace

const EnhancedLightingTuning& enhanced_lighting_tuning() {
    static const EnhancedLightingTuning tuning = parse_tuning();
    return tuning;
}

std::uint32_t enhanced_table_material(const RtMaterial* material) noexcept {
    if (material == nullptr) {
        return 0u;
    }
    const std::uint32_t source =
        material->emission == RtEmission::Point && material->intensity > 0.0f && !material->no_light
            ? kEnhancedLightSource
            : 0u;
    if (material->roughness < 0.0f && material->metallic < 0.0f && !material->matte) {
        return source;
    }
    // Matte classes (foliage, cloth) absorb light: roughness 1, no highlight.
    const float rough = material->matte ? 1.0f
                        : material->roughness >= 0.0f ? std::clamp(material->roughness, 0.0f, 1.0f)
                                                      : 0.6f;
    const float metal = material->metallic >= 0.0f ? std::clamp(material->metallic, 0.0f, 1.0f) : 0.0f;
    return source | kEnhancedTableKnown |
           (static_cast<std::uint32_t>(std::lround(rough * 63.0f)) << kEnhancedTableRoughShift) |
           (static_cast<std::uint32_t>(std::lround(metal * 15.0f)) << kEnhancedTableMetalShift);
}

bool enhanced_table_light(const RtMaterial* material, float time_s, float out_color[3], float& half,
                          float& reach) noexcept {
    if (material == nullptr || material->emission != RtEmission::Point || material->no_light ||
        !(material->intensity > 0.0f)) {
        return false;
    }
    float intensity = std::min(material->intensity, 8.0f);
    if (material->pulse > 0.0f) {
        intensity *= 0.75f + 0.25f * std::sin(6.2831853f * material->pulse * time_s);
    }
    // A Luma (intensity 1) reaches 350 units; the reach grows with the square root of the light.
    reach = material->range > 0.0f ? std::clamp(material->range, 40.0f, 4000.0f)
                                   : std::clamp(350.0f * std::sqrt(intensity), 60.0f, 2500.0f);
    half = 0.25f * reach;
    for (unsigned k = 0; k < 3u; ++k) {
        out_color[k] = std::clamp(material->color[k], 0.0f, 1.0f) * intensity;
    }
    return true;
}

std::uint32_t enhanced_lighting_material(const VertexShaderKey& vs, const PixelShaderKey& ps,
                                         bool point_lit) noexcept {
    const RtSurface surface = classify_rt_surface(vs, ps);
    // Texcoords from normals that a colour combiner reads (environment maps).
    std::uint32_t env = 0u;
    const unsigned stages = std::min<unsigned>(ps.num_tev_stages, 16u);
    const unsigned texgens = std::min<unsigned>(vs.num_texgens, 8u);
    for (unsigned s = 0; s < stages; ++s) {
        const TevStagePacked& stage = ps.stages[s];
        const unsigned coord = (stage.order >> 3u) & 7u;
        if (((stage.order >> 6u) & 1u) == 0u || coord >= texgens) continue;
        bool reads = false;
        for (unsigned shift = 0; shift < 16u; shift += 4u) {
            const auto input = static_cast<TevColorArg>((stage.color_env >> shift) & 0xFu);
            reads |= input == TevColorArg::TexColor || input == TevColorArg::TexAlpha;
        }
        const XfTexGen texgen = decode_xf_texgen(vs.texgen[coord]);
        if (reads && texgen.type == TexGenType::Regular && texgen.source_row == 1u) {
            env |= 1u << coord;
        }
    }
    // Colour channel 0 lit with a diffuse light function; its lights and sources.
    std::uint32_t material = env;
    const XfChannelCtrl colour = decode_xf_channel_ctrl(vs.channel[0]);
    if ((surface.features & kRtSurfaceUnlit) == 0u && vs.num_chans != 0u && colour.lighting_enable &&
        colour.light_mask != 0u && colour.atten_func != 1u) {
        material |= kEnhancedLit | (static_cast<std::uint32_t>(colour.light_mask) << kEnhancedLightShift) |
                    (static_cast<std::uint32_t>(colour.diffuse_func) << kEnhancedDiffuseShift) |
                    (colour.atten_func == 3u ? kEnhancedSpot : 0u) |
                    (colour.ambient_from_vertex ? kEnhancedAmbientVertex : 0u) |
                    (colour.material_from_vertex ? kEnhancedMaterialVertex : 0u);
    }
    if (point_lit) {
        material |= kEnhancedPointLights;
    }
    if (material == 0u) {
        return 0u;
    }
    if (surface.surface_class == RtSurfaceClass::Metal || surface.surface_class == RtSurfaceClass::Water) {
        material |= kEnhancedReflective;
    }
    return material | (enhanced_lighting_tuning().hash << kEnhancedTuneShift);
}

}  // namespace galaxy::gx
