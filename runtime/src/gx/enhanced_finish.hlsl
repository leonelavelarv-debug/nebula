// SPDX-License-Identifier: GPL-3.0-only
// Enhanced lighting's finish of the 3D scene without the ray tracer (compiled by FXC at build time,
// shader model 5.0, so every Direct3D 12 GPU runs it): ambient occlusion from the depth buffer at
// half resolution over the opaque scene; at the scene's end a bloom of its light sources (what the
// glows and particles added over the opaque scene, and the brightest surfaces), a mild haze towards
// the sky's colour under bright skies and a filmic grade. The sky and backdrops keep their colours.
// Sub-frames run both again on their own picture.

cbuffer EnhancedFinish : register(b0)
{
    float4 f_projection;     // GX projection A, B, C, D
    float4 f_depth;          // D3D projection [2][2], [2][3]; viewport min depth, max depth
    float4 f_viewport;       // left, top, width, height (EFB pixels)
    uint2 f_size;            // EFB pixels
    uint2 f_half;            // half-resolution texels
    uint2 f_quarter;         // quarter-resolution texels
    uint2 f_eighth;          // eighth-resolution texels
    float f_split;           // left of this column (EFB pixels) the game's own picture
    float f_ao;              // occlusion strength (0: none)
    float f_ao_radius;       // its reach, eye units
    float f_bloom;           // bloom strength
    float f_threshold;       // luminance where the brightest surfaces start to bloom
    float f_glow;            // share of what the glows added that blooms
    float f_haze;            // haze: its largest share
    float f_haze_distance;   // eye distance where the haze reaches 63 % of that share
    float f_grade;           // grade strength (0: none)
    uint f_flags;            // kFlag*; blur passes: kFlagDown
    float f_pad0;
    float f_pad1;
};

static const uint kFlagOcclusion = 1u;   // finish: multiply by the occlusion (not yet applied)
static const uint kFlagGlowBase = 2u;    // finish: the opaque picture at quarter resolution is known
static const uint kFlagDown = 4u;        // blur: down (else across)
static const uint kFlagViewAo = 8u;      // diagnostics: the occlusion alone
static const float kSky = 6.0e4f;        // eye distance from which a pixel is sky or backdrop

Texture2D<float4> f_color : register(t0);         // the EFB colour
Texture2D<float> f_depth_tex : register(t1);      // the EFB depth
Texture2D<float4> f_copy : register(t2);          // the scene's picture before the finish
Texture2D<float> f_half_depth : register(t3);     // eye distance, half resolution
Texture2D<float> f_ao_raw : register(t4);         // occlusion, half resolution
Texture2D<float> f_ao_tex : register(t5);         // occlusion, blurred
Texture2D<float4> f_quarter_tex : register(t6);   // bloom, quarter resolution
Texture2D<float4> f_eighth_tex : register(t7);    // bloom, eighth resolution
Texture2D<float4> f_opaque : register(t8);        // the opaque picture, quarter resolution
StructuredBuffer<float> f_state : register(t9);   // the sky's colour (rgb), known
RWTexture2D<float4> f_out : register(u0);
RWStructuredBuffer<float> f_state_out : register(u2);
SamplerState f_linear : register(s0);

float luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Eye distance of a depth-buffer value; a large value for the far plane (the sky).
float eye_distance(float depth)
{
    if (depth <= f_depth.z + 1e-6f) {
        return 1.0e9f;
    }
    const float ndc = (depth - f_depth.z) / max(f_depth.w - f_depth.z, 1e-7f);
    return f_depth.y / (ndc + f_depth.x);
}

float distance_at(int2 q)
{
    return eye_distance(f_depth_tex.Load(int3(clamp(q, int2(0, 0), int2(f_size) - 1), 0)));
}

// The eye-space point at an EFB pixel and eye distance.
float3 eye_point(float2 pixel, float distance)
{
    const float ndc_x = (pixel.x - f_viewport.x) / f_viewport.z * 2.0f - 1.0f;
    const float ndc_y = 1.0f - (pixel.y - f_viewport.y) / f_viewport.w * 2.0f;
    return float3((ndc_x + f_projection.y) / f_projection.x, (ndc_y + f_projection.w) / f_projection.z, -1.0f) *
           distance;
}

float half_distance(int2 h)
{
    return f_half_depth.Load(int3(clamp(h, int2(0, 0), int2(f_half) - 1), 0));
}

// ---------------------------------------------------------------------------------------------
// Ambient occlusion (the opaque scene, half resolution)
// ---------------------------------------------------------------------------------------------

