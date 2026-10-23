// Cactus Galaxy Wrapper: la interfaz de la consola (RG35XX H, KNULLI) que convierte tu disco de Super Mario Galaxy
// (ISO, RVZ, WIA, WBFS, GCZ o CISO) en el game.pak del port. Dibuja a 640x480 en memoria (un desierto de noche bajo
// una galaxia, con un saguaro que crece con el avance) y lo muestra con SDL2; la conversion la hace nebula-pak
// (platform/knulli/pak), que corre como proceso aparte y cuenta su avance por la salida.
//
//   cactus-galaxy-wrapper [--smg <carpeta smg>] [--auto]      la interfaz
//   cactus-galaxy-wrapper --demo <carpeta>                     capturas PNG de cada pantalla (sin SDL)
//
// --auto (lo usa el lanzador del juego): si hay un solo disco valido lo convierte sin preguntar y, al terminar, sale
// para que arranque el juego. Salida: 0 si hay game.pak al final, 1 si no, 3 si no se pudo abrir la pantalla.
// GPL-3.0-or-later, como Nebula. Fuente 8x8: font8x8 de Daniel Hepper (dominio publico).
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CACTUS_NO_SDL
#include <SDL.h>
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

// font8x8 declares its tables as `char` with values above 127.
#define char unsigned char
#include "font8x8/font8x8_basic.h"
#include "font8x8/font8x8_ext_latin.h"
#undef char

namespace {

using u32 = uint32_t;
constexpr int W = 640, H = 480;

// ---------------------------------------------------------------------------------------------------------------
// Drawing: 0xRRGGBB pixels in memory.

struct Canvas {
  std::vector<u32> px = std::vector<u32>(W * H, 0);

  void blend(int x, int y, u32 c, float a) {
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0.0f) return;
    u32& d = px[y * W + x];
    if (a >= 1.0f) { d = c; return; }
    const int ia = int(a * 256.0f);
    const int r = int(d >> 16 & 255) + (((int(c >> 16 & 255) - int(d >> 16 & 255)) * ia) >> 8);
    const int g = int(d >> 8 & 255) + (((int(c >> 8 & 255) - int(d >> 8 & 255)) * ia) >> 8);
    const int b = int(d & 255) + (((int(c & 255) - int(d & 255)) * ia) >> 8);
    d = u32(r) << 16 | u32(g) << 8 | u32(b);
  }
  void add(int x, int y, float r, float g, float b) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    u32& d = px[y * W + x];
    const int nr = std::min(255, int(d >> 16 & 255) + int(r));
    const int ng = std::min(255, int(d >> 8 & 255) + int(g));
    const int nb = std::min(255, int(d & 255) + int(b));
    d = u32(nr) << 16 | u32(ng) << 8 | u32(nb);
  }
  void rect(int x, int y, int w, int h, u32 c, float a = 1.0f) {
    for (int j = std::max(0, y); j < std::min(H, y + h); ++j)
      for (int i = std::max(0, x); i < std::min(W, x + w); ++i) blend(i, j, c, a);
  }
  void rrect(int x, int y, int w, int h, int r, u32 c, float a = 1.0f) {
    for (int j = 0; j < h; ++j) {
      for (int i = 0; i < w; ++i) {
        const int dx = i < r ? r - i - 1 : (i >= w - r ? i - (w - r) : -1);
        const int dy = j < r ? r - j - 1 : (j >= h - r ? j - (h - r) : -1);
        float cover = 1.0f;
        if (dx >= 0 && dy >= 0) {
          const float d = std::sqrt(float(dx * dx + dy * dy)) - float(r) + 1.0f;
          cover = std::clamp(1.0f - d, 0.0f, 1.0f);
        }
        blend(x + i, y + j, c, a * cover);
      }
    }
  }
  void rrect_outline(int x, int y, int w, int h, int r, int t, u32 c, float a = 1.0f) {
    for (int j = 0; j < h; ++j) {
      for (int i = 0; i < w; ++i) {
        const float cx = std::clamp(float(i) + 0.5f, float(r), float(w - r));
        const float cy = std::clamp(float(j) + 0.5f, float(r), float(h - r));
        const float d = std::hypot(float(i) + 0.5f - cx, float(j) + 0.5f - cy);
        const float edge = float(r) - d;  // distance inside the rounded shape
        if (edge < 0.0f || edge > float(t) + 1.0f) continue;
        const float cover = std::clamp(edge + 0.5f, 0.0f, 1.0f) * std::clamp(float(t) + 0.5f - edge, 0.0f, 1.0f);
        if (r == 0 && i >= t && j >= t && i < w - t && j < h - t) continue;
        blend(x + i, y + j, c, a * cover);
      }
    }
  }
  void circle(float cx, float cy, float r, u32 c, float a = 1.0f) {
    for (int j = int(cy - r - 1); j <= int(cy + r + 1); ++j)
      for (int i = int(cx - r - 1); i <= int(cx + r + 1); ++i) {
        const float d = std::hypot(float(i) + 0.5f - cx, float(j) + 0.5f - cy);
        blend(i, j, c, a * std::clamp(r - d + 0.5f, 0.0f, 1.0f));
      }
  }
  void ellipse(float cx, float cy, float rx, float ry, u32 c, float a = 1.0f) {
    for (int j = int(cy - ry - 1); j <= int(cy + ry + 1); ++j)
      for (int i = int(cx - rx - 1); i <= int(cx + rx + 1); ++i) {
        const float u = (float(i) + 0.5f - cx) / rx, v = (float(j) + 0.5f - cy) / ry;
        const float d = (std::sqrt(u * u + v * v) - 1.0f) * std::min(rx, ry);
        blend(i, j, c, a * std::clamp(0.5f - d, 0.0f, 1.0f));
      }
  }
  void glow(float cx, float cy, float r, float red, float green, float blue) {
    for (int j = int(cy - r); j <= int(cy + r); ++j)
      for (int i = int(cx - r); i <= int(cx + r); ++i) {
        const float d2 = ((float(i) - cx) * (float(i) - cx) + (float(j) - cy) * (float(j) - cy)) / (r * r);
        if (d2 >= 1.0f) continue;
        const float k = (1.0f - d2) * (1.0f - d2);
        add(i, j, red * k, green * k, blue * k);
      }
  }
  void star(float cx, float cy, float r, u32 c, float a, float spin) {  // five points
    float xs[10], ys[10];
    for (int k = 0; k < 10; ++k) {
      const float ang = spin + float(k) * 3.14159265f / 5.0f - 3.14159265f / 2.0f;
      const float rr = k % 2 == 0 ? r : r * 0.45f;
      xs[k] = cx + rr * std::cos(ang);
      ys[k] = cy + rr * std::sin(ang);
    }
    for (int j = int(cy - r - 1); j <= int(cy + r + 1); ++j)
      for (int i = int(cx - r - 1); i <= int(cx + r + 1); ++i) {
        int inside = 0;
        for (int s = 0; s < 4; ++s) {  // 2x2 samples
          const float px_ = float(i) + 0.25f + 0.5f * float(s & 1), py_ = float(j) + 0.25f + 0.5f * float(s >> 1);
          bool in = false;
          for (int k = 0, l = 9; k < 10; l = k++) {
            if ((ys[k] > py_) != (ys[l] > py_) && px_ < (xs[l] - xs[k]) * (py_ - ys[k]) / (ys[l] - ys[k]) + xs[k]) in = !in;
          }
          inside += in;
        }
        blend(i, j, c, a * float(inside) / 4.0f);
      }
  }
};

