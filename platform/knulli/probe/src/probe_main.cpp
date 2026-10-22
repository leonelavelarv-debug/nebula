// SPDX-License-Identifier: GPL-3.0-only
// nebula-probe: feasibility probe for a Nebula-style (recompiled Wii game + GX on GLES) port to the Anbernic
// RG35XX H (Allwinner H700, 4x Cortex-A53, Mali-G31 MP2, 1 GiB) under KNULLI/PortMaster. It measures what decides
// whether the full game can run before anything of the game is built: memory headroom with the Wii address space
// mapped, recompiled-code speed, the GX front end's CPU cost, shader compile times (stutter), draw-call and fill
// throughput at the Wii's EFB size, EFB copies/readbacks, and vsync pacing.
//
//   nebula-probe [--out DIR] [--headless] [--quick] [--module-mib N]
#include "probe.h"
#include "font8x16.h"
#include "gles_min.h"
#include "sdl_min.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- report ----
struct Report {
    std::vector<std::string> lines;
    std::string json = "{\n";
    FILE* file = nullptr;
    void open(const std::string& dir) { file = std::fopen((dir + "/nebula-probe-report.txt").c_str(), "w"); }
    void line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[1024];
        va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        std::string s(buf);
        std::size_t start = 0;
        while (start <= s.size()) {
            const auto nl = s.find('\n', start);
            const std::string one = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!(one.empty() && nl == std::string::npos)) {
                lines.push_back(one);
                std::printf("%s\n", one.c_str());
                if (file) { std::fprintf(file, "%s\n", one.c_str()); std::fflush(file); }
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    void block(const std::string& text) { if (!text.empty()) line("%s", text.c_str()); }
    void num(const char* key, double v) { char b[160]; std::snprintf(b, sizeof b, "  \"%s\": %.4f,\n", key, v); json += b; }
    void str(const char* key, const std::string& v) {
        std::string e; for (char c : v) { if (c == '"' || c == '\\') e += '\\'; if (c == '\n') { e += "\\n"; continue; } if (static_cast<unsigned char>(c) >= 32) e += c; }
        json += "  \"" + std::string(key) + "\": \"" + e + "\",\n";
    }
    void write_json(const std::string& dir) {
        json += "  \"schema\": \"nebula-probe/1\"\n}\n";
        if (FILE* f = std::fopen((dir + "/nebula-probe-report.json").c_str(), "w")) { std::fputs(json.c_str(), f); std::fclose(f); }
        if (file) std::fclose(file);
    }
};

// ------------------------------------------------------------------- GL -----
struct Gfx {
    sdl::Api sdl;
    gl::Api g;
    sdl::Window* window = nullptr;
    sdl::GLContext ctx = nullptr;
    int es_major = 0, width = 640, height = 480;
    bool quit = false, pressed = false;
    std::string extensions;
    // UI
    gl::GLuint ui_prog = 0, font_tex = 0, ui_vbo = 0, blit_prog = 0;
    gl::GLint ui_color = -1, ui_screen = -1;

    bool has_ext(const char* e) const {
        const std::size_t n = std::strlen(e);
        for (std::size_t p = extensions.find(e); p != std::string::npos; p = extensions.find(e, p + 1)) {
            const bool start = p == 0 || extensions[p - 1] == ' ';
            const bool end = p + n == extensions.size() || extensions[p + n] == ' ';
            if (start && end) return true;
        }
        return false;
    }
    std::string vs_prefix() const { return es_major >= 3 ? "#version 300 es\n#define attribute in\n#define varying out\n" : "#version 100\n"; }
    std::string fs_prefix() const {
        return es_major >= 3 ? "#version 300 es\nprecision mediump float;\n#define varying in\n#define texture2D texture\nout vec4 o_color;\n#define gl_FragColor o_color\n"
                             : "#version 100\nprecision mediump float;\n";
    }
    gl::GLuint compile(gl::GLenum type, const std::string& src, std::string* log) {
        const gl::GLuint s = g.CreateShader(type);
        const char* p = src.c_str();
        g.ShaderSource(s, 1, &p, nullptr);
        g.CompileShader(s);
        gl::GLint ok = 0; g.GetShaderiv(s, gl::COMPILE_STATUS, &ok);
        if (!ok && log) { char b[1024] = {}; g.GetShaderInfoLog(s, sizeof b - 1, nullptr, b); *log += b; }
        return ok ? s : (g.DeleteShader(s), 0u);
    }
    gl::GLuint program(const std::string& vs, const std::string& fs, std::string* log = nullptr) {
        const gl::GLuint v = compile(gl::VERTEX_SHADER, vs_prefix() + vs, log);
        const gl::GLuint f = compile(gl::FRAGMENT_SHADER, fs_prefix() + fs, log);
        if (!v || !f) return 0;
        const gl::GLuint p = g.CreateProgram();
        g.AttachShader(p, v); g.AttachShader(p, f);
        g.BindAttribLocation(p, 0, "a_pos"); g.BindAttribLocation(p, 1, "a_col"); g.BindAttribLocation(p, 2, "a_tc");
        g.LinkProgram(p);
        gl::GLint ok = 0; g.GetProgramiv(p, gl::LINK_STATUS, &ok);
        if (!ok && log) { char b[1024] = {}; g.GetProgramInfoLog(p, sizeof b - 1, nullptr, b); *log += b; }
        g.DeleteShader(v); g.DeleteShader(f);
        return ok ? p : (g.DeleteProgram(p), 0u);
    }
    void pump() {
        sdl::Event e;
        while (sdl.PollEvent(&e)) {
            if (e.type == sdl::QUIT) quit = true;
            if (e.type == sdl::KEYDOWN || e.type == sdl::JOYBUTTONDOWN || e.type == sdl::CONTROLLERBUTTONDOWN) pressed = true;
        }
    }
    void init_ui() {
        ui_prog = program(
            "attribute vec2 a_pos; attribute vec2 a_tc; uniform vec2 u_screen; varying vec2 v_tc;\n"
            "void main(){ v_tc=a_tc; gl_Position=vec4(a_pos.x/u_screen.x*2.0-1.0, 1.0-a_pos.y/u_screen.y*2.0, 0.0, 1.0); }\n",
            "uniform sampler2D u_font; uniform vec4 u_color; varying vec2 v_tc;\n"
            "void main(){ float a=texture2D(u_font,v_tc).r; if(v_tc.x<0.0) a=1.0; gl_FragColor=vec4(u_color.rgb, u_color.a*a); }\n");
        ui_color = g.GetUniformLocation(ui_prog, "u_color");
        ui_screen = g.GetUniformLocation(ui_prog, "u_screen");
        std::vector<unsigned char> atlas(95 * 8 * 16);
        for (int c = 0; c < 95; ++c)
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 8; ++x) atlas[y * 760 + c * 8 + x] = (kFont8x16[c][y] >> (7 - x)) & 1 ? 255 : 0;
        g.GenTextures(1, &font_tex); g.BindTexture(gl::TEXTURE_2D, font_tex);
        g.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        g.TexImage2D(gl::TEXTURE_2D, 0, gl::LUMINANCE, 760, 16, 0, gl::LUMINANCE, gl::UNSIGNED_BYTE, atlas.data());
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST); g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE); g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
        g.GenBuffers(1, &ui_vbo);
        blit_prog = program("attribute vec2 a_pos; attribute vec2 a_tc; varying vec2 v_tc; void main(){ v_tc=a_tc; gl_Position=vec4(a_pos,0.0,1.0);} \n",
                            "uniform sampler2D u_tex; varying vec2 v_tc; void main(){ gl_FragColor=texture2D(u_tex,v_tc);} \n");
    }
    // Text and solid rectangles (u<0 marks a solid quad).
    std::vector<float> quads;
    void rect(float x, float y, float w, float h) {
        const float v[] = {x, y, -1, 0, x + w, y, -1, 0, x, y + h, -1, 0, x + w, y, -1, 0, x + w, y + h, -1, 0, x, y + h, -1, 0};
        quads.insert(quads.end(), v, v + 24);
    }
    void text(float x, float y, const std::string& s, int max_chars) {
        int col = 0;
        for (char ch : s) {
            if (col >= max_chars) break;
            const int c = (ch < 32 || ch > 126) ? '?' - 32 : ch - 32;
            const float u0 = c * 8 / 760.0f, u1 = (c * 8 + 8) / 760.0f, X = x + col * 8;
            const float v[] = {X, y, u0, 0, X + 8, y, u1, 0, X, y + 16, u0, 1, X + 8, y, u1, 0, X + 8, y + 16, u1, 1, X, y + 16, u0, 1};
            quads.insert(quads.end(), v, v + 24);
            ++col;
        }
    }
    void flush(float r, float gg, float b, float a = 1.0f) {
        if (quads.empty()) return;
        g.BindFramebuffer(gl::FRAMEBUFFER, 0);
        g.Viewport(0, 0, width, height);
        g.Disable(gl::DEPTH_TEST); g.Enable(gl::BLEND); g.BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
        g.UseProgram(ui_prog);
        g.Uniform4f(ui_color, r, gg, b, a);
        g.Uniform2f(ui_screen, float(width), float(height));
        g.ActiveTexture(gl::TEXTURE0); g.BindTexture(gl::TEXTURE_2D, font_tex);
        g.BindBuffer(gl::ARRAY_BUFFER, ui_vbo);
        g.BufferData(gl::ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(quads.size() * 4), quads.data(), gl::STREAM_DRAW);
        g.EnableVertexAttribArray(0); g.EnableVertexAttribArray(2); g.DisableVertexAttribArray(1);
        g.VertexAttribPointer(0, 2, gl::FLOAT, 0, 16, reinterpret_cast<void*>(0));
        g.VertexAttribPointer(2, 2, gl::FLOAT, 0, 16, reinterpret_cast<void*>(8));
        g.DrawArrays(gl::TRIANGLES, 0, static_cast<gl::GLsizei>(quads.size() / 4));
        g.DisableVertexAttribArray(2);
        quads.clear();
    }
};

