// SPDX-License-Identifier: GPL-3.0-only
// NXAccel: native hot paths for the NextOS Mario Kart Wii preparer (runtime/preparers/mkw), following Nebula's
// approach of replacing interpreted hot loops with native code (Nebula's Yaz0, GX texture and DSP paths), but with
// the exact arithmetic of the Python it replaces: gclib's yaz0_yay0.py / texture_utils.py and the preparer's
// nw4r_audio.decode_adpcm. NextOS checks the cooked content, so every output must be byte-identical to the
// pure-Python result; tests/test_equivalence.py checks that against the original Python on random inputs.
//
// C ABI, loaded with ctypes. Every function returns a negative value instead of guessing on input the Python
// would treat differently (truncated data, negative back-references); the shim then runs the original Python.
#include <cstddef>
#include <cstdint>
#include <cstring>

#define NXA_EXPORT extern "C" __attribute__((visibility("default")))

namespace {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

inline u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
inline u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

constexpr long kTruncated = -2, kBadReference = -3, kTooSmall = -4, kBadFormat = -5;
}  // namespace

NXA_EXPORT int nxa_version() { return 1; }

// ------------------------------------------------------------------------------------------------ Yaz0 / Yay0 ---
// Output may run past the header size by the tail of the last back-reference, exactly as gclib's loop does
// (it checks the length only between commands). `cap` must be at least size + 0x111.
NXA_EXPORT long nxa_yaz0_decompress(const u8* src, std::size_t n, u8* dst, std::size_t cap) {
    if (n < 16) return kTruncated;
    const std::size_t size = be32(src + 4);
    if (cap < size + 0x111) return kTooSmall;
    std::size_t s = 16, o = 0;
    u32 mask = 0; int bits = 0;
    while (o < size) {
        if (bits == 0) { if (s >= n) return kTruncated; mask = src[s++]; bits = 8; }
        if (mask & 0x80) {
            if (s >= n) return kTruncated;
            dst[o++] = src[s++];
        } else {
            if (s + 1 >= n) return kTruncated;
            const u8 b1 = src[s], b2 = src[s + 1]; s += 2;
            const std::size_t dist = std::size_t(b1 & 0xF) << 8 | b2;
            std::size_t count = b1 >> 4;
            if (count == 0) { if (s >= n) return kTruncated; count = std::size_t(src[s++]) + 0x12; }
            else count += 2;
            if (dist + 1 > o) return kBadReference;  // Python would index from the end of the list
            const u8* from = dst + o - (dist + 1);
            for (std::size_t i = 0; i < count; ++i) dst[o + i] = from[i];  // overlapping copy, byte order matters
            o += count;
        }
        mask <<= 1; --bits;
    }
    return long(o);
}

NXA_EXPORT long nxa_yay0_decompress(const u8* src, std::size_t n, u8* dst, std::size_t cap) {
    if (n < 16) return kTruncated;
    const std::size_t size = be32(src + 4);
    std::size_t link = be32(src + 8), chunk = be32(src + 12), m = 16, o = 0;
    if (cap < size + 0x111) return kTooSmall;
    u32 mask = 0; int bits = 0;
    while (o < size) {
        if (bits == 0) { if (m + 4 > n) return kTruncated; mask = be32(src + m); m += 4; bits = 32; }
        if (mask & 0x80000000u) {
            if (chunk >= n) return kTruncated;
            dst[o++] = src[chunk++];
        } else {
            if (link + 2 > n) return kTruncated;
            const u16 l = be16(src + link); link += 2;
            const std::size_t dist = l & 0x0FFF;
            std::size_t count = l >> 12;
            if (count == 0) { if (chunk >= n) return kTruncated; count = std::size_t(src[chunk++]) + 0x12; }
            else count += 2;
            if (dist + 1 > o) return kBadReference;
            const u8* from = dst + o - (dist + 1);
            for (std::size_t i = 0; i < count; ++i) dst[o + i] = from[i];
            o += count;
        }
        mask <<= 1; --bits;
    }
    return long(o);
}