u32 mix(u32 a, u32 b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  auto ch = [&](int s) { return u32(float(a >> s & 255) + (float(b >> s & 255) - float(a >> s & 255)) * t) << s; };
  return ch(16) | ch(8) | ch(0);
}

// ---------------------------------------------------------------------------------------------------------------
// Text: font8x8 (U+0000-007F, U+00A0-00FF) and a few glyphs of our own.

struct Glyph {
  u32 code;
  const char* rows[8];
};
const Glyph kExtra[] = {
    {0x2713, {"........", ".......#", "......##", ".....##.", "#...##..", "##.##...", ".###....", "..#....."}},  // check
    {0x2717, {"........", "##....##", ".##..##.", "..####..", "...##...", "..####..", ".##..##.", "##....##"}},  // cross
    {0x2192, {"........", "....#...", "....##..", "#######.", "#######.", "....##..", "....#...", "........"}},  // ->
    {0x2191, {"...##...", "..####..", ".######.", "...##...", "...##...", "...##...", "...##...", "........"}},  // up
    {0x2193, {"...##...", "...##...", "...##...", "...##...", ".######.", "..####..", "...##...", "........"}},  // down
    {0x2026, {"........", "........", "........", "........", "........", "........", "##.##.##", "##.##.##"}},  // ...
};

void glyph_bits(u32 code, unsigned char out[8]) {
  if (code < 128) { memcpy(out, font8x8_basic[code], 8); return; }
  if (code >= 0xA0 && code <= 0xFF) { memcpy(out, font8x8_ext_latin[code - 0xA0], 8); return; }
  for (const Glyph& g : kExtra) {
    if (g.code != code) continue;
    for (int r = 0; r < 8; ++r) {
      out[r] = 0;
      for (int c = 0; c < 8; ++c) if (g.rows[r][c] == '#') out[r] |= u32(1) << c;
    }
    return;
  }
  memcpy(out, font8x8_basic[int('?')], 8);
}

std::vector<u32> utf8(const std::string& s) {
  std::vector<u32> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    u32 code = c;
    int extra = 0;
    if (c >= 0xF0) { code = c & 7; extra = 3; }
    else if (c >= 0xE0) { code = c & 15; extra = 2; }
    else if (c >= 0xC0) { code = c & 31; extra = 1; }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) code = code << 6 | ((unsigned char)s[i] & 63);
    out.push_back(code);
  }
  return out;
}

int text_width(const std::string& s, int scale) { return int(utf8(s).size()) * 8 * scale; }

int text(Canvas& cv, int x, int y, const std::string& s, int scale, u32 color, float alpha = 1.0f, bool shadow = true) {
  const std::vector<u32> codes = utf8(s);
  for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
    const int ox = pass == 0 ? std::max(1, scale / 2) : 0;
    const u32 c = pass == 0 ? 0x0a0614 : color;
    const float a = pass == 0 ? alpha * 0.7f : alpha;
    int cx = x;
    for (u32 code : codes) {
      unsigned char bits[8];
      glyph_bits(code, bits);
      for (int r = 0; r < 8; ++r)
        for (int col = 0; col < 8; ++col)
          if (bits[r] >> col & 1) cv.rect(cx + col * scale + ox, y + r * scale + ox, scale, scale, c, a);
      cx += 8 * scale;
    }
  }
  return int(codes.size()) * 8 * scale;
}

void text_center(Canvas& cv, int cx, int y, const std::string& s, int scale, u32 color, float alpha = 1.0f) {
  text(cv, cx - text_width(s, scale) / 2, y, s, scale, color, alpha);
}

// At most `chars` characters, with an ellipsis.
std::string fit(const std::string& s, size_t chars) {
  const std::vector<u32> codes = utf8(s);
  if (codes.size() <= chars) return s;
  std::string out;
  size_t count = 0;
  for (size_t i = 0; i < s.size() && count < chars - 1;) {
    const unsigned char c = (unsigned char)s[i];
    const size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    out += s.substr(i, len);
    i += len;
    ++count;
  }
  return out + "\xE2\x80\xA6";
}

std::vector<std::string> wrap(const std::string& s, size_t chars) {
  std::vector<std::string> lines;
  std::string line, word;
  auto flush_word = [&] {
    if (word.empty()) return;
    if (!line.empty() && utf8(line).size() + 1 + utf8(word).size() > chars) { lines.push_back(line); line.clear(); }
    while (utf8(word).size() > chars) { lines.push_back(word.substr(0, chars)); word = word.substr(chars); }
    line += (line.empty() ? "" : " ") + word;
    word.clear();
  };
  for (char c : s) {
    if (c == ' ' || c == '\n') { flush_word(); if (c == '\n') { lines.push_back(line); line.clear(); } }
    else word += c;
  }
  flush_word();
  if (!line.empty()) lines.push_back(line);
  return lines;
}

// ---------------------------------------------------------------------------------------------------------------
// The scene: night desert, a galaxy, a ringed planet, twinkling stars and the saguaro.

constexpr u32 kGreen = 0x8ef0a0, kGold = 0xffd36e, kPink = 0xff7fb8, kRed = 0xff6b5e, kWhite = 0xf4eefc,
              kMuted = 0xb9a9d6, kPanel = 0x140c2a;

float dune_back(int x) { return 352.0f + 16.0f * std::sin(float(x) * 0.011f + 1.0f) + 9.0f * std::sin(float(x) * 0.027f); }
float dune_front(int x) { return 404.0f + 12.0f * std::sin(float(x) * 0.008f + 2.0f) + 6.0f * std::sin(float(x) * 0.031f); }

void panel(Canvas& cv);
constexpr int kPanelX = 244, kPanelY = 14, kPanelW = 384, kPanelH = 452;

struct Star {
  float x, y, size, phase, speed;
  u32 color;
};