// The nearest surface of each 2x2 block.
[numthreads(8, 8, 1)]
void HalfDepthCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= f_half)) {
        return;
    }
    const int2 q = int2(id.xy) * 2;
    float d = distance_at(q);
    d = min(d, distance_at(q + int2(1, 0)));
    d = min(d, distance_at(q + int2(0, 1)));
    d = min(d, distance_at(q + int2(1, 1)));
    f_out[id.xy] = float4(d, d, d, d);
}

// Points of a spiral around the pixel (its turn from a 4x4 Bayer pattern, which the blur's 4x4
// footprint averages out) within the reach on screen; each occludes by how far it rises above the
// surface (the cosine to the normal), fading with distance to nothing at the reach.
[numthreads(8, 8, 1)]
void OcclusionCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= f_half)) {
        return;
    }
    const int2 h = int2(id.xy);
    const float d = half_distance(h);
    if (d >= kSky) {
        f_out[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }
    const float2 pixel = float2(h) * 2.0f + 1.0f;
    const float3 P = eye_point(pixel, d);
    const float3 px0 = eye_point(pixel + float2(2.0f, 0.0f), half_distance(h + int2(1, 0)));
    const float3 px1 = eye_point(pixel - float2(2.0f, 0.0f), half_distance(h - int2(1, 0)));
    const float3 py0 = eye_point(pixel + float2(0.0f, 2.0f), half_distance(h + int2(0, 1)));
    const float3 py1 = eye_point(pixel - float2(0.0f, 2.0f), half_distance(h - int2(0, 1)));
    const float3 dx = abs(px0.z - P.z) < abs(P.z - px1.z) ? px0 - P : P - px1;
    const float3 dy = abs(py0.z - P.z) < abs(P.z - py1.z) ? py0 - P : P - py1;
    float3 N = cross(dy, dx);
    N = dot(N, N) > 0.0f ? normalize(N) : float3(0.0f, 0.0f, 1.0f);
    if (dot(N, P) > 0.0f) {
        N = -N;
    }
    // The reach in half-resolution texels.
    const float reach = clamp(f_ao_radius * 0.25f * f_projection.x * f_viewport.z / d, 2.0f, 48.0f);
    static const float kBayer[16] = {0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f,
                                     3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f};
    const float bayer = kBayer[((id.y & 3u) << 2u) | (id.x & 3u)] / 16.0f;
    const float r2 = f_ao_radius * f_ao_radius;
    float occlusion = 0.0f;
    [unroll] for (uint i = 0u; i < 10u; ++i) {
        const float t = (float(i) + 0.5f) / 10.0f;
        const float angle = 6.2831853f * (t * 3.0f + bayer);
        const int2 s = h + int2(round(float2(cos(angle), sin(angle)) * (reach * t)));
        const float3 S = eye_point(float2(s) * 2.0f + 1.0f, half_distance(s));
        const float3 v = S - P;
        const float vv = dot(v, v);
        occlusion += saturate(dot(v, N) * rsqrt(vv + 1e-3f) - 0.1f) * saturate(1.0f - vv / r2);
    }
    const float ao = saturate(1.0f - f_ao * occlusion / 10.0f);
    f_out[id.xy] = float4(ao, ao, ao, ao);
}

// 4x4 blur across surfaces of about the same distance.
[numthreads(8, 8, 1)]
void OcclusionBlurCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= f_half)) {
        return;
    }
    const int2 h = int2(id.xy);
    const float d = half_distance(h);
    float sum = 0.0f;
    float weight = 0.0f;
    [unroll] for (int y = -1; y <= 2; ++y) {
        [unroll] for (int x = -1; x <= 2; ++x) {
            const int2 s = clamp(h + int2(x, y), int2(0, 0), int2(f_half) - 1);
            const float w = saturate(1.0f - abs(half_distance(s) - d) / (0.04f * d + 1.0f));
            sum += f_ao_raw.Load(int3(s, 0)) * w;
            weight += w;
        }
    }
    const float ao = weight > 0.0f ? sum / weight : 1.0f;
    f_out[id.xy] = float4(ao, ao, ao, ao);
}