namespace {
constexpr std::size_t kMaxRun = 0xFF + 0x12;
// gclib's simple_rle_encode: the longest match in [offset - depth, offset), the earliest position on ties.
inline void rle_encode(const u8* p, std::size_t n, std::size_t offset, std::size_t depth, std::size_t& count, long& pos) {
    const std::size_t start = offset > depth ? offset - depth : 0;
    std::size_t limit = n - offset;
    if (limit > kMaxRun) limit = kMaxRun;
    count = 0; pos = -1;
    for (std::size_t c = start; c < offset; ++c) {
        std::size_t k = 0;
        while (k < limit && p[c + k] == p[offset + k]) ++k;
        if (k > count) { count = k; pos = long(c); }
    }
}
}  // namespace

// gclib's Yaz0.compress, byte for byte (including its one-byte look-ahead and the trailing zero byte).
NXA_EXPORT long nxa_yaz0_compress(const u8* src, std::size_t n, u8* dst, std::size_t cap, long search_depth) {
    if (cap < 16 + n + n / 8 + 2) return kTooSmall;
    std::memcpy(dst, "Yaz0", 4);
    dst[4] = u8(n >> 24); dst[5] = u8(n >> 16); dst[6] = u8(n >> 8); dst[7] = u8(n);
    std::memset(dst + 8, 0, 8);
    std::size_t out = 16, in = 0;
    const std::size_t depth = search_depth < 0 ? 0 : std::size_t(search_depth);
    bool next_flag = false; std::size_t next_count = 0; long next_pos = -1;
    u8 group[24]; std::size_t glen = 0; u8 mask = 0; int done = 0;
    while (in < n) {
        std::size_t count; long pos;
        if (next_flag) { next_flag = false; count = next_count; pos = next_pos; }
        else {
            rle_encode(src, n, in, depth, count, pos);
            if (count >= 3) {
                rle_encode(src, n, in + 1, depth, next_count, next_pos);
                if (next_count >= count + 2) { count = 1; pos = -1; next_flag = true; }
            }
        }
        if (count < 3) {
            group[glen++] = src[in++];
            mask |= u8(0x80 >> done);
        } else {
            const std::size_t dist = in - std::size_t(pos) - 1;
            if (count >= 0x12) {
                group[glen++] = u8((dist & 0xFF00) >> 8);
                group[glen++] = u8(dist & 0xFF);
                if (count > kMaxRun) count = kMaxRun;
                group[glen++] = u8(count - 0x12);
            } else {
                group[glen++] = u8(((count - 2) << 4) | ((dist >> 8) & 0x0F));
                group[glen++] = u8(dist & 0xFF);
            }
            in += count;
        }
        if (++done == 8) {
            if (out + 1 + glen > cap) return kTooSmall;
            dst[out++] = mask; std::memcpy(dst + out, group, glen); out += glen;
            mask = 0; done = 0; glen = 0;
        }
    }
    if (out + 1 + glen > cap) return kTooSmall;
    if (done > 0) { dst[out++] = mask; std::memcpy(dst + out, group, glen); out += glen; }
    else dst[out++] = 0;
    return long(out);
}