class Scene {
 public:
  Scene() {
    unsigned seed = 7;
    auto rnd = [&] { seed = seed * 1103515245u + 12345u; return float(seed >> 8 & 0xFFFF) / 65535.0f; };
    // Sky.
    for (int y = 0; y < H; ++y) {
      const float t = float(y) / 360.0f;
      u32 c = t < 0.55f ? mix(0x07061f, 0x24124a, t / 0.55f) : mix(0x24124a, 0x9c3f5a, (t - 0.55f) / 0.35f);
      if (t > 0.9f) c = mix(0x9c3f5a, 0xe9824e, (t - 0.9f) / 0.1f);
      for (int x = 0; x < W; ++x) bg_.px[y * W + x] = c;
    }
    // The galaxy: two spiral arms of soft points around a warm core, tilted.
    const float gx = 150.0f, gy = 92.0f, tilt = -0.35f;
    for (int i = 0; i < 2600; ++i) {
      const float t = rnd();
      const float arm = float(i % 2) * 3.14159265f;
      const float ang = t * 4.4f * 3.14159265f + arm + (rnd() - 0.5f) * 0.55f;
      const float r = 5.0f + t * 88.0f + (rnd() - 0.5f) * 9.0f;
      float x = r * std::cos(ang), y = r * std::sin(ang) * 0.42f;
      const float xr = x * std::cos(tilt) - y * std::sin(tilt), yr = x * std::sin(tilt) + y * std::cos(tilt);
      const float warm = std::max(0.0f, 1.0f - t * 2.2f);
      const float k = 22.0f * (1.0f - t * 0.6f);
      bg_.glow(gx + xr, gy + yr, 2.0f + rnd() * 2.5f, k * (0.55f + warm), k * (0.45f + 0.4f * warm), k * (1.0f - 0.3f * warm));
    }
    bg_.glow(gx, gy, 26.0f, 120.0f, 95.0f, 70.0f);
    bg_.glow(gx, gy, 9.0f, 160.0f, 150.0f, 120.0f);
    // A ringed planet.
    bg_.ellipse(52.0f, 196.0f, 30.0f, 7.0f, 0xd9a16b, 0.5f);
    bg_.circle(52.0f, 196.0f, 15.0f, 0x6c4fa8);
    for (int j = 181; j < 211; ++j)  // shading
      for (int i = 37; i < 67; ++i) {
        const float d = std::hypot(float(i) - 46.0f, float(j) - 190.0f);
        if (std::hypot(float(i) - 52.0f, float(j) - 196.0f) < 15.0f) bg_.blend(i, j, 0xa58ad8, std::clamp(0.5f - d / 30.0f, 0.0f, 0.5f));
      }
    for (int i = 22; i < 83; ++i) {  // the ring's front half
      const float u = (float(i) - 52.0f) / 30.0f;
      const float y = 196.0f + 7.0f * std::sqrt(std::max(0.0f, 1.0f - u * u));
      bg_.blend(i, int(y), 0xf0c48c, 0.85f);
      bg_.blend(i, int(y) - 1, 0xd9a16b, 0.5f);
    }
    // Stars (only where the sky shows).
    for (int i = 0; i < 150; ++i) {
      Star s{rnd() * W, rnd() * 340.0f, 0.6f + rnd() * 1.4f, rnd() * 6.28f, 0.8f + rnd() * 2.5f,
             mix(0xffffff, rnd() < 0.5f ? 0xffd9a0 : 0xa8c8ff, rnd())};
      if (s.y < dune_back(int(s.x)) - 8.0f && std::hypot(s.x - gx, (s.y - gy) * 2.0f) > 60.0f) stars_.push_back(s);
    }
    // Dunes, far cacti, near ground.
    for (int x = 0; x < W; ++x) {
      const int yb = int(dune_back(x)), yf = int(dune_front(x));
      for (int y = yb; y < H; ++y) bg_.px[y * W + x] = mix(0x4a2850, 0x2d1838, float(y - yb) / 60.0f);
      for (int y = yf; y < H; ++y) bg_.px[y * W + x] = mix(0x2a1531, 0x170b1d, float(y - yf) / 70.0f);
      bg_.blend(x, yb, 0x8a4f6e, 0.7f);
      bg_.blend(x, yf, 0x6b3a5a, 0.8f);
    }
    far_cactus(214, 0.55f);
    far_cactus(18, 0.42f);
    far_cactus(600, 0.5f);
    // The panel's frame and title never change: drawn once into the background (what moves stays left of it).
    panel(bg_);
    stars_.erase(std::remove_if(stars_.begin(), stars_.end(), [](const Star& st) {
      return st.x > kPanelX - 6 && st.y > kPanelY - 6;
    }), stars_.end());
  }

  void draw(Canvas& cv, float time) const {
    cv.px = bg_.px;
    for (const Star& s : stars_) {
      const float tw = 0.55f + 0.45f * std::sin(time * s.speed + s.phase);
      if (s.size > 1.6f) {
        cv.glow(s.x, s.y, 3.0f * s.size, 70.0f * tw, 70.0f * tw, 80.0f * tw);
      }
      cv.circle(s.x, s.y, s.size * 0.6f, s.color, tw);
    }
    // A shooting star every few seconds.
    const float cycle = std::fmod(time, 7.0f);
    if (cycle < 0.9f) {
      const float t = cycle / 0.9f;
      const float x = 30.0f + t * 220.0f, y = 40.0f + t * 70.0f;
      for (int k = 0; k < 26; ++k) {
        const float f = float(k) / 26.0f;
        cv.circle(x - f * 40.0f, y - f * 12.7f, 1.4f * (1.0f - f), 0xfff2d0, (1.0f - f) * (1.0f - t * 0.6f));
      }
    }
  }

 private:
  void far_cactus(int x, float s) {
    const int ground = int(dune_back(x)) + 2;
    const u32 c = 0x2a1636;
    const int h = int(70 * s), w = std::max(4, int(12 * s));
    bg_.rrect(x - w / 2, ground - h, w, h + 2, w / 2, c);
    bg_.rrect(x - w / 2 - int(14 * s), ground - int(h * 0.6f), int(16 * s), w - 1, 2, c);
    bg_.rrect(x - w / 2 - int(14 * s), ground - int(h * 0.85f), w - 2, int(h * 0.3f), (w - 2) / 2, c);
    bg_.rrect(x + w / 2 - 1, ground - int(h * 0.45f), int(13 * s), w - 2, 2, c);
    bg_.rrect(x + w / 2 + int(9 * s), ground - int(h * 0.68f), w - 2, int(h * 0.25f), (w - 2) / 2, c);
  }
  Canvas bg_;
  std::vector<Star> stars_;
};

