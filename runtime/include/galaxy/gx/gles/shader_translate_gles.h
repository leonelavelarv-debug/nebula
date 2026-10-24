// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The GX shaders (ShaderGenerator's HLSL, the uber shaders) and the renderer's own shaders as GLSL ES 3.20: the HLSL
// goes through glslang's HLSL front end to SPIR-V (vk::compile_hlsl, with the D3D12 sources unchanged), then
// SPIRV-Cross writes GLSL ES with GL bindings:
//   b0, b1             uniform blocks 0 and 1 (every cbuffer is a uniform block; the renderer fills them from rings)
//   t0, space1         the matrix palette: shader storage block 0, or (no vertex SSBOs) a usamplerBuffer at unit 8
//   t0-t7 with s0-s7   one sampler2D each at units 0-7 (tex[i] with samp[i]); a texture read with two samplers (the
//                      EFB conversion's s0 and s1) takes the samplers' units
// Before glslang the GX sources lose interpolated redraw's vertex blending (no sub-frames on GLES) and their texture
// arrays become single textures. The vertex stage writes D3D's clip space for GL: y negated (render targets are drawn
// top row first, as on D3D, so textures and copies keep D3D's orientation) and z from [0, 1] to [-1, 1].

#include <cstdint>
#include <string>
#include <string_view>

namespace galaxy::gx::gles {

enum class Stage { Vertex, Fragment };

enum class ShaderKind {
    Gx,       // ShaderGenerator / uber: b0, b1, the palette, tex[8] + samp[8]
    Utility,  // the renderer's own: cbuffer b0, t0 with s0 (and s1)
};

struct TranslateOptions {
    bool palette_storage_block = true;  // false: the palette as a texture buffer (unit 8)
    bool keep_names = true;             // readable GLSL (the dumps)
    // GLSL ES 320 (OpenGL ES 3.2: Mali's own drivers), or 310 for OpenGL ES 3.1 drivers (Mesa's Panfrost on
    // Bifrost, e.g. ROCKNIX on the RK3326): the same shaders, extensions declared where 3.1 needs them.
    int glsl_version = 320;
};

// The GLSL ES version for a context of OpenGL ES major.minor (GALAXY_GLES_GLSL=310|320 forces one).
[[nodiscard]] int glsl_version_for(int major, int minor) noexcept;

// Texture unit of the palette's texture buffer.
inline constexpr unsigned kPaletteTextureUnit = 8u;

// Changes with anything that changes translate()'s output for the same source (cached GLSL is keyed on it).
inline constexpr std::uint32_t kTranslatorRevision = 1;

// GLSL ES for `hlsl` (entry `main`); empty with `error` set when it does not translate.
[[nodiscard]] std::string translate(std::string_view hlsl, Stage stage, ShaderKind kind,
                                    const TranslateOptions& options, std::string& error);

// The HLSL changes translate() makes before glslang (exposed for the tests).
[[nodiscard]] std::string prepare_hlsl(std::string_view hlsl, ShaderKind kind, const TranslateOptions& options);

}  // namespace galaxy::gx::gles