// The blurred occlusion at an EFB pixel: its four half-resolution neighbours, weighted by their
// distance's agreement with the pixel's.
float occlusion_at(float2 pixel, float d)
{
    const float2 t = pixel * 0.5f - 0.5f;
    const int2 base = int2(floor(t));
    const float2 f = t - float2(base);
    float sum = 0.0f;
    float weight = 0.0f;
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        const int2 o = int2(k & 1u, k >> 1u);
        const int2 s = clamp(base + o, int2(0, 0), int2(f_half) - 1);
        const float w = (o.x != 0 ? f.x : 1.0f - f.x) * (o.y != 0 ? f.y : 1.0f - f.y) *
                        (1.0f / (1e-3f + abs(f_half_depth.Load(int3(s, 0)) - d) / (0.02f * d + 1.0f))) + 1e-5f;
        sum += f_ao_tex.Load(int3(s, 0)) * w;
        weight += w;
    }
    return sum / weight;
}

float4 FinishVS(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1u) & 2u, id & 2u);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

bool outside(float2 pixel)
{
    return pixel.x < f_split || any(pixel < f_viewport.xy) || any(pixel >= f_viewport.xy + f_viewport.zw);
}

// Multiplied into the EFB colour (blend: destination times source).
float4 OcclusionPS(float4 position : SV_Position) : SV_Target0
{
    if (outside(position.xy)) {
        discard;
    }
    const float d = distance_at(int2(position.xy));
    if (d >= kSky) {
        discard;
    }
    const float ao = occlusion_at(position.xy, d);
    return float4(ao, ao, ao, 1.0f);
}

// ---------------------------------------------------------------------------------------------
// Bloom (quarter and eighth resolution)
// ---------------------------------------------------------------------------------------------

// The opaque picture (after its occlusion), averaged over 4x4 EFB pixels.
[numthreads(8, 8, 1)]
void OpaqueCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= f_quarter)) {
        return;
    }
    const float2 inv = 1.0f / float2(f_size);
    const float2 at = float2(id.xy) * 4.0f;
    float3 c = 0.0f;
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        c += f_color.SampleLevel(f_linear, (at + float2(1.0f + (k & 1u) * 2.0f, 1.0f + (k >> 1u) * 2.0f)) * inv, 0.0f).rgb;
    }
    f_out[id.xy] = float4(c * 0.25f, 1.0f);
}

// What blooms: what the glows and particles added over the opaque picture (light sources the game
// draws), and of the scene's surfaces (never the sky) the light above the threshold, under a soft
// knee.
[numthreads(8, 8, 1)]
void BrightCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= f_quarter)) {
        return;
    }
    const float2 inv = 1.0f / float2(f_size);
    const float2 at = float2(id.xy) * 4.0f;
    float3 c = 0.0f;
    float surface = 0.0f;
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        const float2 o = float2(1.0f + (k & 1u) * 2.0f, 1.0f + (k >> 1u) * 2.0f);
        c += f_copy.SampleLevel(f_linear, (at + o) * inv, 0.0f).rgb;
        surface += distance_at(int2(at + o)) < kSky ? 0.25f : 0.0f;
    }
    c *= 0.25f;
    const float l = luminance(c);
    const float knee = 0.1f;
    const float soft = clamp(l - f_threshold + knee, 0.0f, 2.0f * knee);
    float3 bloom = c * (surface * max(soft * soft / (4.0f * knee), l - f_threshold) / max(l, 1e-4f));
    if ((f_flags & kFlagGlowBase) != 0u) {
        const float3 added = max(c - f_opaque.Load(int3(id.xy, 0)).rgb, 0.0f);
        bloom += added * (f_glow * smoothstep(0.2f, 0.6f, l));
    }
    f_out[id.xy] = float4(bloom, 1.0f);
}

// Separable Gaussian (13 taps, sigma 3 texels) across or down (kFlagDown), at quarter and eighth
// resolution.
[numthreads(8, 8, 1)]
void BlurQuarterCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= f_quarter)) {
        return;
    }
    const int2 step = (f_flags & kFlagDown) != 0u ? int2(0, 1) : int2(1, 0);
    float3 sum = 0.0f;
    float weight = 0.0f;
    [unroll] for (int i = -6; i <= 6; ++i) {
        const int2 s = clamp(int2(id.xy) + step * i, int2(0, 0), int2(f_quarter) - 1);
        const float w = exp(-float(i * i) / 18.0f);
        sum += f_quarter_tex.Load(int3(s, 0)).rgb * w;
        weight += w;
    }
    f_out[id.xy] = float4(sum / weight, 1.0f);
}

[numthreads(8, 8, 1)]
void BlurEighthCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= f_eighth)) {
        return;
    }
    const int2 step = (f_flags & kFlagDown) != 0u ? int2(0, 1) : int2(1, 0);
    float3 sum = 0.0f;
    float weight = 0.0f;
    [unroll] for (int i = -6; i <= 6; ++i) {
        const int2 s = clamp(int2(id.xy) + step * i, int2(0, 0), int2(f_eighth) - 1);
        const float w = exp(-float(i * i) / 18.0f);
        sum += f_eighth_tex.Load(int3(s, 0)).rgb * w;
        weight += w;
    }
    f_out[id.xy] = float4(sum / weight, 1.0f);
}