// The saguaro: grows with `growth` (0..1), flowers with `bloom` (0..1), browns and droops with `wilt` (0..1).
void draw_cactus(Canvas& cv, float base_x, float growth, float bloom, float wilt, float time) {
  const float ground = dune_front(int(base_x)) + 6.0f;
  const float trunk_h = 70.0f + 185.0f * std::clamp(growth, 0.0f, 1.0f);
  const float w = 34.0f;
  const float top = ground - trunk_h;
  auto shade = [&](float u) {  // u: -1 left edge .. 1 right edge
    u32 c = u < -0.35f ? 0x67c872 : (u < 0.35f ? 0x43a152 : 0x2d7339);
    if (std::fabs(std::fmod(std::fabs(u) * 3.0f + 0.15f, 1.0f) - 0.5f) < 0.09f) c = mix(c, 0x1f5a2b, 0.7f);  // ribs
    return mix(c, mix(c, 0x7d6a3a, 0.8f), wilt);
  };
  auto column = [&](float cx, float ytop, float ybottom, float width, float lean) {
    // A vertical capsule with ribs and spines; `lean` tilts its top (the wilted cactus droops).
    const float r = width / 2.0f;
    for (int y = int(ytop - r); y <= int(ybottom); ++y) {
      const float along = std::clamp((ybottom - float(y)) / std::max(1.0f, ybottom - ytop + r), 0.0f, 1.0f);
      const float off = lean * along * along;
      float half = r;
      if (float(y) < ytop) half = std::sqrt(std::max(0.0f, r * r - (ytop - float(y)) * (ytop - float(y))));
      for (int x = int(cx + off - half - 1); x <= int(cx + off + half + 1); ++x) {
        const float u = (float(x) + 0.5f - cx - off) / std::max(0.5f, half);
        const float cover = std::clamp(half - std::fabs(float(x) + 0.5f - cx - off) + 0.5f, 0.0f, 1.0f);
        if (cover > 0.0f) cv.blend(x, y, shade(u), cover);
      }
      if (int(y) % 9 == 0 && float(y) > ytop - r * 0.5f) {  // spines on the ribs
        for (float rib : {-0.62f, 0.0f, 0.62f}) {
          const float sx = cx + off + rib * half;
          cv.blend(int(sx), y, mix(0xeaf7cc, 0x9a8a60, wilt), 0.95f);
          cv.blend(int(sx) + (rib < 0 ? -1 : 1), y - 1, mix(0xeaf7cc, 0x9a8a60, wilt), 0.6f);
        }
      }
    }
  };
  auto elbow = [&](float x0, float x1, float y, float thick) {
    const float lo = std::min(x0, x1), hi = std::max(x0, x1);
    for (int yy = int(y - thick / 2); yy <= int(y + thick / 2); ++yy) {
      const float v = (float(yy) - y) / (thick / 2);
      const u32 c = shade(v * 0.9f);
      for (int x = int(lo); x <= int(hi); ++x) cv.blend(x, yy, c, 1.0f);
    }
  };
  cv.ellipse(base_x + 6.0f, ground + 2.0f, 40.0f, 6.0f, 0x0a0410, 0.55f);
  const float lean = wilt * 16.0f;
  // Arms first (the trunk covers their joints).
  const float a1 = std::clamp((growth - 0.28f) / 0.35f, 0.0f, 1.0f);
  if (a1 > 0.0f) {
    const float y = ground - trunk_h * 0.42f, x = base_x - w / 2.0f - 20.0f;
    elbow(x, base_x, y, 22.0f);
    column(x, y - 18.0f - 56.0f * a1, y + 4.0f, 22.0f, -lean * 0.6f);
  }
  const float a2 = std::clamp((growth - 0.55f) / 0.35f, 0.0f, 1.0f);
  if (a2 > 0.0f) {
    const float y = ground - trunk_h * 0.62f, x = base_x + w / 2.0f + 17.0f;
    elbow(base_x, x, y, 20.0f);
    column(x, y - 14.0f - 46.0f * a2, y + 4.0f, 20.0f, lean * 0.6f);
  }
  column(base_x, top, ground, w, lean);
  if (bloom > 0.0f) {  // flowers: the crown and the arm tips
    auto flower = [&](float fx, float fy, float s) {
      for (int k = 0; k < 6; ++k) {
        const float ang = float(k) * 1.047f + time * 0.4f;
        cv.circle(fx + std::cos(ang) * 6.0f * s, fy + std::sin(ang) * 6.0f * s, 5.0f * s, kPink, 0.95f);
      }
      cv.circle(fx, fy, 4.0f * s, 0xffe066);
      cv.glow(fx, fy, 22.0f * s, 60.0f * s, 30.0f * s, 45.0f * s);
    };
    flower(base_x, top - w / 2.0f + 2.0f, bloom);
    if (a1 > 0.0f) flower(base_x - w / 2.0f - 20.0f, ground - trunk_h * 0.42f - 18.0f - 56.0f * a1 - 9.0f, bloom * 0.7f);
    if (a2 > 0.0f) flower(base_x + w / 2.0f + 17.0f, ground - trunk_h * 0.62f - 14.0f - 46.0f * a2 - 8.0f, bloom * 0.6f);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The panel and its pieces.

constexpr int kLeft = kPanelX + 16;
constexpr int kCenter = kPanelX + kPanelW / 2;

void panel(Canvas& cv) {
  cv.rrect(kPanelX + 4, kPanelY + 6, kPanelW, kPanelH, 16, 0x05020c, 0.45f);
  cv.rrect(kPanelX, kPanelY, kPanelW, kPanelH, 16, kPanel, 0.84f);
  cv.rrect_outline(kPanelX, kPanelY, kPanelW, kPanelH, 16, 2, 0x5fc46a, 0.75f);
  text_center(cv, kCenter, kPanelY + 18, "CACTUS GALAXY", 3, kGreen);
  text_center(cv, kCenter, kPanelY + 46, "WRAPPER", 3, kGold);
  // A spiny divider.
  for (int x = kLeft + 10; x < kPanelX + kPanelW - 26; ++x) cv.blend(x, kPanelY + 80, 0x43a152, 0.9f);
  for (int x = kLeft + 14; x < kPanelX + kPanelW - 26; x += 12) {
    cv.blend(x, kPanelY + 79, 0xeaf7cc, 0.9f);
    cv.blend(x + 6, kPanelY + 81, 0xeaf7cc, 0.9f);
  }
  text_center(cv, kCenter, kPanelY + 90, "ISO · RVZ \xE2\x86\x92 game.pak", 2, kMuted);
}

void button(Canvas& cv, int x, int y, const char* label, u32 color) {
  cv.circle(float(x) + 11.0f, float(y) + 8.0f, 11.5f, 0x05020c, 0.5f);
  cv.circle(float(x) + 10.0f, float(y) + 7.0f, 11.0f, color);
  text(cv, x + 10 - text_width(label, 2) / 2, y, label, 2, 0x140c2a, 1.0f, false);
}

// "(A) Convertir   (B) Salir" centered in a row.
void hints(Canvas& cv, int y, const std::vector<std::pair<std::string, std::string>>& items) {
  int total = 0;
  for (const auto& [key, label] : items) total += 22 + 6 + text_width(label, 2) + 18;
  total -= 18;
  int x = kCenter - total / 2;
  for (const auto& [key, label] : items) {
    const u32 color = key == "A" ? 0xff6b6b : key == "B" ? 0xffcf5a : key == "X" ? 0x6fb8ff : 0xc9c0dc;
    button(cv, x, y, key.c_str(), color);
    x += 28;
    x += text(cv, x, y, label, 2, kWhite) + 18;
  }
}

void progress_bar(Canvas& cv, int x, int y, int w, int h, float p, float time) {
  cv.rrect(x, y, w, h, h / 2, 0x0b0618, 0.9f);
  cv.rrect_outline(x, y, w, h, h / 2, 2, 0x3a2a5c, 1.0f);
  const int fill = int(float(w - 6) * std::clamp(p, 0.0f, 1.0f));
  if (fill > 0) {
    const int fx = x + 3, fy = y + 3, fh = h - 6;
    cv.rrect(fx, fy, std::max(fh, fill), fh, fh / 2, 0x43a152);
    for (int i = 0; i < std::max(fh, fill); ++i) {  // a lying cactus: light top, dark bottom, ribs, spines
      cv.blend(fx + i, fy + 2, 0x7fd88a, 0.8f);
      cv.blend(fx + i, fy + 3, 0x67c872, 0.6f);
      cv.blend(fx + i, fy + fh - 3, 0x2d7339, 0.9f);
      cv.blend(fx + i, fy + fh / 2, 0x2f7d3e, 0.55f);
      if (i % 10 == 4 && i > fh / 2 && i < fill - fh / 2) {
        cv.blend(fx + i, fy - 1, 0xeaf7cc, 0.9f);
        cv.blend(fx + i, fy + fh, 0xeaf7cc, 0.9f);
        cv.blend(fx + i + 1, fy + fh / 2 - 4, 0xeaf7cc, 0.7f);
      }
    }
    // A little star riding the tip.
    cv.star(float(fx + std::max(fh, fill)) - float(fh) / 2.0f, float(fy + fh / 2), float(fh) * 0.62f, 0xffe066, 1.0f, time * 2.0f);
  }
}

std::string mib_text(double mib) {
  char b[64];
  if (mib >= 1024.0) snprintf(b, sizeof(b), "%.1f GB", mib / 1024.0);
  else snprintf(b, sizeof(b), "%.0f MB", mib);
  std::string s = b;
  std::replace(s.begin(), s.end(), '.', ',');
  return s;
}

std::string duration_text(double seconds) {
  char b[64];
  const long s = long(seconds + 0.5);
  if (s >= 3600) snprintf(b, sizeof(b), "%ld h %02ld min", s / 3600, s / 60 % 60);
  else if (s >= 60) snprintf(b, sizeof(b), "%ld min %02ld s", s / 60, s % 60);
  else snprintf(b, sizeof(b), "%ld s", s);
  return b;
}

// ---------------------------------------------------------------------------------------------------------------
// Disc images and the conversion (nebula-pak as a child process).

struct Image {
  std::string path, name, format, id, title;
  double mib = 0;
  int status = -1;  // -1 checking, 0 Super Mario Galaxy USA, 1 another game, 3 not a disc
};

std::string lower(std::string s) {
  for (char& c : s) c = char(tolower((unsigned char)c));
  return s;
}

bool is_image(const std::string& name) {
  const std::string l = lower(name);
  for (const char* ext : {".iso", ".rvz", ".wia", ".wbfs", ".gcz", ".ciso"}) {
    const size_t n = strlen(ext);
    if (l.size() > n && l.compare(l.size() - n, n, ext) == 0) return true;
  }
  return false;
}

std::vector<Image> find_images(const std::vector<std::string>& dirs) {
  std::vector<Image> out;
  std::vector<std::string> seen;
  for (const std::string& dir : dirs) {
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) continue;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.' || !is_image(e->d_name)) continue;
      const std::string path = dir + "/" + e->d_name;
      char real[PATH_MAX];
      const std::string key = realpath(path.c_str(), real) ? real : path;
      if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
      struct stat st;
      if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      seen.push_back(key);
      Image im;
      im.path = path;
      im.name = e->d_name;
      im.mib = double(st.st_size) / 1048576.0;
      out.push_back(im);
    }
    closedir(d);
  }
  // .wbf1 halves of split images are not separate discs; Galaxy-looking names first.
  std::stable_sort(out.begin(), out.end(), [](const Image& a, const Image& b) {
    auto score = [](const Image& i) { const std::string l = lower(i.name); return l.find("galaxy") != std::string::npos || l.find("rmge01") != std::string::npos || l.find("smg") != std::string::npos; };
    return score(a) > score(b);
  });
  return out;
}