Gfx* g_gfx = nullptr;
std::string g_shot;           // --shot FILE.ppm: capture of the final screen
double g_exit_after = 120.0;  // --exit-after SECONDS on the final screen

void save_screenshot(Gfx& x, const std::string& path) {
    std::vector<unsigned char> px(std::size_t(x.width) * x.height * 4);
    x.g.BindFramebuffer(gl::FRAMEBUFFER, 0);
    x.g.PixelStorei(gl::PACK_ALIGNMENT, 1);
    x.g.ReadPixels(0, 0, x.width, x.height, gl::RGBA, gl::UNSIGNED_BYTE, px.data());
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", x.width, x.height);
        for (int y = x.height - 1; y >= 0; --y)
            for (int i = 0; i < x.width; ++i) std::fwrite(&px[(std::size_t(y) * x.width + i) * 4], 1, 3, f);
        std::fclose(f);
    }
}
Report* g_report = nullptr;
std::string g_status = "iniciando";
double g_progress = 0;

void draw_log_screen(bool final_screen = false) {
    if (!g_gfx || !g_gfx->ctx) return;
    Gfx& x = *g_gfx;
    x.pump();
    x.g.BindFramebuffer(gl::FRAMEBUFFER, 0);
    x.g.Viewport(0, 0, x.width, x.height);
    x.g.ClearColor(0.05f, 0.06f, 0.10f, 1); x.g.Clear(gl::COLOR_BUFFER_BIT);
    const int cols = x.width / 8, rows = x.height / 16 - 3;
    x.rect(0, 0, float(x.width), 18); x.flush(0.18f, 0.22f, 0.45f);
    x.text(4, 1, final_screen ? "NEBULA PROBE - terminado. Pulsa un boton para salir." : ("NEBULA PROBE - " + g_status), cols - 1);
    x.flush(1, 1, 1);
    // Wrap long report lines; continuation lines are indented.
    std::vector<std::string> L;
    const auto& src = g_report->lines;
    for (std::size_t i = src.size() > std::size_t(rows) * 2 ? src.size() - std::size_t(rows) * 2 : 0; i < src.size(); ++i) {
        const std::string& s = src[i];
        if (int(s.size()) <= cols) { L.push_back(s); continue; }
        L.push_back(s.substr(0, cols));
        for (std::size_t p = cols; p < s.size(); p += cols - 4) L.push_back("    " + s.substr(p, cols - 4));
    }
    const int first = std::max(0, int(L.size()) - rows);
    for (int i = first; i < int(L.size()); ++i) x.text(0, 20 + (i - first) * 16.0f, L[i], cols);
    x.flush(0.85f, 0.92f, 0.85f);
    if (!final_screen) {
        x.rect(8, float(x.height - 14), float(x.width - 16), 8); x.flush(0.2f, 0.2f, 0.25f);
        x.rect(8, float(x.height - 14), float((x.width - 16) * std::clamp(g_progress, 0.0, 1.0)), 8); x.flush(0.3f, 0.8f, 0.4f);
    }
    if (final_screen && !g_shot.empty()) { save_screenshot(x, g_shot); g_shot.clear(); }
    x.sdl.GL_SwapWindow(x.window);
}

void status(const char* s, double p) { g_status = s; g_progress = p; g_report->line("== %s", s); draw_log_screen(); }

// ------------------------------------------------------------ GX-like GPU ---
const char* kVs =
    "attribute vec3 a_pos; attribute vec4 a_col; attribute vec2 a_tc;\n"
    "uniform vec4 u_mtx[30]; uniform float u_mtxidx; uniform vec4 u_ofs;\n"
    "varying vec4 v_col; varying vec2 v_tc0; varying vec2 v_tc1;\n"
    "void main(){ int m=int(u_mtxidx)*3; vec4 p=vec4(a_pos,1.0);\n"
    "  vec3 w=vec3(dot(u_mtx[m],p), dot(u_mtx[m+1],p), dot(u_mtx[m+2],p));\n"
    "  v_col=a_col; v_tc0=a_tc; v_tc1=a_tc*2.0+w.xy*0.1;\n"
    "  gl_Position=vec4(w.xy*u_ofs.z+u_ofs.xy, 0.5+0.4*w.z, 1.0); }\n";