// The blurred quarter, halved again (a wider glow).
[numthreads(8, 8, 1)]
void DownCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= f_eighth)) {
        return;
    }
    const float2 uv = (float2(id.xy) * 2.0f + 1.0f) / float2(f_quarter);
    f_out[id.xy] = float4(f_quarter_tex.SampleLevel(f_linear, uv, 0.0f).rgb, 1.0f);
}

// The sky's colour: the mean of a grid of points where the frame shows sky, over time.
[numthreads(1, 1, 1)]
void SkyCS()
{
    float3 sky = 0.0f;
    float count = 0.0f;
    [loop] for (int y = 0; y < 8; ++y) {
        [loop] for (int x = 0; x < 12; ++x) {
            const int2 q = int2(f_viewport.xy + f_viewport.zw * float2((x + 0.5f) / 12.0f, (y + 0.5f) / 8.0f));
            if (distance_at(q) >= kSky) {
                sky += f_copy.Load(int3(clamp(q, int2(0, 0), int2(f_size) - 1), 0)).rgb;
                count += 1.0f;
            }
        }
    }
    if (count >= 6.0f) {
        sky /= count;
        const bool known = f_state_out[3] != 0.0f;
        [unroll] for (uint k = 0u; k < 3u; ++k) {
            f_state_out[k] = known ? lerp(f_state_out[k], sky[k], 0.05f) : sky[k];
        }
        f_state_out[3] = 1.0f;
    }
}

// ---------------------------------------------------------------------------------------------
// The finish (full screen, into the EFB colour)
// ---------------------------------------------------------------------------------------------

// Filmic contrast on luminance (an S around mid grey that keeps black, white and the hue), vibrance
// (dull colours gain more saturation than vivid ones), warm highlights against cool shadows; light
// above 1 (bloom) rolls off softly into white.
float3 grade(float3 c)
{
    const float3 over = max(c - 0.95f, 0.0f);
    const float spill = max(max(over.r, max(over.g, over.b)) - 0.05f, 0.0f);
    c = saturate(min(c, 0.95f) + 0.05f * (1.0f - exp(-over / 0.05f)) + spill * 0.15f);
    const float l = luminance(c);
    const float lc = saturate(l + 0.45f * (l - 0.5f) * l * (1.0f - l));
    float3 g = c * (lc / max(l, 1e-4f));
    const float chroma = max(g.r, max(g.g, g.b)) - min(g.r, min(g.g, g.b));
    g = lc + (g - lc) * (1.03f + 0.15f * (1.0f - smoothstep(0.0f, 0.5f, chroma)));
    g *= lerp(float3(0.99f, 0.997f, 1.015f), float3(1.012f, 1.0f, 0.988f), smoothstep(0.15f, 0.85f, lc));
    return lerp(c, saturate(g), f_grade);
}

float4 FinishPS(float4 position : SV_Position) : SV_Target0
{
    if (outside(position.xy)) {
        discard;
    }
    const int2 q = int2(position.xy);
    float3 c = f_copy.Load(int3(q, 0)).rgb;
    const float d = distance_at(q);
    if ((f_flags & (kFlagOcclusion | kFlagViewAo)) != 0u && d < kSky) {
        const float ao = occlusion_at(position.xy, d);
        c = (f_flags & kFlagViewAo) != 0u ? ao.xxx : c * ao;
    }
    // Distance haze (aerial perspective) under a bright sky; none under a dark one (space).
    if (f_haze > 0.0f && d < kSky && f_state[3] != 0.0f) {
        const float3 sky = float3(f_state[0], f_state[1], f_state[2]);
        const float amount = f_haze * smoothstep(0.12f, 0.45f, luminance(sky)) *
                             (1.0f - exp(-d / max(f_haze_distance, 1.0f)));
        c = lerp(c, sky, amount);
    }
    const float2 uv = position.xy / float2(f_size);
    c += (0.6f * f_quarter_tex.SampleLevel(f_linear, uv, 0.0f).rgb +
          0.4f * f_eighth_tex.SampleLevel(f_linear, uv, 0.0f).rgb) * f_bloom;
    if (d < kSky) {
        c = grade(c);
    }
    return float4(saturate(c), 1.0f);
}