// Runs `argv`, calling `line` for each line (or \r-terminated update) it prints; returns its exit status.
pid_t spawn(const std::vector<std::string>& argv, int* out_fd) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  const pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execv(args[0], args.data());
    _exit(127);
  }
  close(fds[1]);
  if (pid < 0) { close(fds[0]); return -1; }
  *out_fd = fds[0];
  return pid;
}

template <typename F>
int read_lines(int fd, pid_t pid, F&& line) {
  std::string pending;
  char buf[4096];
  while (true) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == '\n' || buf[i] == '\r') { if (!pending.empty()) line(pending); pending.clear(); }
      else pending += buf[i];
    }
  }
  if (!pending.empty()) line(pending);
  close(fd);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

struct Job {
  std::mutex mutex;
  std::thread thread;
  pid_t pid = -1;
  std::atomic<bool> finished{false};
  int exit_code = -1;
  double percent = 0, done_mib = 0, total_mib = 0;
  std::string error, last_line;
  double started = 0;
};

double now_seconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

// ---------------------------------------------------------------------------------------------------------------
// The app.

enum class Screen { Menu, Converting, Done, Failed };

struct App {
  std::string smg, gamedata, tool, output;
  bool auto_mode = false, delete_after = false, cancelling = false, deleted = false;
  std::vector<Image> images;
  std::mutex images_mutex;
  std::thread identify_thread;
  std::atomic<bool> identified{false};
  int selected = 0, scroll = 0;
  Screen screen = Screen::Menu;
  std::unique_ptr<Job> job;
  std::string error, converted_name;
  double done_at = 0, took = 0, speed = 0, speed_mib = 0, speed_at = 0;
  double free_mib = -1;
  bool have_pak = false;
  bool quit = false;
  int result = 1;

  void refresh_disk() {
    struct statvfs fs;
    free_mib = statvfs(gamedata.c_str(), &fs) == 0 ? double(fs.f_bavail) * double(fs.f_frsize) / 1048576.0 : -1;
    struct stat st;
    have_pak = stat(output.c_str(), &st) == 0 && st.st_size > 0;
  }

  void start_identify() {
    identify_thread = std::thread([this] {
      for (size_t i = 0;; ++i) {
        std::string path;
        {
          std::lock_guard<std::mutex> lock(images_mutex);
          if (i >= images.size()) break;
          path = images[i].path;
        }
        int fd = -1;
        std::string out;
        const pid_t pid = spawn({tool, "--identify", path}, &fd);
        int code = 3;
        if (pid > 0) code = read_lines(fd, pid, [&](const std::string& l) { if (out.empty() && l.find('\t') != std::string::npos) out = l; });
        std::lock_guard<std::mutex> lock(images_mutex);
        Image& im = images[i];
        im.status = code == 0 ? 0 : (code == 1 && !out.empty() ? 1 : 3);
        if (!out.empty()) {
          std::vector<std::string> f;
          size_t at = 0;
          while (true) { const size_t t = out.find('\t', at); f.push_back(out.substr(at, t - at)); if (t == std::string::npos) break; at = t + 1; }
          if (f.size() >= 4) { im.id = f[0]; im.format = f[1]; im.title = f[3]; }
        }
      }
      // Valid discs first.
      std::lock_guard<std::mutex> lock(images_mutex);
      std::stable_sort(images.begin(), images.end(), [](const Image& a, const Image& b) { return (a.status == 0) > (b.status == 0); });
      identified = true;
    });
  }