// ------------------------------------------------------------------------------------------------- GX textures ---
// Formats and palette formats as gclib.gx_enums numbers them.
namespace {
enum Fmt { I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA32 = 6, C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14 };
enum Pal { PAL_IA8 = 0, PAL_RGB565 = 1, PAL_RGB5A3 = 2 };

struct Rgba { u8 r, g, b, a; };
inline u8 s3(u32 v) { return u8((v << 5) | (v << 2) | (v >> 1)); }
inline u8 s4(u32 v) { return u8((v << 4) | v); }
inline u8 s5(u32 v) { return u8((v << 3) | (v >> 2)); }
inline u8 s6(u32 v) { return u8((v << 2) | (v >> 4)); }
inline Rgba rgb565(u32 c) { return {s5((c >> 11) & 0x1F), s6((c >> 5) & 0x3F), s5(c & 0x1F), 255}; }
inline Rgba rgb5a3(u32 c) {
    if ((c & 0x8000) == 0) return {s4((c >> 8) & 0xF), s4((c >> 4) & 0xF), s4(c & 0xF), s3((c >> 12) & 0x7)};
    return {s5((c >> 10) & 0x1F), s5((c >> 5) & 0x1F), s5(c & 0x1F), 255};
}
inline Rgba ia8(u32 c) { const u8 l = u8(c & 0xFF); return {l, l, l, u8(c >> 8)}; }

struct Layout { int bw, bh, bytes; };
bool layout(int fmt, Layout& l) {
    switch (fmt) {
        case I4: l = {8, 8, 32}; return true;      case I8: l = {8, 4, 32}; return true;
        case IA4: l = {8, 4, 32}; return true;     case IA8: l = {4, 4, 32}; return true;
        case RGB565: l = {4, 4, 32}; return true;  case RGB5A3: l = {4, 4, 32}; return true;
        case RGBA32: l = {4, 4, 64}; return true;  case C4: l = {8, 8, 32}; return true;
        case C8: l = {8, 4, 32}; return true;      case C14X2: l = {4, 4, 32}; return true;
        case CMPR: l = {8, 8, 32}; return true;
    }
    return false;
}
}  // namespace