// Uber TEV: every stage selects its four inputs and operation at run time from uniforms, as Nebula's uber pixel
// shader does while a specialised pipeline is being built.
const char* kUberFs =
    "uniform sampler2D u_t0; uniform sampler2D u_t1; uniform float u_stages;\n"
    "uniform vec4 u_sel[8]; uniform vec4 u_op[8]; uniform vec4 u_k[4];\n"
    "varying vec4 v_col; varying vec2 v_tc0; varying vec2 v_tc1;\n"
    "vec4 pick(float s, vec4 prev, vec4 t0, vec4 t1, vec4 k){\n"
    "  if(s<0.5) return prev; if(s<1.5) return t0; if(s<2.5) return t1; if(s<3.5) return v_col;\n"
    "  if(s<4.5) return k; if(s<5.5) return vec4(1.0); if(s<6.5) return vec4(0.5); return vec4(0.0); }\n"
    "void main(){ vec4 t0=texture2D(u_t0,v_tc0); vec4 t1=texture2D(u_t1,v_tc1); vec4 prev=v_col;\n"
    "  for(int i=0;i<8;i++){ if(float(i)>=u_stages) break; vec4 sel=u_sel[i]; vec4 op=u_op[i]; vec4 k=u_k[i-4*(i/4)];\n"
    "    vec4 a=pick(sel.x,prev,t0,t1,k); vec4 b=pick(sel.y,prev,t0,t1,k); vec4 c=pick(sel.z,prev,t0,t1,k); vec4 d=pick(sel.w,prev,t0,t1,k);\n"
    "    vec4 r=d+op.z*mix(a,b,c)+op.x; r*=op.y; prev= op.w>0.5 ? clamp(r,0.0,1.0) : r; }\n"
    "  gl_FragColor=prev; }\n";

std::string specialised_fs(int variant, int stages) {
    std::string s = "uniform sampler2D u_t0; uniform sampler2D u_t1; uniform vec4 u_k[4];\n"
                    "varying vec4 v_col; varying vec2 v_tc0; varying vec2 v_tc1;\n"
                    "void main(){ vec4 t0=texture2D(u_t0,v_tc0); vec4 t1=texture2D(u_t1,v_tc1); vec4 prev=v_col; vec4 r;\n";
    const char* inputs[] = {"prev", "t0", "t1", "v_col", "u_k[0]", "vec4(1.0)", "vec4(0.5)", "vec4(0.0)"};
    for (int i = 0; i < stages; ++i) {
        const int a = (variant + i) % 8, b = (variant * 3 + i) % 8, c = (variant * 5 + 2 * i) % 8, d = (variant * 7 + i) % 8;
        char line[256];
        std::snprintf(line, sizeof line, "  r=%s+mix(%s,%s,%s)+vec4(%d.0/255.0); prev=clamp(r*%s,0.0,1.0);\n", inputs[d], inputs[a], inputs[b], inputs[c],
                      variant, (i & 1) ? "2.0" : "1.0");
        s += line;
    }
    return s + "  gl_FragColor=prev; }\n";
}

struct Scene {
    gl::GLuint vbo = 0, ibo = 0, tex[2] = {}, efb_fbo = 0, efb_tex = 0, efb_depth = 0, copy_tex = 0;
    gl::GLsizei index_count = 0;
    static constexpr int kEfbW = 640, kEfbH = 528;
};

void bind_mesh(Gfx& x, Scene& s) {
    x.g.BindBuffer(gl::ARRAY_BUFFER, s.vbo); x.g.BindBuffer(gl::ELEMENT_ARRAY_BUFFER, s.ibo);
    x.g.EnableVertexAttribArray(0); x.g.EnableVertexAttribArray(1); x.g.EnableVertexAttribArray(2);
    x.g.VertexAttribPointer(0, 3, gl::FLOAT, 0, 24, reinterpret_cast<void*>(0));
    x.g.VertexAttribPointer(1, 4, gl::UNSIGNED_BYTE, 1, 24, reinterpret_cast<void*>(12));
    x.g.VertexAttribPointer(2, 2, gl::FLOAT, 0, 24, reinterpret_cast<void*>(16));
    x.g.ActiveTexture(gl::TEXTURE0 + 1); x.g.BindTexture(gl::TEXTURE_2D, s.tex[1]);
    x.g.ActiveTexture(gl::TEXTURE0); x.g.BindTexture(gl::TEXTURE_2D, s.tex[0]);
}

void set_common_uniforms(Gfx& x, gl::GLuint p, int stages) {
    x.g.UseProgram(p);
    x.g.Uniform1i(x.g.GetUniformLocation(p, "u_t0"), 0);
    x.g.Uniform1i(x.g.GetUniformLocation(p, "u_t1"), 1);
    float mtx[30 * 4];
    for (int m = 0; m < 10; ++m) {
        const float a = m * 0.3f, c = std::cos(a), s = std::sin(a);
        const float rows[12] = {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0};
        std::memcpy(mtx + m * 12, rows, sizeof rows);
    }
    x.g.Uniform4fv(x.g.GetUniformLocation(p, "u_mtx"), 30, mtx);
    float k[16]; for (int i = 0; i < 16; ++i) k[i] = 0.1f * float(i % 7);
    x.g.Uniform4fv(x.g.GetUniformLocation(p, "u_k"), 4, k);
    const gl::GLint st = x.g.GetUniformLocation(p, "u_stages");
    if (st >= 0) {
        x.g.Uniform1f(st, float(stages));
        float sel[32], op[32];
        for (int i = 0; i < 8; ++i) {
            sel[i * 4 + 0] = float((i + 1) % 8); sel[i * 4 + 1] = float((i * 3 + 2) % 8); sel[i * 4 + 2] = float((i + 5) % 8); sel[i * 4 + 3] = float(i % 4);
            op[i * 4 + 0] = 0.01f * i; op[i * 4 + 1] = (i & 1) ? 2.0f : 1.0f; op[i * 4 + 2] = (i & 2) ? -1.0f : 1.0f; op[i * 4 + 3] = 1.0f;
        }
        x.g.Uniform4fv(x.g.GetUniformLocation(p, "u_sel"), 8, sel);
        x.g.Uniform4fv(x.g.GetUniformLocation(p, "u_op"), 8, op);
    }
}