  void start_conversion(const Image& im) {
    job = std::make_unique<Job>();
    job->started = now_seconds();
    converted_name = im.name;
    speed = 0; speed_mib = 0; speed_at = job->started;
    cancelling = false;
    Job* j = job.get();
    int fd = -1;
    j->pid = spawn({tool, im.path, output}, &fd);
    if (j->pid < 0) { error = "no se pudo ejecutar nebula-pak"; screen = Screen::Failed; return; }
    screen = Screen::Converting;
    printf("[cactus] convirtiendo %s -> %s\n", im.path.c_str(), output.c_str());
    fflush(stdout);
    const std::string image_path = im.path;
    const bool remove_image = delete_after;
    j->thread = std::thread([this, j, fd, image_path, remove_image] {
      const int code = read_lines(fd, j->pid, [j](const std::string& l) {
        printf("%s\n", l.c_str());
        fflush(stdout);
        std::lock_guard<std::mutex> lock(j->mutex);
        j->last_line = l;
        unsigned percent = 0;
        unsigned long long done = 0, total = 0;
        if (sscanf(l.c_str(), " %u%% (%llu / %llu MiB)", &percent, &done, &total) == 3) {
          j->percent = percent; j->done_mib = double(done); j->total_mib = double(total);
        } else if (l.rfind("Error: ", 0) == 0) {
          j->error = l.substr(7);
        }
      });
      std::lock_guard<std::mutex> lock(j->mutex);
      j->exit_code = code;
      if (code == 0 && remove_image) deleted = unlink(image_path.c_str()) == 0;
      j->finished = true;
    });
  }

  void finish_job() {
    if (!job || !job->finished) return;
    job->thread.join();
    took = now_seconds() - job->started;
    refresh_disk();
    if (job->exit_code == 0 && have_pak) {
      screen = Screen::Done;
      result = 0;
      done_at = now_seconds();
    } else {
      error = job->error.empty() ? (cancelling ? "conversion cancelada" : job->last_line) : job->error;
      if (error.empty()) error = "nebula-pak termino con codigo " + std::to_string(job->exit_code);
      screen = Screen::Failed;
    }
    job.reset();
  }

  // Buttons: confirm (A/START), back (B/SELECT), toggle (X), up, down.
  enum Key { kConfirm, kBack, kToggle, kUp, kDown };
  void press(Key key) {
    switch (screen) {
      case Screen::Menu: {
        std::lock_guard<std::mutex> lock(images_mutex);
        if (key == kUp && selected > 0) --selected;
        if (key == kDown && selected + 1 < int(images.size())) ++selected;
        if (key == kToggle) delete_after = !delete_after;
        if (key == kBack) quit = true;
        if (key == kConfirm && selected < int(images.size()) && images[selected].status == 0) {
          const Image im = images[selected];
          images_mutex.unlock();
          start_conversion(im);
          images_mutex.lock();
        }
        break;
      }
      case Screen::Converting:
        if (key == kBack && job && !cancelling) { cancelling = true; kill(job->pid, SIGTERM); }
        break;
      case Screen::Done:
        if (key == kConfirm || key == kBack) quit = true;
        break;
      case Screen::Failed:
        if (key == kConfirm) { screen = Screen::Menu; refresh_disk(); }
        if (key == kBack) quit = true;
        break;
    }
  }

  void tick() {
    finish_job();
    if (auto_mode && screen == Screen::Menu && identified && !job && error.empty()) {
      std::lock_guard<std::mutex> lock(images_mutex);
      int valid = 0, which = -1;
      for (int i = 0; i < int(images.size()); ++i) if (images[i].status == 0) { ++valid; which = i; }
      auto_mode = false;  // only once
      if (valid == 1) {
        const Image im = images[which];
        images_mutex.unlock();
        start_conversion(im);
        images_mutex.lock();
      }
    }
    if (screen == Screen::Done && result == 0 && done_at > 0 && auto_quit && now_seconds() - done_at > 6.0) quit = true;
  }
  bool auto_quit = false;

  // ------------------------------------------------------------------------------------------------------------
  void draw(Canvas& cv, const Scene& scene, float time) {
    scene.draw(cv, time);
    float growth = 0.08f, bloom = 0.0f, wilt = 0.0f;
    if (screen == Screen::Converting && job) {
      std::lock_guard<std::mutex> lock(job->mutex);
      growth = 0.08f + 0.92f * float(job->percent) / 100.0f;
    }
    if (screen == Screen::Done) { growth = 1.0f; bloom = std::clamp(float(now_seconds() - done_at) * 1.5f, 0.0f, 1.0f); }
    if (screen == Screen::Failed) { growth = 0.45f; wilt = 1.0f; }
    if (demo_growth >= 0) { growth = demo_growth; bloom = demo_bloom; wilt = demo_wilt; }
    draw_cactus(cv, 120.0f, growth, bloom, wilt, time);
    if (screen == Screen::Converting) {  // a star circling the cactus
      const float ang = time * 1.6f;
      const float top = dune_front(120) + 6.0f - (70.0f + 185.0f * growth);
      cv.star(120.0f + std::cos(ang) * 46.0f, top + 10.0f + std::sin(ang) * 14.0f, 7.0f, 0xffe066, 0.95f, time * 3.0f);
    }
    switch (screen) {
      case Screen::Menu: draw_menu(cv); break;
      case Screen::Converting: draw_converting(cv, time); break;
      case Screen::Done: draw_done(cv); break;
      case Screen::Failed: draw_failed(cv); break;
    }
  }

  void draw_menu(Canvas& cv) {
    std::lock_guard<std::mutex> lock(images_mutex);
    int y = 134;
    if (images.empty()) {
      text_center(cv, kCenter, y + 6, "No encontré ningún disco", 2, kGold);
      int ly = y + 40;
      for (const std::string& l : wrap("Pon tu ISO o RVZ de Super Mario Galaxy (USA) en roms/ports/smg/gamedata o en roms/wii y vuelve a abrir.", 22)) {
        text(cv, kLeft + 4, ly, l, 2, kWhite);
        ly += 22;
      }
    } else {
      text(cv, kLeft, y, "Elige tu disco:", 2, kGold);
      y += 26;
      const int rows = 4;
      if (selected < scroll) scroll = selected;
      if (selected >= scroll + rows) scroll = selected - rows + 1;
      for (int i = scroll; i < std::min(int(images.size()), scroll + rows); ++i) {
        const Image& im = images[i];
        const bool sel = i == selected;
        if (sel) {
          cv.rrect(kLeft - 6, y - 4, kPanelW - 20, 42, 8, 0x2f7d3e, 0.55f);
          cv.rrect_outline(kLeft - 6, y - 4, kPanelW - 20, 42, 8, 2, 0x7fd88a, 0.9f);
        }
        const u32 name_color = im.status == 0 ? kWhite : (im.status == -1 ? kMuted : 0x8f82a8);
        text(cv, kLeft + 2, y, fit(im.name, 22), 2, name_color);
        std::string detail;
        u32 dc = kMuted;
        const std::string size = mib_text(im.mib);
        if (im.status == -1) detail = size + " · comprobando\xE2\x80\xA6";
        else if (im.status == 0) { detail = im.format + " · " + size + " · \xE2\x9C\x93 USA"; dc = kGreen; }
        else if (im.status == 1) { detail = "\xE2\x9C\x97 otro juego (" + im.id + ")"; dc = 0xd98a8a; }
        else { detail = "\xE2\x9C\x97 no es un disco de Wii"; dc = 0xd98a8a; }
        text(cv, kLeft + 2, y + 18, fit(detail, 22), 2, dc);
        y += 46;
      }
      if (int(images.size()) > rows) {  // scroll arrows
        if (scroll > 0) text(cv, kPanelX + kPanelW - 30, 134, "\xE2\x86\x91", 2, kGold);
        if (scroll + rows < int(images.size())) text(cv, kPanelX + kPanelW - 30, 340, "\xE2\x86\x93", 2, kGold);
      }
    }
    // Where it goes and how much room there is.
    const int sy = 350;
    for (int x = kLeft + 10; x < kPanelX + kPanelW - 26; x += 4) cv.blend(x, sy - 8, 0x43a152, 0.6f);
    if (have_pak) text(cv, kLeft, sy, fit("Ya hay game.pak: se cambia", 22), 2, kGold);
    else text(cv, kLeft, sy, "Destino: gamedata/", 2, kMuted);
    if (free_mib >= 0) {
      const bool low = free_mib < 3400.0;
      text(cv, kLeft, sy + 20, fit("Libre: " + mib_text(free_mib) + (low ? " (poco)" : ""), 22), 2, low ? kRed : kMuted);
    }
    const bool can = selected < int(images.size()) && images[selected].status == 0;
    if (can) hints(cv, 406, {{"A", "Convertir"}, {"B", "Salir"}});
    else hints(cv, 406, {{"B", "Salir"}});
    hints(cv, 436, {{"X", std::string("Borrar disco: ") + (delete_after ? "Sí" : "No")}});
  }