// Decodes like gclib.texture_utils.decode_image into `out` (width*height RGBA8, pre-cleared to 0 by the caller,
// as Image.new("RGBA", size, (0,0,0,0)) is). Palette indexes past the palette (gclib's None) return kBadFormat so the
// caller keeps the Python behaviour for them.
NXA_EXPORT long nxa_gx_decode(int fmt, const u8* data, std::size_t n, const u8* pal, std::size_t pal_n, int pal_fmt,
                              int ncolors, int width, int height, u8* out) {
    Layout L;
    if (!layout(fmt, L) || width <= 0 || height <= 0) return kBadFormat;
    Rgba palette[1 << 14];
    const bool paletted = fmt == C4 || fmt == C8 || fmt == C14X2;
    int colors = 0;
    if (paletted) {
        if (ncolors < 0 || ncolors > (1 << 14) || pal_n < std::size_t(ncolors) * 2) return kTruncated;
        if (pal_fmt < 0 || pal_fmt > 2) return kBadFormat;
        for (int i = 0; i < ncolors; ++i) {
            const u16 c = be16(pal + 2 * i);
            palette[i] = pal_fmt == PAL_IA8 ? ia8(c) : pal_fmt == PAL_RGB565 ? rgb565(c) : rgb5a3(c);
        }
        colors = ncolors;
    }
    const int bx_count = (width + L.bw - 1) / L.bw, by_count = (height + L.bh - 1) / L.bh;
    if (std::size_t(bx_count) * by_count * L.bytes > n) return kTruncated;
    Rgba px[64];
    const u8* blk = data;
    for (int by = 0; by < by_count; ++by) {
        for (int bx = 0; bx < bx_count; ++bx, blk += L.bytes) {
            int count = L.bw * L.bh;
            switch (fmt) {
                case I4: for (int i = 0; i < 32; ++i) { const u8 b = blk[i]; const u8 h = s4(b >> 4), l = s4(b & 0xF); px[2 * i] = {h, h, h, h}; px[2 * i + 1] = {l, l, l, l}; } break;
                case I8: for (int i = 0; i < 32; ++i) { const u8 v = blk[i]; px[i] = {v, v, v, v}; } break;
                case IA4: for (int i = 0; i < 32; ++i) { const u8 v = s4(blk[i] & 0xF); px[i] = {v, v, v, s4(blk[i] >> 4)}; } break;
                case IA8: for (int i = 0; i < 16; ++i) px[i] = ia8(be16(blk + 2 * i)); break;
                case RGB565: for (int i = 0; i < 16; ++i) px[i] = rgb565(be16(blk + 2 * i)); break;
                case RGB5A3: for (int i = 0; i < 16; ++i) px[i] = rgb5a3(be16(blk + 2 * i)); break;
                case RGBA32: for (int i = 0; i < 16; ++i) px[i] = {blk[2 * i + 1], blk[2 * i + 32], blk[2 * i + 33], blk[2 * i]}; break;
                case C4:
                    for (int i = 0; i < 32; ++i) {
                        const int hi = blk[i] >> 4, lo = blk[i] & 0xF;
                        if (hi >= colors || lo >= colors) return kBadFormat;
                        px[2 * i] = palette[hi]; px[2 * i + 1] = palette[lo];
                    }
                    break;
                case C8: for (int i = 0; i < 32; ++i) { if (blk[i] >= colors) return kBadFormat; px[i] = palette[blk[i]]; } break;
                case C14X2: for (int i = 0; i < 16; ++i) { const int k = be16(blk + 2 * i) & 0x3FFF; if (k >= colors) return kBadFormat; px[i] = palette[k]; } break;
                case CMPR:
                    for (int sb = 0; sb < 4; ++sb) {
                        const u8* p = blk + sb * 8;
                        const u16 c0 = be16(p), c1 = be16(p + 2);
                        Rgba c[4] = {rgb565(c0), rgb565(c1), {}, {}};
                        if (c0 > c1) {
                            c[2] = {u8((2 * c[0].r + c[1].r) / 3), u8((2 * c[0].g + c[1].g) / 3), u8((2 * c[0].b + c[1].b) / 3), 255};
                            c[3] = {u8((c[0].r + 2 * c[1].r) / 3), u8((c[0].g + 2 * c[1].g) / 3), u8((c[0].b + 2 * c[1].b) / 3), 255};
                        } else {
                            c[2] = {u8(c[0].r / 2 + c[1].r / 2), u8(c[0].g / 2 + c[1].g / 2), u8(c[0].b / 2 + c[1].b / 2), 255};
                            c[3] = {0, 0, 0, 0};
                        }
                        const u32 idx = be32(p + 4);
                        const int sx = (sb % 2) * 4, sy = (sb / 2) * 4;
                        for (int i = 0; i < 16; ++i) px[sx + sy * 8 + (i / 4) * 8 + (i % 4)] = c[(idx >> ((15 - i) * 2)) & 3];
                    }
                    count = 64;
                    break;
            }
            for (int i = 0; i < count; ++i) {
                const int x = bx * L.bw + i % L.bw, y = by * L.bh + i / L.bw;
                if (x >= width || y >= height) continue;
                std::memcpy(out + (std::size_t(y) * width + x) * 4, &px[i], 4);
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------- DSP-ADPCM -------
// nw4r_audio.decode_adpcm: `coefs` 16 signed values, output `samples` int16 values.
NXA_EXPORT long nxa_adpcm_decode(const u8* data, std::size_t n, std::size_t start, std::size_t samples, const int* coefs,
                                 int yn1, int yn2, std::int16_t* out) {
    std::size_t pos = start, produced = 0;
    long h1 = yn1, h2 = yn2;
    while (produced < samples) {
        if (pos >= n) return kTruncated;
        const u8 header = data[pos];
        const long scale = 1L << (header & 15);
        const int pred = header >> 4;
        if (pred > 7) return kBadFormat;  // Python would read past the coefficient list
        const long c1 = coefs[pred * 2], c2 = coefs[pred * 2 + 1];
        for (std::size_t k = 1; k < 8 && produced < samples; ++k) {
            if (pos + k >= n) return kTruncated;
            const u8 b = data[pos + k];
            for (int half = 0; half < 2 && produced < samples; ++half) {
                const int nib = half == 0 ? b >> 4 : b & 15;
                const long sn = nib >= 8 ? nib - 16 : nib;
                long v = ((sn * scale) << 11) + 1024 + c1 * h1 + c2 * h2;
                v >>= 11;  // arithmetic shift, as Python's >> on negative ints (floor)
                v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
                h2 = h1; h1 = v;
                out[produced++] = std::int16_t(v);
            }
        }
        pos += 8;
    }
    return long(produced);
}