// Draws `draws` GX-like draw calls (100 triangles each, a uniform change per draw) into the EFB. Returns ms.
double render_efb_frame(Gfx& x, Scene& s, gl::GLuint p, int draws) {
    const double t0 = now();
    x.g.BindFramebuffer(gl::FRAMEBUFFER, s.efb_fbo);
    x.g.Viewport(0, 0, Scene::kEfbW, Scene::kEfbH);
    x.g.ClearColor(0.1f, 0.1f, 0.2f, 1); x.g.Clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
    x.g.Enable(gl::DEPTH_TEST); x.g.DepthFunc(gl::LEQUAL); x.g.Disable(gl::BLEND);
    x.g.UseProgram(p);
    bind_mesh(x, s);
    const gl::GLint idx = x.g.GetUniformLocation(p, "u_mtxidx"), ofs = x.g.GetUniformLocation(p, "u_ofs");
    for (int d = 0; d < draws; ++d) {
        x.g.Uniform1f(idx, float(d % 10));
        x.g.Uniform4f(ofs, -0.9f + 1.8f * float(d % 16) / 15.0f, -0.9f + 1.8f * float((d / 16) % 16) / 15.0f, 0.18f, 0);
        x.g.DrawElements(gl::TRIANGLES, s.index_count, gl::UNSIGNED_SHORT, nullptr);
    }
    x.g.Finish();
    return 1e3 * (now() - t0);
}

void present_efb(Gfx& x, Scene& s) {
    x.g.BindFramebuffer(gl::FRAMEBUFFER, 0);
    x.g.Viewport(0, 0, x.width, x.height);
    x.g.Disable(gl::DEPTH_TEST); x.g.Disable(gl::BLEND);
    x.g.UseProgram(x.blit_prog);
    x.g.Uniform1i(x.g.GetUniformLocation(x.blit_prog, "u_tex"), 0);
    x.g.ActiveTexture(gl::TEXTURE0); x.g.BindTexture(gl::TEXTURE_2D, s.efb_tex);
    static gl::GLuint vbo = 0;
    if (!vbo) {
        const float v[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
        x.g.GenBuffers(1, &vbo); x.g.BindBuffer(gl::ARRAY_BUFFER, vbo); x.g.BufferData(gl::ARRAY_BUFFER, sizeof v, v, gl::STATIC_DRAW);
    }
    x.g.BindBuffer(gl::ARRAY_BUFFER, vbo);
    x.g.DisableVertexAttribArray(1);
    x.g.EnableVertexAttribArray(0); x.g.EnableVertexAttribArray(2);
    x.g.VertexAttribPointer(0, 2, gl::FLOAT, 0, 16, reinterpret_cast<void*>(0));
    x.g.VertexAttribPointer(2, 2, gl::FLOAT, 0, 16, reinterpret_cast<void*>(8));
    x.g.DrawArrays(gl::TRIANGLE_STRIP, 0, 4);
}

double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[v.size() / 2]; }

// GX RGB5A3 4x4-tiled texture to RGBA8 (CPU side of a texture upload: what Nebula's TextureCache does per texture).
void decode_rgb5a3(const std::uint8_t* src, std::uint8_t* dst, int w, int h) {
    for (int ty = 0; ty < h; ty += 4)
        for (int tx = 0; tx < w; tx += 4)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const std::uint16_t v = std::uint16_t(src[0] << 8 | src[1]); src += 2;
                    std::uint8_t* o = dst + ((ty + y) * w + tx + x) * 4;
                    if (v & 0x8000) { o[0] = std::uint8_t(((v >> 10) & 31) * 255 / 31); o[1] = std::uint8_t(((v >> 5) & 31) * 255 / 31); o[2] = std::uint8_t((v & 31) * 255 / 31); o[3] = 255; }
                    else { o[0] = std::uint8_t(((v >> 8) & 15) * 17); o[1] = std::uint8_t(((v >> 4) & 15) * 17); o[2] = std::uint8_t((v & 15) * 17); o[3] = std::uint8_t(((v >> 12) & 7) * 255 / 7); }
                }
}