  void draw_converting(Canvas& cv, float time) {
    double percent = 0, done = 0, total = 0;
    if (job) {
      std::lock_guard<std::mutex> lock(job->mutex);
      percent = job->percent; done = job->done_mib; total = job->total_mib;
    }
    if (demo_percent >= 0) { percent = demo_percent; done = demo_done; total = demo_total; speed = demo_speed; }
    const double t = now_seconds();
    if (demo_percent < 0 && t - speed_at > 2.0) {  // MiB/s over the last couple of seconds, smoothed
      const double s = (done - speed_mib) / (t - speed_at);
      speed = speed <= 0 ? s : speed * 0.6 + s * 0.4;
      speed_mib = done; speed_at = t;
    }
    text_center(cv, kCenter, 134, cancelling ? "Cancelando\xE2\x80\xA6" : "Convirtiendo", 2, kGold);
    text_center(cv, kCenter, 156, fit(converted_name, 22), 2, kMuted);
    char big[16];
    snprintf(big, sizeof(big), "%d%%", int(percent));
    text_center(cv, kCenter, 188, big, 6, kWhite);
    progress_bar(cv, kLeft + 4, 252, kPanelW - 40, 32, float(percent / 100.0), time);
    char line[96];
    if (total > 0) {
      snprintf(line, sizeof(line), "%.0f / %.0f MB", done, total);
      text_center(cv, kCenter, 300, line, 2, kWhite);
    } else {
      text_center(cv, kCenter, 300, "Leyendo el disco\xE2\x80\xA6", 2, kWhite);
    }
    if (speed > 0.05 && total > 0) {
      std::string sp = mib_text(speed);
      snprintf(line, sizeof(line), "%s/s", sp.c_str());
      text_center(cv, kCenter, 324, line, 2, kMuted);
      text_center(cv, kCenter, 348, "Quedan " + duration_text((total - done) / speed), 2, kMuted);
    }
    const float pulse = 0.65f + 0.35f * std::sin(time * 3.0f);
    text_center(cv, kCenter, 386, "No apagues la consola", 2, kPink, pulse);
    if (!cancelling) hints(cv, 430, {{"B", "Cancelar"}});
  }

  void draw_done(Canvas& cv) {
    text_center(cv, kCenter, 140, "¡LISTO!", 5, kGreen);
    text_center(cv, kCenter, 194, "game.pak creado", 2, kWhite);
    text_center(cv, kCenter, 230, fit(converted_name, 22), 2, kMuted);
    text_center(cv, kCenter, 254, "en " + duration_text(took), 2, kMuted);
    if (deleted) text_center(cv, kCenter, 278, "Disco borrado", 2, kMuted);
    text_center(cv, kCenter, 318, "Tu cactus floreció", 2, kPink);
    if (auto_quit) {
      const int left = std::max(0, 6 - int(now_seconds() - done_at));
      text_center(cv, kCenter, 370, "El juego empieza en " + std::to_string(demo_percent >= 0 ? 5 : left) + " s", 2, kGold);
      hints(cv, 430, {{"A", "Jugar ya"}});
    } else {
      hints(cv, 430, {{"A", "Salir"}});
    }
  }

  void draw_failed(Canvas& cv) {
    text_center(cv, kCenter, 132, "¡AY!", 5, kRed);
    text_center(cv, kCenter, 184, "No se pudo crear", 2, kWhite);
    text_center(cv, kCenter, 204, "game.pak", 2, kWhite);
    int y = 240;
    std::vector<std::string> lines = wrap(error, 22);
    if (lines.size() > 7) { lines.resize(7); lines.back() = fit(lines.back() + " xx", 21); }
    for (const std::string& l : lines) { text(cv, kLeft + 4, y, l, 2, 0xffb4a8); y += 22; }
    hints(cv, 430, {{"A", "Volver"}, {"B", "Salir"}});
  }

  // Demo values (--demo).
  double demo_percent = -1, demo_done = 0, demo_total = 0, demo_speed = 0;
  float demo_growth = -1, demo_bloom = 0, demo_wilt = 0;
};