void run_gpu_tests(Gfx& x, Report& R, bool quick) {
    auto& g = x.g;
    status("GPU: cadenas y limites", 0.40);
    R.line("  GL_VERSION  %s", reinterpret_cast<const char*>(g.GetString(gl::VERSION)));
    R.line("  GL_RENDERER %s", reinterpret_cast<const char*>(g.GetString(gl::RENDERER)));
    R.line("  GL_VENDOR   %s", reinterpret_cast<const char*>(g.GetString(gl::VENDOR)));
    R.line("  GLSL        %s", reinterpret_cast<const char*>(g.GetString(gl::SHADING_LANGUAGE_VERSION)));
    R.str("gl_version", reinterpret_cast<const char*>(g.GetString(gl::VERSION)));
    R.str("gl_renderer", reinterpret_cast<const char*>(g.GetString(gl::RENDERER)));
    struct { gl::GLenum e; const char* n; } lim[] = {
        {gl::MAX_TEXTURE_SIZE, "MAX_TEXTURE_SIZE"}, {gl::MAX_TEXTURE_IMAGE_UNITS, "MAX_TEXTURE_IMAGE_UNITS"},
        {gl::MAX_VERTEX_UNIFORM_VECTORS, "MAX_VERTEX_UNIFORM_VECTORS"}, {gl::MAX_FRAGMENT_UNIFORM_VECTORS, "MAX_FRAGMENT_UNIFORM_VECTORS"},
        {gl::MAX_VARYING_VECTORS, "MAX_VARYING_VECTORS"}, {gl::MAX_VERTEX_ATTRIBS, "MAX_VERTEX_ATTRIBS"}, {gl::MAX_RENDERBUFFER_SIZE, "MAX_RENDERBUFFER_SIZE"}};
    std::string ls;
    for (auto& l : lim) { gl::GLint v = 0; g.GetIntegerv(l.e, &v); char b[64]; std::snprintf(b, sizeof b, "%s=%d ", l.n + 4, v); ls += b; R.num(l.n, v); }
    if (x.es_major >= 3) { gl::GLint v = 0; g.GetIntegerv(gl::MAX_SAMPLES, &v); ls += "SAMPLES=" + std::to_string(v) + " "; g.GetIntegerv(gl::MAX_DRAW_BUFFERS, &v); ls += "DRAW_BUFFERS=" + std::to_string(v); }
    R.line("  %s", ls.c_str());
    if (g.GetShaderPrecisionFormat) {
        gl::GLint range[2] = {}, prec = 0; g.GetShaderPrecisionFormat(gl::FRAGMENT_SHADER, gl::HIGH_FLOAT, range, &prec);
        R.line("  highp float en fragment: rango 2^%d, precision %d bits%s", range[1], prec, prec == 0 ? " (NO soportado)" : "");
    }
    // What Nebula's GX backend needs (docs/VULKAN.md, "Portability limits") and how this GPU answers.
    struct { const char* ext; const char* use; } need[] = {
        {"GL_EXT_blend_func_extended", "blend dual-source (alfa SRC1 de TEV)"},
        {"GL_EXT_shader_framebuffer_fetch", "logic-op/blend en shader"},
        {"GL_ARM_shader_framebuffer_fetch", "logic-op/blend en shader (ARM)"},
        {"GL_EXT_texture_compression_s3tc", "texturas CMPR sin decodificar"},
        {"GL_EXT_texture_compression_dxt1", "CMPR (DXT1)"},
        {"GL_OES_depth24", "profundidad 24 bits (EFB Z24)"},
        {"GL_OES_packed_depth_stencil", "Z24S8"},
        {"GL_EXT_geometry_shader", "lineas/puntos anchos GX"},
        {"GL_OES_texture_npot", "texturas NPOT con mip/repeat"},
        {"GL_EXT_disjoint_timer_query", "medicion GPU real"},
        {"GL_KHR_parallel_shader_compile", "compilar shaders sin bloquear"},
        {"GL_OES_get_program_binary", "cache de programas en disco"},
        {"GL_EXT_buffer_storage", "anillos persistentes"},
        {"GL_EXT_clip_control", "rango de profundidad GX"},
        {"GL_EXT_color_buffer_half_float", "EFB de alta precision"},
        {"GL_OES_EGL_image_external", "presentacion zero-copy"},
    };
    int found = 0;
    for (auto& n : need) {
        const bool has = x.has_ext(n.ext);
        found += has;
        R.line("  [%s] %-34s %s", has ? "SI" : "no", n.ext, n.use);
        R.num(n.ext, has ? 1 : 0);
    }
    if (x.es_major >= 3) { gl::GLint bin = 0; g.GetIntegerv(0x87FE /*NUM_PROGRAM_BINARY_FORMATS*/, &bin); R.line("  formatos de binario de programa (ES3): %d", bin); R.num("program_binary_formats", bin); }

    // ---- shaders
    status("GPU: compilacion de shaders (tirones)", 0.45);
    std::string log;
    const double tu0 = now();
    const gl::GLuint uber = x.program(kVs, kUberFs, &log);
    const double uber_ms = 1e3 * (now() - tu0);
    R.line("  uber TEV (8 etapas dinamicas): %s en %.1f ms %s", uber ? "OK" : "FALLO", uber_ms, log.c_str());
    R.num("uber_compile_ms", uber_ms);
    const int variants = quick ? 8 : 24;
    std::vector<double> spec_ms; gl::GLuint spec = 0;
    for (int v = 0; v < variants; ++v) {
        const double t = now();
        std::string l2;
        const gl::GLuint p = x.program(kVs, specialised_fs(v + 1, 4 + v % 5), &l2);
        g.UseProgram(p); g.Finish();
        spec_ms.push_back(1e3 * (now() - t));
        if (v == 0) spec = p; else if (p) g.DeleteProgram(p);
        if (!p) R.line("  variante %d fallo: %s", v, l2.c_str());
        if (v % 6 == 0) draw_log_screen();
    }
    const double spec_med = median(spec_ms), spec_max = *std::max_element(spec_ms.begin(), spec_ms.end());
    R.line("  %d pipelines especializados: mediana %.1f ms, max %.1f ms por programa", variants, spec_med, spec_max);
    R.line("  -> a 60 fps un frame dura 16.7 ms: cada programa nuevo en el hilo de render es %s", spec_med > 8 ? "un tiron visible" : "tolerable");
    R.num("specialised_compile_median_ms", spec_med); R.num("specialised_compile_max_ms", spec_max);
    if (!uber || !spec) { R.line("  sin programas validos: se omiten pruebas de dibujo"); return; }

    // ---- scene resources
    Scene s;
    {
        std::vector<float> verts; std::vector<std::uint16_t> idx;
        const int gx = 11, gy = 6;
        for (int y = 0; y < gy; ++y)
            for (int xx = 0; xx < gx; ++xx) {
                const float fx = xx / float(gx - 1) * 2 - 1, fy = y / float(gy - 1) * 2 - 1;
                std::uint32_t col = 0xFF000000u | (std::uint32_t(xx * 23) << 16) | (std::uint32_t(y * 40) << 8) | 0x80u;
                float colf; std::memcpy(&colf, &col, 4);
                const float v[6] = {fx, fy, 0.3f * std::sin(fx * 3.0f), colf, (fx + 1) * 2, (fy + 1) * 2};
                verts.insert(verts.end(), v, v + 6);
            }
        for (int y = 0; y + 1 < gy; ++y)
            for (int xx = 0; xx + 1 < gx; ++xx) {
                const std::uint16_t a = std::uint16_t(y * gx + xx), b = std::uint16_t(a + 1), c = std::uint16_t(a + gx), d = std::uint16_t(c + 1);
                const std::uint16_t q[6] = {a, b, c, b, d, c}; idx.insert(idx.end(), q, q + 6);
            }
        s.index_count = static_cast<gl::GLsizei>(idx.size());  // 100 triangles
        g.GenBuffers(1, &s.vbo); g.BindBuffer(gl::ARRAY_BUFFER, s.vbo); g.BufferData(gl::ARRAY_BUFFER, verts.size() * 4, verts.data(), gl::STATIC_DRAW);
        g.GenBuffers(1, &s.ibo); g.BindBuffer(gl::ELEMENT_ARRAY_BUFFER, s.ibo); g.BufferData(gl::ELEMENT_ARRAY_BUFFER, idx.size() * 2, idx.data(), gl::STATIC_DRAW);
        std::vector<std::uint8_t> texel(256 * 256 * 4);
        for (int i = 0; i < 256 * 256; ++i) { texel[i * 4] = std::uint8_t(i); texel[i * 4 + 1] = std::uint8_t(i >> 8); texel[i * 4 + 2] = std::uint8_t((i >> 3) ^ i); texel[i * 4 + 3] = 255; }
        g.GenTextures(2, s.tex);
        for (auto t : s.tex) {
            g.BindTexture(gl::TEXTURE_2D, t);
            g.TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA, 256, 256, 0, gl::RGBA, gl::UNSIGNED_BYTE, texel.data());
            g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR); g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
            g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::REPEAT); g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::REPEAT);
        }
        // EFB: 640x528 RGBA8 + depth, as the Wii's embedded frame buffer at 1x.
        g.GenTextures(1, &s.efb_tex); g.BindTexture(gl::TEXTURE_2D, s.efb_tex);
        g.TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA, Scene::kEfbW, Scene::kEfbH, 0, gl::RGBA, gl::UNSIGNED_BYTE, nullptr);
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR); g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE); g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
        g.GenRenderbuffers(1, &s.efb_depth); g.BindRenderbuffer(gl::RENDERBUFFER, s.efb_depth);
        const bool d24 = x.es_major >= 3 || x.has_ext("GL_OES_depth24");
        g.RenderbufferStorage(gl::RENDERBUFFER, d24 ? gl::DEPTH_COMPONENT24 : gl::DEPTH_COMPONENT16, Scene::kEfbW, Scene::kEfbH);
        g.GenFramebuffers(1, &s.efb_fbo); g.BindFramebuffer(gl::FRAMEBUFFER, s.efb_fbo);
        g.FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, s.efb_tex, 0);
        g.FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, s.efb_depth);
        const gl::GLenum fbs = g.CheckFramebufferStatus(gl::FRAMEBUFFER);
        R.line("  EFB 640x528 RGBA8 + Z%d: %s", d24 ? 24 : 16, fbs == gl::FRAMEBUFFER_COMPLETE ? "completo" : "INCOMPLETO");
        g.GenTextures(1, &s.copy_tex); g.BindTexture(gl::TEXTURE_2D, s.copy_tex);
        g.TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA, Scene::kEfbW, Scene::kEfbH, 0, gl::RGBA, gl::UNSIGNED_BYTE, nullptr);
        g.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    }
    set_common_uniforms(x, uber, 4);
    set_common_uniforms(x, spec, 4);
    const MemSnapshot after_res = mem_snapshot();
    R.line("  memoria tras crear recursos GL: RSS %ld MiB, PSS %ld MiB, MemAvailable %ld MiB", after_res.rss_kib / 1024, after_res.pss_kib / 1024, after_res.mem_available_kib / 1024);
    R.num("rss_after_gl_mib", after_res.rss_kib / 1024.0); R.num("pss_after_gl_mib", after_res.pss_kib / 1024.0);

    // ---- draw calls
    status("GPU: llamadas de dibujo en el EFB (uber vs especializado)", 0.55);
    const int counts[] = {100, 250, 500, 1000, 2000};
    double ms_at[5] = {};
    for (int ci = 0; ci < 5; ++ci) {
        std::vector<double> t;
        for (int f = 0; f < (quick ? 4 : 8); ++f) { t.push_back(render_efb_frame(x, s, uber, counts[ci])); }
        ms_at[ci] = median(t);
        present_efb(x, s); draw_log_screen();
        R.line("  uber: %4d draws x 100 tri -> %6.2f ms/frame", counts[ci], ms_at[ci]);
        R.num(("uber_ms_" + std::to_string(counts[ci])).c_str(), ms_at[ci]);
    }
    std::vector<double> ts;
    for (int f = 0; f < (quick ? 4 : 8); ++f) ts.push_back(render_efb_frame(x, s, spec, 500));
    const double spec500 = median(ts);
    R.line("  especializado: 500 draws -> %6.2f ms/frame (uber %.2f)", spec500, ms_at[2]);
    R.num("spec_ms_500", spec500);
    // Draws that fit in 12 ms (16.7 ms frame minus present and the game's own GPU slack).
    const double per_draw = (ms_at[4] - ms_at[0]) / (counts[4] - counts[0]);
    const double fixed = ms_at[0] - per_draw * counts[0];
    const double fit12 = per_draw > 0 ? (12.0 - fixed) / per_draw : 0;
    if (fit12 > 0) R.line("  costo marginal %.4f ms/draw, fijo %.2f ms -> ~%.0f draws caben en 12 ms", per_draw, fixed, fit12);
    else R.line("  costo marginal %.4f ms/draw, fijo %.2f ms -> ni 100 draws caben en 12 ms", per_draw, fixed);
    R.num("draw_marginal_ms", per_draw); R.num("draws_in_12ms", fit12);

    // ---- fill rate at EFB size, blended full-screen layers
    status("GPU: tasa de relleno (capas a pantalla completa)", 0.62);
    {
        x.g.BindFramebuffer(gl::FRAMEBUFFER, s.efb_fbo); x.g.Viewport(0, 0, Scene::kEfbW, Scene::kEfbH);
        x.g.Disable(gl::DEPTH_TEST); x.g.Enable(gl::BLEND); x.g.BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
        x.g.UseProgram(uber); bind_mesh(x, s);
        const gl::GLint idx = g.GetUniformLocation(uber, "u_mtxidx"), ofs = g.GetUniformLocation(uber, "u_ofs");
        g.Uniform1f(idx, 0); g.Uniform4f(ofs, 0, 0, 1.0f, 0);
        const int layers = 16;
        g.Finish();
        const double t0 = now();
        for (int f = 0; f < 5; ++f) for (int l = 0; l < layers; ++l) g.DrawElements(gl::TRIANGLES, s.index_count, gl::UNSIGNED_SHORT, nullptr);
        g.Finish();
        const double sec = now() - t0;
        const double mpix = 5.0 * layers * Scene::kEfbW * Scene::kEfbH / sec / 1e6;
        R.line("  uber 4 etapas + 2 texturas + blend: %.0f Mpix/s (= %.1f capas EFB por frame a 60 fps)", mpix, mpix * 1e6 / 60 / (Scene::kEfbW * Scene::kEfbH));
        R.num("fill_mpix_s", mpix);
    }

    // ---- textures
    status("GPU: subida de texturas y decodificacion GX en CPU", 0.68);
    {
        std::vector<std::uint8_t> gx(256 * 256 * 2), rgba(256 * 256 * 4);
        for (std::size_t i = 0; i < gx.size(); ++i) gx[i] = std::uint8_t(i * 37 + (i >> 9));
        const int n = quick ? 16 : 64;
        double t0 = now();
        for (int i = 0; i < n; ++i) decode_rgb5a3(gx.data(), rgba.data(), 256, 256);
        const double dec = now() - t0;
        g.BindTexture(gl::TEXTURE_2D, s.tex[1]);
        g.Finish(); t0 = now();
        for (int i = 0; i < n; ++i) { rgba[i] ^= 1; g.TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, 256, 256, gl::RGBA, gl::UNSIGNED_BYTE, rgba.data()); }
        g.Finish();
        const double up = now() - t0;
        const double mb = n * 256.0 * 256 * 4 / 1e6;
        R.line("  RGB5A3 256x256 -> RGBA8 en CPU: %.2f ms/textura (%.0f MB/s salida)", 1e3 * dec / n, mb / dec);
        R.line("  glTexSubImage2D 256x256 RGBA8: %.2f ms/textura (%.0f MB/s)", 1e3 * up / n, mb / up);
        R.num("tex_decode_ms", 1e3 * dec / n); R.num("tex_upload_ms", 1e3 * up / n);
    }

    // ---- EFB copies and peeks
    status("GPU: copias EFB a textura y lecturas a RAM", 0.72);
    {
        render_efb_frame(x, s, uber, 100);
        std::vector<double> cp, rd;
        std::vector<std::uint8_t> pixels(Scene::kEfbW * Scene::kEfbH * 4);
        for (int i = 0; i < 10; ++i) {
            g.BindFramebuffer(gl::FRAMEBUFFER, s.efb_fbo);
            g.BindTexture(gl::TEXTURE_2D, s.copy_tex);
            double t0 = now(); g.CopyTexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, 0, 0, Scene::kEfbW, Scene::kEfbH); g.Finish(); cp.push_back(1e3 * (now() - t0));
            g.PixelStorei(gl::PACK_ALIGNMENT, 4);
            t0 = now(); g.ReadPixels(0, 0, Scene::kEfbW, Scene::kEfbH, gl::RGBA, gl::UNSIGNED_BYTE, pixels.data()); rd.push_back(1e3 * (now() - t0));
        }
        double t0 = now(); std::uint8_t px[4]; for (int i = 0; i < 32; ++i) g.ReadPixels(i * 9, i * 7, 1, 1, gl::RGBA, gl::UNSIGNED_BYTE, px); const double peek = 1e3 * (now() - t0) / 32;
        R.line("  copia EFB->textura 640x528: %.2f ms; lectura completa a RAM: %.2f ms; peek de 1 pixel: %.3f ms", median(cp), median(rd), peek);
        R.num("efb_copy_ms", median(cp)); R.num("efb_readback_ms", median(rd)); R.num("efb_peek_ms", peek);
        std::uint32_t sum = 0; for (std::size_t i = 0; i < pixels.size(); i += 997) sum += pixels[i];
        R.line("  (suma de control de la lectura: %u; 0 indicaria que no se dibujo nada)", sum);
    }

    // ---- frame pacing with vsync
    status("GPU: ritmo de presentacion con vsync (3 s)", 0.78);
    {
        x.sdl.GL_SetSwapInterval(1);
        std::vector<double> iv;
        double last = now();
        const int frames = quick ? 90 : 180;
        for (int f = 0; f < frames && !x.quit; ++f) {
            render_efb_frame(x, s, uber, 300);
            present_efb(x, s);
            x.rect(0, 0, float(x.width), 18); x.flush(0, 0, 0, 0.6f);
            x.text(4, 1, "vsync: frame " + std::to_string(f) + "/" + std::to_string(frames) + " (300 draws/frame)", 79); x.flush(1, 1, 0.4f);
            x.sdl.GL_SwapWindow(x.window);
            x.pump();
            const double t = now(); iv.push_back(1e3 * (t - last)); last = t;
        }
        iv.erase(iv.begin());
        std::vector<double> sorted = iv; std::sort(sorted.begin(), sorted.end());
        double mean = 0; int late = 0; for (double v : iv) { mean += v; late += v > 20.0; }
        mean /= iv.size();
        R.line("  intervalo medio %.2f ms (%.1f Hz), p95 %.2f ms, max %.2f ms, frames >20 ms: %d de %zu", mean, 1000 / mean, sorted[sorted.size() * 95 / 100], sorted.back(), late, iv.size());
        R.num("present_mean_ms", mean); R.num("present_p95_ms", sorted[sorted.size() * 95 / 100]); R.num("present_late", late);
    }
}

bool init_gfx(Gfx& x, Report& R) {
    if (!x.sdl.load()) { R.line("  SDL2 no encontrado (libSDL2-2.0.so.0): se omiten pruebas graficas"); return false; }
    sdl::Version v{}; x.sdl.GetVersion(&v);
    R.line("  SDL %d.%d.%d cargado de %s", v.major, v.minor, v.patch, x.sdl.loaded_from);
    if (x.sdl.Init(sdl::INIT_VIDEO | sdl::INIT_JOYSTICK | sdl::INIT_EVENTS) != 0) { R.line("  SDL_Init fallo: %s", x.sdl.GetError()); return false; }
    R.line("  driver de video SDL: %s", x.sdl.GetCurrentVideoDriver() ? x.sdl.GetCurrentVideoDriver() : "?");
    sdl::DisplayMode dm{};
    if (x.sdl.GetCurrentDisplayMode(0, &dm) == 0) { R.line("  pantalla: %dx%d @ %d Hz", dm.w, dm.h, dm.refresh_rate); x.width = dm.w; x.height = dm.h; R.num("display_hz", dm.refresh_rate); }
    for (int major : {3, 2}) {
        x.sdl.GL_ResetAttributes();
        x.sdl.GL_SetAttribute(sdl::GL_CONTEXT_PROFILE_MASK, sdl::GL_CONTEXT_PROFILE_ES);
        x.sdl.GL_SetAttribute(sdl::GL_CONTEXT_MAJOR_VERSION, major);
        x.sdl.GL_SetAttribute(sdl::GL_CONTEXT_MINOR_VERSION, 0);
        x.sdl.GL_SetAttribute(sdl::GL_DOUBLEBUFFER, 1);
        x.sdl.GL_SetAttribute(sdl::GL_DEPTH_SIZE, 0);
        if (!x.window) x.window = x.sdl.CreateWindow("nebula-probe", sdl::WINDOWPOS_UNDEFINED, sdl::WINDOWPOS_UNDEFINED, x.width, x.height,
                                                     sdl::WINDOW_OPENGL | sdl::WINDOW_SHOWN | sdl::WINDOW_FULLSCREEN_DESKTOP);
        if (!x.window) { R.line("  SDL_CreateWindow fallo: %s", x.sdl.GetError()); return false; }
        x.ctx = x.sdl.GL_CreateContext(x.window);
        if (x.ctx) { x.es_major = major; break; }
        R.line("  contexto GLES %d.0 no disponible: %s", major, x.sdl.GetError());
    }
    if (!x.ctx) return false;
    x.sdl.GL_MakeCurrent(x.window, x.ctx);
    x.sdl.ShowCursor(0);
    x.g.load(x.sdl.GL_GetProcAddress);
    if (x.g.missing_core) { R.line("  faltan %d funciones GLES basicas", x.g.missing_core); return false; }
    x.sdl.GL_GetDrawableSize(x.window, &x.width, &x.height);
    R.line("  contexto GLES %d.0 creado, superficie %dx%d", x.es_major, x.width, x.height);
    R.num("gles_major", x.es_major);
    if (x.es_major >= 3 && x.g.GetStringi) {
        gl::GLint n = 0; x.g.GetIntegerv(gl::NUM_EXTENSIONS, &n);
        for (int i = 0; i < n; ++i) { x.extensions += reinterpret_cast<const char*>(x.g.GetStringi(gl::EXTENSIONS, i)); x.extensions += ' '; }
    } else if (const auto* e = x.g.GetString(gl::EXTENSIONS)) x.extensions = reinterpret_cast<const char*>(e);
    x.sdl.GL_SetSwapInterval(0);
    for (int j = 0; j < x.sdl.NumJoysticks(); ++j) if (auto* js = x.sdl.JoystickOpen(j)) R.line("  mando %d: %s", j, x.sdl.JoystickName(js));
    x.init_ui();
    return x.ui_prog != 0;
}
}  // namespace