std::string dir_of_exe() {
  char buf[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  return std::string(buf).substr(0, std::string(buf).rfind('/'));
}

void save_png(const Canvas& cv, const std::string& path) {
  std::vector<unsigned char> rgb(W * H * 3);
  for (int i = 0; i < W * H; ++i) {
    rgb[i * 3] = (unsigned char)(cv.px[i] >> 16); rgb[i * 3 + 1] = (unsigned char)(cv.px[i] >> 8); rgb[i * 3 + 2] = (unsigned char)cv.px[i];
  }
  stbi_write_png(path.c_str(), W, H, 3, rgb.data(), W * 3);
}

int demo(const std::string& out) {
  mkdir(out.c_str(), 0755);
  Scene scene;
  Canvas cv;
  App app;
  app.free_mib = 12650;
  app.identified = true;
  Image a; a.name = "Super Mario Galaxy (USA).rvz"; a.format = "RVZ"; a.mib = 1693; a.status = 0; a.id = "RMGE01";
  Image b; b.name = "Mario Kart Wii (USA).rvz"; b.format = "RVZ"; b.mib = 2870; b.status = 1; b.id = "RMCE01";
  Image c; c.name = "smg-copia.iso"; c.format = "ISO"; c.mib = 4482; c.status = -1;
  Image d; d.name = "notas.iso"; d.mib = 1; d.status = 3;
  app.images = {a, b, c, d};
  app.draw(cv, scene, 1.3f);
  save_png(cv, out + "/1-menu.png");
  app.screen = Screen::Converting;
  app.converted_name = a.name;
  app.demo_percent = 37; app.demo_done = 1251; app.demo_total = 3381; app.demo_speed = 14.8;
  app.demo_growth = 0.08f + 0.92f * 0.37f;
  app.draw(cv, scene, 2.1f);
  save_png(cv, out + "/2-convirtiendo-37.png");
  app.demo_percent = 84; app.demo_done = 2840; app.demo_total = 3381; app.demo_speed = 15.6;
  app.demo_growth = 0.08f + 0.92f * 0.84f;
  app.draw(cv, scene, 3.4f);
  save_png(cv, out + "/3-convirtiendo-84.png");
  app.screen = Screen::Done; app.took = 231; app.auto_quit = true;
  app.demo_growth = 1.0f; app.demo_bloom = 1.0f;
  app.draw(cv, scene, 4.0f);
  save_png(cv, out + "/4-listo.png");
  app.screen = Screen::Failed;
  app.error = "no hay espacio para game.pak: hacen falta 3390 MiB libres y hay 2104 MiB";
  app.demo_growth = 0.45f; app.demo_bloom = 0; app.demo_wilt = 1.0f;
  app.draw(cv, scene, 5.0f);
  save_png(cv, out + "/5-error.png");
  // The growing cactus, frame by frame (for an animation).
  app.screen = Screen::Converting; app.demo_wilt = 0;
  for (int f = 0; f <= 90; ++f) {
    const double p = std::min(100.0, f * 100.0 / 80.0);
    app.demo_percent = p; app.demo_done = 3381 * p / 100; app.demo_total = 3381; app.demo_speed = 15.2;
    app.demo_growth = float(0.08 + 0.92 * p / 100);
    if (f > 80) { app.screen = Screen::Done; app.demo_growth = 1; app.demo_bloom = std::min(1.0f, float(f - 80) / 6.0f); }
    app.draw(cv, scene, float(f) / 15.0f);
    char name[64];
    snprintf(name, sizeof(name), "/frame-%03d.png", f);
    save_png(cv, out + name);
  }
  printf("capturas en %s\n", out.c_str());
  return 0;
}

#ifndef CACTUS_NO_SDL
int run_ui(App& app) {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
    fprintf(stderr, "[cactus] SDL_Init: %s\n", SDL_GetError());
    return 3;
  }
  SDL_DisplayMode mode{};
  const bool have_mode = SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0;
  SDL_Window* window = SDL_CreateWindow("Cactus Galaxy Wrapper", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                        have_mode ? mode.w : W, have_mode ? mode.h : H,
                                        getenv("CACTUS_WINDOWED") ? 0 : SDL_WINDOW_FULLSCREEN);
  if (window == nullptr) { fprintf(stderr, "[cactus] ventana: %s\n", SDL_GetError()); SDL_Quit(); return 3; }
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer == nullptr) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (renderer == nullptr) { fprintf(stderr, "[cactus] renderer: %s\n", SDL_GetError()); SDL_Quit(); return 3; }
  SDL_ShowCursor(SDL_DISABLE);
  SDL_RenderSetLogicalSize(renderer, W, H);
  SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
  std::vector<SDL_GameController*> pads;
  for (int i = 0; i < SDL_NumJoysticks(); ++i) if (SDL_IsGameController(i)) pads.push_back(SDL_GameControllerOpen(i));
  // The RG35XX H's SDL mapping is positional: its A (right) is SDL's B, its B (bottom) SDL's A.
  const bool swap = getenv("CACTUS_SWAP_AB") != nullptr;
  Scene scene;
  Canvas cv;
  const double start = now_seconds();
  bool guide = false;
  while (!app.quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) app.quit = true;
      if (e.type == SDL_CONTROLLERDEVICEADDED) pads.push_back(SDL_GameControllerOpen(e.cdevice.which));
      if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
        const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
        const int b = e.cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_GUIDE) guide = down;
        if (!down) continue;
        if (b == SDL_CONTROLLER_BUTTON_START && guide) { app.quit = true; continue; }  // Function + Start
        if (b == (swap ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B) || b == SDL_CONTROLLER_BUTTON_START) app.press(App::kConfirm);
        if (b == (swap ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A) || b == SDL_CONTROLLER_BUTTON_BACK) app.press(App::kBack);
        if (b == SDL_CONTROLLER_BUTTON_Y) app.press(App::kToggle);  // the RG35XX H's X (top) is SDL's Y
        if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) app.press(App::kUp);
        if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) app.press(App::kDown);
      }
      if (e.type == SDL_KEYDOWN) {  // gptokeyb or a keyboard
        switch (e.key.keysym.sym) {
          case SDLK_RETURN: case SDLK_a: app.press(App::kConfirm); break;
          case SDLK_ESCAPE: case SDLK_b: case SDLK_BACKSPACE: app.press(App::kBack); break;
          case SDLK_x: app.press(App::kToggle); break;
          case SDLK_UP: app.press(App::kUp); break;
          case SDLK_DOWN: app.press(App::kDown); break;
          default: break;
        }
      }
    }
    app.tick();
    app.draw(cv, scene, float(now_seconds() - start));
    SDL_UpdateTexture(texture, nullptr, cv.px.data(), W * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    SDL_Delay(app.screen == Screen::Converting ? 60 : 30);  // leave the CPU to nebula-pak while it works
  }
  if (app.job && !app.job->finished) { kill(app.job->pid, SIGTERM); }
  if (app.job) { app.job->thread.join(); }
  for (SDL_GameController* p : pads) if (p) SDL_GameControllerClose(p);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  App app;
  app.smg = dir_of_exe();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--demo" && i + 1 < argc) return demo(argv[++i]);
    if (a == "--smg" && i + 1 < argc) app.smg = argv[++i];
    else if (a == "--auto") { app.auto_mode = true; app.auto_quit = true; }
    else if (a == "--delete-image") app.delete_after = true;
    else { fprintf(stderr, "uso: cactus-galaxy-wrapper [--smg <carpeta smg>] [--auto] [--delete-image] | --demo <carpeta>\n"); return 2; }
  }
  app.gamedata = app.smg + "/gamedata";
  mkdir(app.gamedata.c_str(), 0755);
  app.output = app.gamedata + "/game.pak";
  app.tool = dir_of_exe() + "/nebula-pak";
  if (access(app.tool.c_str(), X_OK) != 0) app.tool = app.smg + "/nebula-pak";
  std::string ports = app.smg.substr(0, app.smg.rfind('/'));
  std::string roms = ports.substr(0, ports.rfind('/'));
  app.images = find_images({app.gamedata, app.smg, roms + "/wii", "/userdata/roms/wii", "/roms/wii", "/roms2/wii",
                            "/storage/roms/wii"});
  app.refresh_disk();
  printf("[cactus] Cactus Galaxy Wrapper: %zu imagen(es), destino %s\n", app.images.size(), app.output.c_str());
  app.start_identify();
#ifndef CACTUS_NO_SDL
  const int ui = run_ui(app);
#else
  const int ui = 3;
#endif
  if (app.identify_thread.joinable()) app.identify_thread.join();
  if (ui == 3) return 3;
  app.refresh_disk();
  return app.have_pak ? 0 : 1;
}