int main(int argc, char** argv) {
    std::string out = ".";
    bool headless = false, quick = false;
    std::size_t module_mib = 64;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--headless")) headless = true;
        else if (!std::strcmp(argv[i], "--quick")) quick = true;
        else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) g_shot = argv[++i];
        else if (!std::strcmp(argv[i], "--exit-after") && i + 1 < argc) g_exit_after = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--module-mib") && i + 1 < argc) module_mib = std::strtoul(argv[++i], nullptr, 10);
    }
    Report R; R.open(out); g_report = &R;
    Gfx gfx; g_gfx = &gfx;
    R.line("Nebula probe 0.1 - viabilidad de un port recompilado (Wii -> RG35XX H)");
    R.block(sys_summary());
    const MemSnapshot m0 = mem_snapshot();
    R.line("  RAM total %ld MiB, disponible %ld MiB, swap %ld/%ld MiB libres", m0.mem_total_kib / 1024, m0.mem_available_kib / 1024, m0.swap_free_kib / 1024, m0.swap_total_kib / 1024);
    R.num("mem_total_mib", m0.mem_total_kib / 1024.0); R.num("mem_available_start_mib", m0.mem_available_kib / 1024.0);

    if (!headless) {
        R.line("== video");
        if (!init_gfx(gfx, R)) { gfx.ctx = nullptr; R.line("  (continuando sin GPU)"); }
        else {
            const MemSnapshot mg = mem_snapshot();
            R.line("  memoria tras crear el contexto: RSS %ld MiB, PSS %ld MiB", mg.rss_kib / 1024, mg.pss_kib / 1024);
            R.num("pss_after_context_mib", mg.pss_kib / 1024.0);
        }
    }

    status("CPU: informacion, ancho de banda y nucleos", 0.05);
    const CpuReport cpu = run_cpu_info_and_bench();
    R.line("  %s", cpu.model.c_str());
    std::string fr; for (int c = 0; c < cpu.cores && c < 8; ++c) fr += std::to_string(cpu.cur_khz[c] / 1000) + "/" + std::to_string(cpu.max_khz[c] / 1000) + " ";
    R.line("  %d nucleos, MHz actual/max: %s gobernador=%s", cpu.cores, fr.c_str(), cpu.governor.c_str());
    R.line("  temperatura: %s", cpu.thermal.empty() ? "n/d" : cpu.thermal.c_str());
    R.line("  memcpy 32 MiB: %.2f GB/s; ALU 1 hilo %.2f ns/iter, %d hilos %.2f ns/iter/hilo", cpu.memcpy_gbs, cpu.single_hash_ns, cpu.threads, cpu.multi_hash_ns);
    R.num("memcpy_gbs", cpu.memcpy_gbs); R.num("alu_single_ns", cpu.single_hash_ns); R.num("alu_multi_ns", cpu.multi_hash_ns);

    status("CPU: codigo PowerPC recompilado por Nebula", 0.12);
    const RecompReport rc = run_recomp_probes(quick ? 1 : 2);
    R.block(rc.text);
    R.line("  ns/iter recompilado vs nativo: suma %.2f/%.2f (x%.1f), fmadds %.2f/%.2f (x%.1f), ALU %.2f/%.2f (x%.1f)",
           rc.sum_ns, rc.sum_native_ns, rc.sum_ns / rc.sum_native_ns, rc.dot_ns, rc.dot_native_ns, rc.dot_ns / rc.dot_native_ns,
           rc.hash_ns, rc.hash_native_ns, rc.hash_ns / rc.hash_native_ns);
    R.line("  (forma simple de lower_ppc_code_range: registros en memoria; el modulo completo los mantiene en locales)");
    R.num("recomp_passed", rc.passed); R.num("recomp_failed", rc.failed);
    R.num("recomp_sum_ns", rc.sum_ns); R.num("recomp_fmadds_ns", rc.dot_ns); R.num("recomp_alu_ns", rc.hash_ns);
    R.num("native_sum_ns", rc.sum_native_ns); R.num("native_fmadds_ns", rc.dot_native_ns); R.num("native_alu_ns", rc.hash_native_ns);

    {
        // The same probes lowered as Nebula's release module lowers game code (separate executable: the fast-float
        // helpers must not share a binary with the exact ones, as Nebula keeps the game module apart from the runtime).
        char exe[512] = {}; const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        std::string dir = n > 0 ? std::string(exe, std::size_t(n)) : std::string("./x");
        dir = dir.substr(0, dir.rfind('/'));
        const std::string cmd = "'" + dir + "/nebula-probe-cpu-fast' " + (quick ? "1" : "2") + " 2>&1";
        R.line("  -- modulo optimizado (native+fast float, vista plana, registros en locales):");
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[512];
            while (std::fgets(buf, sizeof buf, p)) { std::string l(buf); while (!l.empty() && l.back() == '\n') l.pop_back(); if (l.rfind("bench", 0) == 0 || l.find("FAIL") != std::string::npos || l.rfind("ALL", 0) == 0 || l.rfind("XARCH", 0) == 0 || l.find("fctiwz") != std::string::npos) R.line("  %s", l.c_str()); }
            R.num("cpu_fast_status", pclose(p));
        }
    }
    status("CPU: frente GX de Nebula (FifoParser + claves de shader)", 0.20);
    for (unsigned draws : {300u, 1000u}) {
        const GxReport gx = run_gx_fifo_bench(draws, quick ? 20 : 60);
        R.line("  %4u draws/frame (%zu KB de FIFO): %s %.2f ms/frame, %.0f ns/draw, %.0f MB/s %s", draws, gx.frame_bytes / 1024, gx.ok ? "OK" : "FALLO",
               gx.ms_per_frame, gx.ns_per_draw, gx.mb_per_s, gx.ok ? "" : gx.error);
        R.num(("gx_ms_" + std::to_string(draws)).c_str(), gx.ms_per_frame); R.num(("gx_ns_per_draw_" + std::to_string(draws)).c_str(), gx.ns_per_draw);
    }

    status("Memoria: espacio de direcciones Wii y anillo GX", 0.28);
    const GuestMemReport gm = run_guest_memory_test(module_mib);
    R.block(gm.text);
    R.num("guest_reserve_4g", gm.reserve_4g); R.num("guest_ring_alias", gm.ring_alias); R.num("guest_pss_growth_mib", gm.pss_growth_kib / 1024.0);
    R.num("available_with_guest_mib", gm.available_after_kib / 1024.0); R.num("available_with_guest_and_module_mib", gm.available_with_module_kib / 1024.0);

    status("Vulkan", 0.34);
    const VkReport vk = run_vulkan_probe();
    R.block(vk.text);
    R.num("vulkan_devices", vk.devices);

    if (gfx.ctx) run_gpu_tests(gfx, R, quick);

    const MemSnapshot mend = mem_snapshot();
    R.line("== resumen");
    R.line("  pico RSS %ld MiB; MemAvailable al final %ld MiB", mend.hwm_kib / 1024, mend.mem_available_kib / 1024);
    R.num("peak_rss_mib", mend.hwm_kib / 1024.0);
    R.line("  informe: %s/nebula-probe-report.txt y .json", out.c_str());
    g_progress = 1.0;
    R.write_json(out);
    if (gfx.ctx) {
        gfx.pressed = false;
        const double t0 = now();
        while (!gfx.pressed && !gfx.quit && now() - t0 < g_exit_after) { draw_log_screen(true); }
        gfx.sdl.GL_DeleteContext(gfx.ctx); gfx.sdl.DestroyWindow(gfx.window); gfx.sdl.Quit();
    }
    return rc.failed != 0 ? 2 : 0;
}
