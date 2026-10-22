#include "Renderer.h"
#include "Math3D.h"
#include "../util/Log.h"
#include "../world/Terrain.h"
#include "../world/Way.h"
#include "../world/B3D.h"
#include "../world/SceneTextures.h"
#include "../world/GroundMap.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace ht2 {

// ---- anisotropic filtering (GL_EXT_texture_filter_anisotropic) -------------
// The token is not in every GL header, and the entry points are not part of the
// core-profile set the loader resolves, so they are fetched here (same pattern
// as glTexSubImage2D in updateTexture()).
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
typedef void (*PFN_TexParameterf)(GLenum, GLenum, GLfloat);
typedef void (*PFN_GetFloatv)(GLenum, GLfloat*);
typedef void (*PFN_GetTexParameterfv)(GLenum, GLenum, GLfloat*);
typedef void (*PFN_GetTexParameteriv)(GLenum, GLenum, GLint*);
typedef void (*PFN_GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint*);
typedef const GLubyte* (*PFN_GetString)(GLenum);

// ---- framebuffer objects (render targets) ---------------------------------
// GL 3.0 / ARB_framebuffer_object, resolved through SDL_GL_GetProcAddress for
// the same reason as the anisotropic-filter entry points above: they are not
// part of the loader's core set.  The tokens are guarded because an older
// <GL/glext.h> may not define them.
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
typedef void   (*PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void   (*PFN_DeleteFramebuffers)(GLsizei, const GLuint*);
typedef void   (*PFN_BindFramebuffer)(GLenum, GLuint);
typedef void   (*PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFN_CheckFramebufferStatus)(GLenum);
typedef void   (*PFN_GenRenderbuffers)(GLsizei, GLuint*);
typedef void   (*PFN_DeleteRenderbuffers)(GLsizei, const GLuint*);
typedef void   (*PFN_BindRenderbuffer)(GLenum, GLuint);
typedef void   (*PFN_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (*PFN_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);

struct FboApi {
    PFN_GenFramebuffers            genFramebuffers = nullptr;
    PFN_DeleteFramebuffers         deleteFramebuffers = nullptr;
    PFN_BindFramebuffer            bindFramebuffer = nullptr;
    PFN_FramebufferTexture2D       framebufferTexture2D = nullptr;
    PFN_CheckFramebufferStatus     checkFramebufferStatus = nullptr;
    PFN_GenRenderbuffers           genRenderbuffers = nullptr;
    PFN_DeleteRenderbuffers        deleteRenderbuffers = nullptr;
    PFN_BindRenderbuffer           bindRenderbuffer = nullptr;
    PFN_RenderbufferStorage        renderbufferStorage = nullptr;
    PFN_FramebufferRenderbuffer    framebufferRenderbuffer = nullptr;
    bool ready = false;
};

static FboApi& fboApi() {
    static FboApi a;
    static bool probed = false;
    if (!probed) {
        probed = true;
        #define HT2_FBO(FIELD, NAME) a.FIELD = (decltype(a.FIELD))SDL_GL_GetProcAddress(NAME);
        HT2_FBO(genFramebuffers, "glGenFramebuffers")
        HT2_FBO(deleteFramebuffers, "glDeleteFramebuffers")
        HT2_FBO(bindFramebuffer, "glBindFramebuffer")
        HT2_FBO(framebufferTexture2D, "glFramebufferTexture2D")
        HT2_FBO(checkFramebufferStatus, "glCheckFramebufferStatus")
        HT2_FBO(genRenderbuffers, "glGenRenderbuffers")
        HT2_FBO(deleteRenderbuffers, "glDeleteRenderbuffers")
        HT2_FBO(bindRenderbuffer, "glBindRenderbuffer")
        HT2_FBO(renderbufferStorage, "glRenderbufferStorage")
        HT2_FBO(framebufferRenderbuffer, "glFramebufferRenderbuffer")
        #undef HT2_FBO
        a.ready = a.genFramebuffers && a.deleteFramebuffers && a.bindFramebuffer &&
                  a.framebufferTexture2D && a.checkFramebufferStatus &&
                  a.genRenderbuffers && a.deleteRenderbuffers && a.bindRenderbuffer &&
                  a.renderbufferStorage && a.framebufferRenderbuffer;
        if (!a.ready) LOG("gl: framebuffer objects unavailable, render targets disabled");
    }
    return a;
}

static PFN_TexParameterf    s_texParameterf = nullptr;
static PFN_GetFloatv        s_getFloatv = nullptr;
static PFN_GetTexParameterfv s_getTexParameterfv = nullptr;
static PFN_GetTexParameteriv s_getTexParameteriv = nullptr;
static PFN_GetTexLevelParameteriv s_getTexLevelParameteriv = nullptr;

// The cap the task (and common practice) asks for: more than 8:1 buys almost
// nothing on a road surface but costs samples on every grazing-angle texel.
static const float kAnisoCap = 8.0f;

static const char* VSH = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec3 aNormal;
layout(location=3) in vec3 aColor;
uniform mat4 uProj;
uniform mat4 uView;
uniform mat4 uModel;
uniform float uFogDist;
out vec2 vUV;
out vec3 vColor;
out vec3 vNormal;
out float vFog;
void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vec4 vp = uView * wp;
    gl_Position = uProj * vp;
    vUV = aUV;
    vColor = aColor;
    vNormal = mat3(uModel) * aNormal;
    float d = length(vp.xyz);
    vFog = clamp(pow(clamp(d / uFogDist, 0.0, 1.0), 1.8), 0.0, 1.0);
}
)";

static const char* FSH = R"(
#version 330 core
in vec2 vUV;
in vec3 vColor;
in vec3 vNormal;
in float vFog;
uniform vec3 uSun;
uniform vec3 uSky;
uniform vec3 uFogColor;
uniform float uAmbient;
uniform float uSunAmt;
uniform sampler2D uTex;
uniform float uUseTex;
out vec4 frag;
void main() {
    vec4 texel = texture(uTex, vUV);
    // Sprites (foliage, lamp glows, light cones) carry their cut-out in the
    // texture alpha; without the test their transparent parts render as solid
    // black quads.
    if (uUseTex > 0.5 && texel.a < 0.5) discard;
    float diff = max(dot(normalize(vNormal), uSun), 0.0);
    float lum = uAmbient + (1.0 - uAmbient) * 0.9 * uSunAmt * diff;
    vec3 base = mix(vColor, texel.rgb * vColor, uUseTex);
    vec3 c = mix(base * lum, uFogColor, vFog);
    frag = vec4(c, 1.0);
}
)";

static const char* VSH2 = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform vec2 uScale;   // 2/width, 2/height
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPos.x * uScale.x - 1.0, 1.0 - aPos.y * uScale.y, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

static const char* FSH2 = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 frag;
void main() {
    frag = texture(uTex, vUV) * vColor;
}
)";

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

static void sdlDie(const char* what) {
    LOG("%s failed: %s", what, SDL_GetError());
}

// --- material palette ------------------------------------------------------
// The original uses textures (txr) per material; until the .txr decoder lands we
// shade by material family. Deterministic, so screenshots are reproducible.
static bool has(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}
static void materialColor(const std::string& mat, float rgb[3]) {
    std::string s = mat;
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    auto set = [&](float r, float g, float b) { rgb[0] = r; rgb[1] = g; rgb[2] = b; };
    if (s.empty())                                   { set(0.60f, 0.60f, 0.58f); return; }
    // traffic lights: the *_on/*_off materials are the lamp colours themselves
    if (s == "mat_tl_ron" || s == "_tl_ron")         { set(1.00f, 0.15f, 0.10f); return; }
    if (s == "mat_tl_yon" || s == "_tl_yon")         { set(1.00f, 0.80f, 0.15f); return; }
    if (s == "mat_tl_gon" || s == "_tl_gon")         { set(0.20f, 1.00f, 0.25f); return; }
    if (s == "mat_tl_roff" || s == "_tl_roff")       { set(0.28f, 0.06f, 0.05f); return; }
    if (s == "mat_tl_yoff" || s == "_tl_yoff")       { set(0.30f, 0.24f, 0.06f); return; }
    if (s == "mat_tl_goff" || s == "_tl_goff")       { set(0.06f, 0.28f, 0.08f); return; }
    if (s == "mat_tl_s" || s == "mat_tl_b")          { set(0.10f, 0.10f, 0.11f); return; }
    if (s.rfind("mat_tl", 0) == 0)                   { set(0.12f, 0.12f, 0.13f); return; }
    if (has(s, "svet") || has(s, "light") || has(s, "lamp")) { set(0.95f, 0.92f, 0.70f); return; }
    if (has(s, "post") || has(s, "stolb") || has(s, "opora")) { set(0.55f, 0.55f, 0.57f); return; }
    if (has(s, "forest") || has(s, "tree"))          { set(0.22f, 0.36f, 0.18f); return; }
    if (has(s, "water"))                             { set(0.16f, 0.34f, 0.52f); return; }
    if (has(s, "sky"))                               { set(0.62f, 0.75f, 0.92f); return; }
    if (has(s, "grass") || has(s, "forest") || has(s, "tree"))
                                                     { set(0.24f, 0.42f, 0.20f); return; }
    if (has(s, "gravii"))                            { set(0.58f, 0.55f, 0.47f); return; }
    if (has(s, "road") || has(s, "beton") || has(s, "wall") || has(s, "stolb"))
                                                     { set(0.42f, 0.42f, 0.44f); return; }
    if (has(s, "finish"))                            { set(0.85f, 0.85f, 0.85f); return; }
    if (has(s, "house"))                             { set(0.62f, 0.53f, 0.42f); return; }
    if (has(s, "br_"))                               { set(0.55f, 0.48f, 0.38f); return; }
    if (has(s, "tribune") || has(s, "opora") || has(s, "column") || has(s, "naves"))
                                                     { set(0.70f, 0.70f, 0.72f); return; }
    if (has(s, "red"))                               { set(0.62f, 0.26f, 0.22f); return; }
    if (has(s, "gre"))                               { set(0.26f, 0.50f, 0.28f); return; }
    if (has(s, "refl"))                              { set(0.55f, 0.62f, 0.70f); return; }
    if (has(s, "sphere") || has(s, "cayman"))        { set(0.50f, 0.58f, 0.62f); return; }
    if (has(s, "gate") || has(s, "zz") )             { set(0.72f, 0.66f, 0.45f); return; }
    if (has(s, "window") || has(s, "glass"))         { set(0.35f, 0.45f, 0.55f); return; }
    // stable fallback: hue from the name hash, muted
    uint32_t h = 2166136261u;
    for (unsigned char c : mat) { h ^= c; h *= 16777619u; }
    float hue = (float)(h % 360u) / 360.0f;
    float x = hue * 6.0f;
    int i = (int)x; float f = x - i;
    float q = 0.85f * (1 - 0.30f), p = 0.85f * (1 - 0.30f * f), t = 0.85f * (1 - 0.30f * (1 - f));
    float r = 0, g = 0, b = 0;
    switch (i % 6) {
        case 0: r = 0.85f; g = t; b = 0.85f * 0.70f; break;
        case 1: r = q; g = 0.85f; b = 0.85f * 0.70f; break;
        case 2: r = 0.85f * 0.70f; g = 0.85f; b = t; break;
        case 3: r = 0.85f * 0.70f; g = q; b = 0.85f; break;
        case 4: r = t; g = 0.85f * 0.70f; b = 0.85f; break;
        default: r = 0.85f; g = 0.85f * 0.70f; b = q; break;
    }
    set(r, g, b);
}

bool Renderer::init(int w, int h, bool offscreen_, bool fullscreen) {
    width_ = w; height_ = h; offscreen = offscreen_;
    renderW_ = w; renderH_ = h;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { sdlDie("SDL_Init"); return false; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    unsigned flags = 0;
    if (offscreen) flags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL;
    else if (fullscreen) flags |= SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP;
    else flags |= SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    window = SDL_CreateWindow("HT2:RE", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width_, height_, flags);
    if (!window) { sdlDie("SDL_CreateWindow"); return false; }
    ctx = SDL_GL_CreateContext(window);
    if (!ctx) { sdlDie("SDL_GL_CreateContext"); return false; }
    if (!gl.load()) return false;
    fboApi();                    // resolve the render-target entry points once

    // Identify the driver (goes into the log for bug reports) and decide the
    // anisotropic filtering level before the first texture exists.
    if (PFN_GetString getString = (PFN_GetString)SDL_GL_GetProcAddress("glGetString")) {
        auto str = [&](GLenum e) {
            const GLubyte* s = getString(e);
            return s ? std::string((const char*)s) : std::string("?");
        };
        vendor_ = str(GL_VENDOR);
        renderer_ = str(GL_RENDERER);
        version_ = str(GL_VERSION);
        LOG("gl: vendor '%s', renderer '%s', version '%s'",
            vendor_.c_str(), renderer_.c_str(), version_.c_str());
    }
    probeAnisotropy();

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glClearColor(skyR, skyG, skyB, 1.0f);
    glViewport(0, 0, width_, height_);
    if (!buildProgram()) return false;
    if (!buildProgram2D()) return false;
    {
        const uint8_t w1[4] = {255, 255, 255, 255};
        white = createTexture(w1, 1, 1);
    }
    if (!buildTerrainMesh()) return false;
    if (!buildSceneMesh()) return false;
    if (!buildVehicleMesh()) return false;
    glReady = true;
    return true;
}

void Renderer::shutdown() {
    if (texStats_.created && !statsLogged_) {
        statsLogged_ = true;
        LOG("gl: %s", texStats_.line().c_str());
    }
    if (ctx) {
        // Delete every render target this class handed out (the caller's copy of
        // the ids becomes stale, which bindRenderTarget() rejects).
        FboApi& a = fboApi();
        for (RenderTarget& t : targets_) {
            if (t.fbo && a.deleteFramebuffers) { GLuint f = t.fbo; a.deleteFramebuffers(1, &f); }
            if (t.color) { GLuint c = t.color; gl.glDeleteTextures(1, &c); }
            if (t.depth && a.deleteRenderbuffers) { GLuint d = t.depth; a.deleteRenderbuffers(1, &d); }
            t = RenderTarget();
        }
        targets_.clear();
        boundRT = nullptr;
        if (renderW_ != width_ || renderH_ != height_) { renderW_ = width_; renderH_ = height_; }
    }
    if (ctx) { SDL_GL_DeleteContext(ctx); ctx = nullptr; }
    if (window) { SDL_DestroyWindow(window); window = nullptr; }
    SDL_Quit();
}

bool Renderer::buildProgram() {
    auto compile = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = gl.glCreateShader(type);
        const GLchar* p = src;
        gl.glShaderSource(s, 1, &p, nullptr);
        gl.glCompileShader(s);
        GLint ok = 0;
        gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLchar buf[1024];
            gl.glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
            LOG("shader compile failed: %s", buf);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, VSH);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FSH);
    if (!vs || !fs) return false;
    prog = gl.glCreateProgram();
    gl.glAttachShader(prog, vs);
    gl.glAttachShader(prog, fs);
    gl.glLinkProgram(prog);
    GLint ok = 0;
    gl.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar buf[1024];
        gl.glGetProgramInfoLog(prog, sizeof buf, nullptr, buf);
        LOG("program link failed: %s", buf);
        return false;
    }
    uProj = gl.glGetUniformLocation(prog, "uProj");
    uView = gl.glGetUniformLocation(prog, "uView");
    uSun = gl.glGetUniformLocation(prog, "uSun");
    uFog = gl.glGetUniformLocation(prog, "uFogDist");
    uSky = gl.glGetUniformLocation(prog, "uSky");
    uFogColor = gl.glGetUniformLocation(prog, "uFogColor");
    uAmbient = gl.glGetUniformLocation(prog, "uAmbient");
    uSunAmt = gl.glGetUniformLocation(prog, "uSunAmt");
    uUseTex = gl.glGetUniformLocation(prog, "uUseTex");
    uTex = gl.glGetUniformLocation(prog, "uTex");
    uModel = gl.glGetUniformLocation(prog, "uModel");
    return true;
}

bool Renderer::buildProgram2D() {
    auto compile = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = gl.glCreateShader(type);
        const GLchar* p = src;
        gl.glShaderSource(s, 1, &p, nullptr);
        gl.glCompileShader(s);
        GLint ok = 0;
        gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLchar buf[1024];
            gl.glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
            LOG("2d shader compile failed: %s", buf);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, VSH2);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FSH2);
    if (!vs || !fs) return false;
    prog2 = gl.glCreateProgram();
    gl.glAttachShader(prog2, vs);
    gl.glAttachShader(prog2, fs);
    gl.glLinkProgram(prog2);
    GLint ok = 0;
    gl.glGetProgramiv(prog2, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar buf[1024];
        gl.glGetProgramInfoLog(prog2, sizeof buf, nullptr, buf);
        LOG("2d program link failed: %s", buf);
        return false;
    }
    uOrtho2 = gl.glGetUniformLocation(prog2, "uScale");
    uTex2 = gl.glGetUniformLocation(prog2, "uTex");
    gl.glGenVertexArrays(1, &vao2);
    gl.glBindVertexArray(vao2);
    gl.glGenBuffers(1, &vbo2);
    gl.glBindBuffer(GL_ARRAY_BUFFER, vbo2);
    gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)offsetof(SpriteVert, x));
    gl.glEnableVertexAttribArray(0);
    gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)offsetof(SpriteVert, u));
    gl.glEnableVertexAttribArray(1);
    gl.glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), (void*)offsetof(SpriteVert, r));
    gl.glEnableVertexAttribArray(2);
    return true;
}

void Renderer::setScene(const B3D* s, const SceneTextures* tex) {
    scene = s;
    sceneMeshes = s ? &s->submeshes() : nullptr;
    sceneTex = tex;
    if (glReady) buildSceneMesh();
}

void Renderer::setSceneMeshes(const std::vector<B3D::SubMesh>* m, const SceneTextures* tex) {
    scene = nullptr;
    sceneMeshes = m;
    sceneTex = tex;
    if (glReady) buildSceneMesh();
}

void Renderer::upload3D(std::vector<TVert>& verts, std::vector<unsigned>& idx,
                        unsigned& outVao, unsigned& outVbo, unsigned& outEbo, unsigned& count) {
    if (outVao == 0) gl.glGenVertexArrays(1, &outVao);
    gl.glBindVertexArray(outVao);
    if (outVbo == 0) gl.glGenBuffers(1, &outVbo);
    gl.glBindBuffer(GL_ARRAY_BUFFER, outVbo);
    gl.glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(TVert), verts.data(), GL_STATIC_DRAW);
    if (outEbo == 0) gl.glGenBuffers(1, &outEbo);
    gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, outEbo);
    gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned), idx.data(), GL_STATIC_DRAW);
    gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TVert), (void*)offsetof(TVert, x));
    gl.glEnableVertexAttribArray(0);
    gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TVert), (void*)offsetof(TVert, u));
    gl.glEnableVertexAttribArray(1);
    gl.glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(TVert), (void*)offsetof(TVert, nx));
    gl.glEnableVertexAttribArray(2);
    gl.glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(TVert), (void*)offsetof(TVert, r));
    gl.glEnableVertexAttribArray(3);
    count = (unsigned)idx.size();
}

// The visible ground.
//
// The heightmap alone is a *coarse* (415 units/sample) approximation of the
// natural terrain and, on top of that, it has nothing to do with the tiles: it
// locally sits 20..40 units above the roads.  The old mesh clamped each of its
// 415-unit vertices to `min(tileGround) - 2`, which bounds the surface AT the
// vertices but not between them - the interpolated triangle rose back above the
// road within one cell, so on every bend a green wedge swallowed the asphalt
// (docs/terrain_road.md has the before/after frames).
//
// The mesh is therefore built on a *fine* grid over the mapped world:
//
//     y = min( rawHeightmap(x,z), ceiling(x,z) - margin(vertex) )
//
// `ceiling` is GroundMap's tile-surface field: the lowest tile triangle over
// each 12-unit cell (a kilometre-wide apron quad counts over its whole area,
// not just its four corners), dilated by GroundMap::CLEARANCE_RADIUS cells and
// then raised by GroundMap::CEILING_SLOPE per unit of distance from the tiles.
// The rising ceiling is what keeps the clamp a *cut slope* instead of a flat
// shelf with a hard straight edge, and it also means the clamp stops applying a
// few hundred units away from the nearest road or building.
//
// `margin` is slope-adaptive: the flat part absorbs the bilinear gap between
// the 16-unit mesh grid and the 12-unit clearance grid, and the slope part
// (`|dy/dx| + |dy/dz|` times the mesh cell) guarantees that a triangle spanning
// a slope cannot lift its middle above the surface it was clamped to.
static constexpr float kGroundCell = 16.0f;
static constexpr float kGroundMarginFlat = 1.0f;
static constexpr float kGroundSkirt = 1600.0f;   // drawn beyond the tiles

// The margin the generated ground keeps between the tile surface and itself, as
// a function of `dist` = distance to the nearest covered tile cell (metres).
//
// `kGroundMarginFlat` (1 unit) is only what the 16-unit mesh grid needs next to
// a *flat* road.  On a slope the road profile changes by |dy/dx| * 16 inside one
// mesh cell, and the mesh's triangle chord would rise above the asphalt in the
// middle of the cell - hence the slope-adaptive part, which is added per vertex
// from the pass-1 gradient in buildFineGroundMesh() and is therefore *not* part
// of this function.
//
// `kGroundClearance` is the second half of the road-shaking fix: inside 25 m of
// the tiles the ground sheet keeps 2.5 extra units below the tile surface (over
// the metre the interpolation bound needs), fading out over the next 75 m.  The
// measured gap under asphalt goes from 1.0 m to 3.5 m (median 8.8 m, unchanged -
// the terrain is only clamped where it was already close), which is at least 6
// orders of magnitude coarser than the depth buffer can resolve at the draw
// distance.  The depth-space half of the fix is the polygon offset on the ground
// pass in draw().
static constexpr float kGroundClearance = 2.5f;
static constexpr float kGroundClearanceFade = 100.0f;
static constexpr float kGroundClearanceNear = 25.0f;

float Renderer::profileMargin(float dist) {
    if (!(dist > 0.0f)) return kGroundMarginFlat + kGroundClearance;
    if (dist >= kGroundClearanceFade) return kGroundMarginFlat;
    float t = dist / kGroundClearanceFade;
    t = t * t * (3.0f - 2.0f * t);                       // smoothstep
    return kGroundMarginFlat + kGroundClearance * (1.0f - t);
}

// Pass 1 of the mesh: the raw heightmap clamped under the tile ceiling.
static float groundProfile(const GroundMap* g, const Terrain* t, float x, float z) {
    float y = t->heightAtWorld(x, z, 0.0f);
    float near = 0.0f, dist = 0.0f;
    if (g->ceilingAt(x, z, near, dist)) {
        float s = dist / GroundMap::CEILING_FADE;
        if (s > 1.0f) s = 1.0f;
        s = s * s * (3.0f - 2.0f * s);                   // smoothstep
        float limit = near + (y - near) * s;             // = the raw terrain at the fade end
        float margin = Renderer::profileMargin(dist);
        if (y > limit - margin) y = limit - margin;
    }
    return y;
}

bool Renderer::groundHeightAt(float x, float z, float& outY) const {
    if (!terrain || !terrain->loaded() || !groundEnabled) return false;
    if (useRawTerrain || !groundMap || groundMap->empty() || !groundMap->hasClearance()) {
        // The raw 415-unit mesh, exactly as buildRawTerrainMesh() indexes it.
        const float cell = terrain->sampleStep;
        const float x0 = terrain->sampleX(0), z0 = terrain->sampleZ(Terrain::GRID - 1);
        float fi = (x - x0) / cell, fj = (z - z0) / cell;
        if (fi < 0.0f || fj < 0.0f) return false;
        int i = (int)fi, j = (int)fj;
        if (i + 1 >= Terrain::GRID || j + 1 >= Terrain::GRID) return false;
        const float tx = fi - i, tz = fj - j;
        auto at = [&](int a, int b) {
            return terrain->worldY(terrain->h[(size_t)b * Terrain::GRID + a]);
        };
        float v00 = at(i, j), v10 = at(i + 1, j), v01 = at(i, j + 1), v11 = at(i + 1, j + 1);
        // indices (a, c=a+G, b=a+1, d=c+1): (i,j), (i,j+1), (i+1,j), (i+1,j+1)
        outY = (tx + tz <= 1.0f) ? v00 + (v10 - v00) * tx + (v01 - v00) * tz
                                 : v11 + (v01 - v11) * (1.0f - tx) + (v10 - v11) * (1.0f - tz);
        return true;
    }
    const float cell = kGroundCell;
    const float x0 = groundMap->minX() - kGroundSkirt;
    const float z0 = groundMap->minZ() - kGroundSkirt;
    const int NX = (int)((groundMap->maxX() + kGroundSkirt - x0) / cell) + 1;
    const int NZ = (int)((groundMap->maxZ() + kGroundSkirt - z0) / cell) + 1;
    float fi = (x - x0) / cell, fj = (z - z0) / cell;
    if (fi < 0.0f || fj < 0.0f) return false;
    int i = (int)fi, j = (int)fj;
    if (i + 1 >= NX || j + 1 >= NZ) return false;
    const float tx = fi - i, tz = fj - j;
    // Every interior vertex carries the slope-adaptive part of the margin, which
    // needs the pass-1 gradient; the border ring does not (pass 2 only walks the
    // interior of the mesh), so it has to be mirrored here.
    auto vertY = [&](int a, int b) -> float {
        float y = groundProfile(groundMap, terrain, x0 + a * cell, z0 + b * cell);
        if (a == 0 || b == 0 || a == NX - 1 || b == NZ - 1) return y;
        float gx = (groundProfile(groundMap, terrain, x0 + (a + 1) * cell, z0 + b * cell) -
                    groundProfile(groundMap, terrain, x0 + (a - 1) * cell, z0 + b * cell)) / (2 * cell);
        float gz = (groundProfile(groundMap, terrain, x0 + a * cell, z0 + (b + 1) * cell) -
                    groundProfile(groundMap, terrain, x0 + a * cell, z0 + (b - 1) * cell)) / (2 * cell);
        float sm = (std::fabs(gx) + std::fabs(gz)) * cell;
        float cap = 4.0f * cell;
        if (sm > cap) sm = cap;
        float near = 0.0f, dist = 0.0f;
        if (groundMap->ceilingAt(x0 + a * cell, z0 + b * cell, near, dist) && y > near - sm)
            y = near - sm;
        return y;
    };
    // Cell (i, j): idx = (a, b, a+NX) then (b, d, a+NX), i.e. the diagonal runs
    // from (i+1, j) to (i, j+1); tx + tz <= 1 is the lower-left triangle.
    float v00 = vertY(i, j), v10 = vertY(i + 1, j), v01 = vertY(i, j + 1), v11 = vertY(i + 1, j + 1);
    outY = (tx + tz <= 1.0f) ? v00 + (v10 - v00) * tx + (v01 - v00) * tz
                             : v11 + (v01 - v11) * (1.0f - tx) + (v10 - v11) * (1.0f - tz);
    return true;
}

float Renderer::groundCellSize() const {
    return (useRawTerrain || !groundMap || !groundMap->hasClearance())
               ? (terrain ? terrain->sampleStep : kGroundCell)
               : kGroundCell;
}

bool Renderer::buildTerrainMesh() {
    if (!groundEnabled) { LOG("ground: disabled"); triCount = 0; return true; }
    if (!terrain || !terrain->loaded()) return true;
    // --terrain forces the raw 415-unit heightmap (the pre-fix mesh): it is the
    // "before" side of the road-continuity measurement in docs/terrain_road.md.
    if (useRawTerrain) { LOG("ground: --terrain (raw heightmap, unclamped)"); return buildRawTerrainMesh(); }
    if (groundMap && !groundMap->empty() && groundMap->hasClearance())
        return buildFineGroundMesh();
    return buildRawTerrainMesh();
}

// The generated ground of the mapped world (see the comment above).
bool Renderer::buildFineGroundMesh() {
    const GroundMap* g = groundMap;
    const float cell = kGroundCell;
    float x0 = g->minX() - kGroundSkirt, x1 = g->maxX() + kGroundSkirt;
    float z0 = g->minZ() - kGroundSkirt, z1 = g->maxZ() + kGroundSkirt;
    int NX = (int)((x1 - x0) / cell) + 1;
    int NZ = (int)((z1 - z0) / cell) + 1;
    std::vector<TVert> verts;
    verts.reserve((size_t)NX * NZ);
    // Pass 1: the clamp.  The ceiling is kept per vertex so pass 2 can add the
    // slope term without querying the grid again.
    std::vector<float> ceil((size_t)NX * NZ, 0.0f);
    std::vector<float> base;            // pass-1 heights: the gradient source
    base.reserve((size_t)NX * NZ);
    size_t clamped = 0;
    for (int j = 0; j < NZ; ++j)
        for (int i = 0; i < NX; ++i) {
            TVert v;
            v.x = x0 + i * cell;
            v.z = z0 + j * cell;
            float y = groundProfile(g, terrain, v.x, v.z);
            float near = 0.0f, dist = 0.0f;
            bool have = g->ceilingAt(v.x, v.z, near, dist);
            if (have && y < terrain->heightAtWorld(v.x, v.z, 0.0f)) ++clamped;
            ceil[(size_t)j * NX + i] = have ? near : std::numeric_limits<float>::quiet_NaN();
            base.push_back(y);
            v.y = y;
            v.u = v.x * 0.125f; v.v = v.z * 0.125f;    // world-space tiling
            v.r = 0.62f; v.g = 0.70f; v.b = 0.52f;     // tinted by the ground texture
            v.nx = 0; v.ny = 1; v.nz = 0;
            verts.push_back(v);
        }
    // Pass 2: normals, then the slope-adaptive part of the margin.  Both the
    // gradient and the margin come from the *pass-1* heights: reading them back
    // from the vertices as they are lowered would feed the clamp into its own
    // slope estimate and run away.
    const float smCap = 4.0f * cell;    // 4:1 is already a cliff
    for (int j = 1; j < NZ - 1; ++j)
        for (int i = 1; i < NX - 1; ++i) {
            size_t k = (size_t)j * NX + i;
            TVert* v = &verts[k];
            float dx = (base[k + 1] - base[k - 1]) / (2 * cell);
            float dz = (base[k + NX] - base[k - NX]) / (2 * cell);
            float sm = (std::fabs(dx) + std::fabs(dz)) * cell;
            if (sm > smCap) sm = smCap;
            float c = ceil[k];
            if (!std::isnan(c) && v->y > c - sm) { v->y = c - sm; ++clamped; }
            v->nx = -dx; v->ny = 1; v->nz = -dz;
            float l = sqrtf(v->nx * v->nx + v->ny * v->ny + v->nz * v->nz);
            v->nx /= l; v->ny /= l; v->nz /= l;
        }
    float ymin = 1e30f, ymax = -1e30f;
    for (const TVert& v : verts) { ymin = std::min(ymin, v.y); ymax = std::max(ymax, v.y); }
    std::vector<unsigned> idx;
    idx.reserve((size_t)(NX - 1) * (NZ - 1) * 6);
    for (int j = 0; j + 1 < NZ; ++j)
        for (int i = 0; i + 1 < NX; ++i) {
            unsigned a = j * NX + i, b = a + 1, c = a + NX, d = c + 1;
            // x grows with i and z grows with j -> CCW seen from above is (a,b,c)
            idx.push_back(a); idx.push_back(b); idx.push_back(c);
            idx.push_back(b); idx.push_back(d); idx.push_back(c);
        }
    // The shipped ENV/*.b3d tiles already contain the complete textured road
    // surface.  The old fallback ribbon duplicated it at heights sampled from
    // the smoothed GroundMap; on dips and bends that ribbon rose through the
    // authored asphalt and produced the torn/stacked road strips visible from
    // the cockpit.  Keep the generated mesh strictly terrain-only.
    groundTris = (unsigned)idx.size() / 3;
    upload3D(verts, idx, vao, vbo, ebo, triCount);
    if (getenv("HT2_GROUND_DEBUG")) {
        const float px[5] = {4700, 4750, -3020, -2327, -1740};
        const float pz[5] = {-5700, -5670, -4625, -3767, 3960};
        for (int k = 0; k < 5; ++k) {
            float raw = terrain->heightAtWorld(px[k], pz[k], 0.0f);
            float near = 0.0f, dist = 0.0f;
            bool have = g->ceilingAt(px[k], pz[k], near, dist);
            LOG("ground-debug: (%.0f,%.0f) raw %.1f near %.1f dist %.1f (have=%d)",
                px[k], pz[k], raw, near, dist, have ? 1 : 0);
        }
    }
    LOG("ground mesh: %dx%d cells of %.0f units, %u tris (%zu vertices clamped %.1f..%.1f), "
        "x %.0f..%.0f z %.0f..%.0f, clearance radius %d cells",
        NX, NZ, cell, triCount / 3, clamped, ymin, ymax, x0, x1, z0, z1,
        GroundMap::CLEARANCE_RADIUS);
    return true;
}

// Fallback: the raw heightmap, clamped under the tile footprint (only used when
// no ground map was built at all, e.g. --no-ground / a missing .way network).
bool Renderer::buildRawTerrainMesh() {
    const int G = Terrain::GRID;
    const int C = Terrain::CELLS;
    using V = TVert;
    std::vector<V> verts;
    verts.reserve((size_t)G * G);
    const float hmin = terrain->hmin, hmax = terrain->hmax;
    const float span = (hmax - hmin) > 0 ? hmax - hmin : 1.0f;

    std::vector<float> tileMin((size_t)G * G, std::numeric_limits<float>::quiet_NaN());
    int clamped = 0;
    // --terrain is the *unclamped* pre-fix mesh (the control of the road
    // continuity measurement): it must not consult the ground map at all, or a
    // fix to the ground profile would silently repair the control as well.
    if (!useRawTerrain && groundMap && !groundMap->empty()) {
        const int gw = groundMap->width(), gh = groundMap->height();
        const float gcell = groundMap->cellSize(), gx0 = groundMap->minX(), gz0 = groundMap->minZ();
        const std::vector<float>& cells = groundMap->cells();
        for (int j = 0; j < gh; ++j)
            for (int i = 0; i < gw; ++i) {
                float h = cells[(size_t)j * gw + i];
                if (std::isnan(h)) continue;
                float wx = gx0 + i * gcell, wz = gz0 + j * gcell;
                int ti = (int)lroundf(wx / terrain->sampleStep + terrain->centreI);
                int tj = (int)lroundf(-wz / terrain->sampleStep + terrain->centreJ);
                if (ti < 0 || tj < 0 || ti >= G || tj >= G) continue;
                float& c = tileMin[(size_t)tj * G + ti];
                if (std::isnan(c) || h < c) c = h;
            }
        // grow the covered area by two samples so tile borders are safe too
        std::vector<float> tmp = tileMin;
        for (int pass = 0; pass < 2; ++pass) {
            for (int j = 0; j < G; ++j)
                for (int i = 0; i < G; ++i) {
                    float m = tileMin[(size_t)j * G + i];
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di) {
                            int ni = i + di, nj = j + dj;
                            if (ni < 0 || nj < 0 || ni >= G || nj >= G) continue;
                            float v = tileMin[(size_t)nj * G + ni];
                            if (!std::isnan(v) && (std::isnan(m) || v < m)) m = v;
                        }
                    tmp[(size_t)j * G + i] = m;
                }
            tileMin.swap(tmp);
        }
    }

    for (int j = 0; j < G; ++j)
        for (int i = 0; i < G; ++i) {
            V v;
            v.x = terrain->sampleX(i);
            v.z = terrain->sampleZ(j);
            float hh = terrain->h[j * G + i];
            v.y = terrain->worldY(hh);
            float tm = tileMin[(size_t)j * G + i];
            if (!std::isnan(tm) && v.y > tm - 2.0f) { v.y = tm - 2.0f; ++clamped; }
            float t = (hh - hmin) / span;
            v.r = 0.18f + 0.34f * t;
            v.g = 0.40f - 0.08f * t;
            v.b = 0.16f + 0.16f * t;
            v.nx = 0; v.ny = 1; v.nz = 0;
            v.u = v.x * 0.125f; v.v = v.z * 0.125f;      // world-space tiling
            verts.push_back(v);
        }
    for (int j = 1; j < G - 1; ++j)
        for (int i = 1; i < G - 1; ++i) {
            V* v = &verts[(size_t)j * G + i];
            float dx = (verts[(size_t)j * G + i + 1].y - verts[(size_t)j * G + i - 1].y) /
                       (2 * terrain->sampleStep);
            float dz = (verts[(size_t)(j + 1) * G + i].y - verts[(size_t)(j - 1) * G + i].y) /
                       (2 * terrain->sampleStep);
            v->nx = -dx; v->ny = 1; v->nz = -dz;
            float l = sqrtf(v->nx * v->nx + v->ny * v->ny + v->nz * v->nz);
            v->nx /= l; v->ny /= l; v->nz /= l;
        }
    std::vector<unsigned> idx;
    idx.reserve((size_t)C * C * 6);
    for (int j = 0; j < C; ++j)
        for (int i = 0; i < C; ++i) {
            unsigned a = j * G + i, b = a + 1, c2 = a + G, d = c2 + 1;
            idx.push_back(a); idx.push_back(c2); idx.push_back(b);
            idx.push_back(b); idx.push_back(c2); idx.push_back(d);
        }
    // Roads come from the authored B3D tiles; see buildFineGroundMesh().
    groundTris = (unsigned)idx.size() / 3;
    upload3D(verts, idx, vao, vbo, ebo, triCount);
    LOG("terrain mesh (raw heightmap, no ground map): %u tris, %d/%d samples clamped below "
        "the tile surface, x %.0f..%.0f z %.0f..%.0f",
        triCount / 3, clamped, G * G, terrain->sampleX(G - 1), terrain->sampleX(0),
        terrain->sampleZ(0), terrain->sampleZ(G - 1));
    return true;
}


bool Renderer::buildRoadMesh(std::vector<TVert>& verts, std::vector<unsigned>& idx) {
    if (!roads || !terrain) return true;
    const unsigned baseIndex = (unsigned)verts.size();
    const float halfW = 15.0f;
    unsigned added = 0;
    for (const auto& sg : roads->segments()) {
        for (size_t k = 0; k + 1 < sg.pts.size(); ++k) {
            const Way::Pos& p0 = sg.pts[k];
            const Way::Pos& p1 = sg.pts[k + 1];
            float dx = (float)(p1.x - p0.x), dz = (float)(p1.z - p0.z);
            float len = sqrtf(dx * dx + dz * dz);
            if (len < 1e-3f) continue;
            dx /= len; dz /= len;
            float px = -dz * halfW, pz = dx * halfW;
            TVert c[4];
            const Way::Pos* q[2] = {&p0, &p1};
            for (int e = 0; e < 2; ++e) {
                const Way::Pos& pt = *q[e];
                // The road network carries its own heights (.way VDAT v2).  The
                // shipped terrain*.raw does not match them (r~0.3), so prefer the
                // ground map derived from the world geometry.
                float y;
                if (groundMap && !groundMap->empty())
                    y = groundMap->heightAt((float)pt.x, (float)pt.z,
                                            (float)pt.y) + 1.5f;
                else {
                    y = terrain->heightAtWorld((float)pt.x, (float)pt.z, (float)pt.y) + 2.0f;
                }
                c[e*2 + 0].x = (float)pt.x - px; c[e*2 + 0].z = (float)pt.z - pz; c[e*2 + 0].y = y;
                c[e*2 + 1].x = (float)pt.x + px; c[e*2 + 1].z = (float)pt.z + pz; c[e*2 + 1].y = y;
                for (int t = 0; t < 2; ++t) {
                    TVert& r = c[e*2 + t];
                    r.r = 1.0f; r.g = 0.85f; r.b = 0.15f;
                    r.nx = 0; r.ny = 1; r.nz = 0;
                    r.u = (float)t; r.v = (float)e;
                }
            }
            unsigned b0 = baseIndex + added;
            verts.push_back(c[0]); verts.push_back(c[1]);
            verts.push_back(c[2]); verts.push_back(c[3]);
            idx.push_back(b0 + 0); idx.push_back(b0 + 1); idx.push_back(b0 + 2);
            idx.push_back(b0 + 1); idx.push_back(b0 + 3); idx.push_back(b0 + 2);
            added += 4;
        }
    }
    if (!added) { LOG("road mesh: empty"); return true; }
    LOG("road mesh: %d ribbon quads (%u tris)", added / 4, (unsigned)idx.size() / 3);
    return true;
}

// ---------------------------------------------------------------------------
// Moving objects (AI traffic): upload each distinct model once.
// ---------------------------------------------------------------------------
int Renderer::addTrafficModel(const std::vector<B3D::SubMesh>& meshes, const SceneTextures* tex) {
    if (!glReady || meshes.empty()) return -1;
    Batch b;
    if (!buildMeshFor(&meshes, tex, b.vao, b.vbo, b.ebo, b.tris, b.ranges)) return -1;
    if (!b.tris) return -1;
    // Local bounding radius: how far any range's sphere reaches from the model
    // origin.  Each instance is culled with this at its own world position.
    for (const Range& r : b.ranges) {
        if (r.radius < 0.0f) continue;
        float d = sqrtf(r.cx * r.cx + r.cy * r.cy + r.cz * r.cz) + r.radius;
        if (d > b.radius) b.radius = d;
    }
    batches.push_back(std::move(b));
    return (int)batches.size() - 1;
}

size_t Renderer::trafficTriangles() const {
    size_t n = 0;
    for (const auto& b : batches) n += b.tris / 3;
    return n;
}

void Renderer::beginTraffic(float cx, float cy, float cz, float yawDeg, float pitchDeg) {
    if (!prog) return;
    // A traffic instance is placed by its own model matrix, so its ranges live
    // in *model* space and must not go through the world-space range cull (that
    // tested them against the world camera and rejected every batch).
    // drawTrafficInstance() culls each instance at its real position instead.
    cullByRange = false;
    trafficDrawn_ = trafficCulled_ = 0;
    viewPos_[0] = cx; viewPos_[1] = cy; viewPos_[2] = cz;
    float yaw = yawDeg * 3.14159265f / 180.0f;
    float pitch = pitchDeg * 3.14159265f / 180.0f;
    float tx = cx + sinf(yaw) * cosf(pitch);
    float ty = cy + sinf(pitch);
    float tz = cz - cosf(yaw) * cosf(pitch);
    Mat4 proj = Mat4::perspective(60.0f, (float)renderW_ / (float)renderH_, nearPlane, farPlane());
    if (mirrorX) proj.m[0] = -proj.m[0];        // Direct3D -> OpenGL orientation
    Mat4 view = Mat4::lookAt(cx, cy, cz, tx, ty, tz);
    if (culling) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    gl.glUseProgram(prog);
    gl.glUniformMatrix4fv(uProj, 1, GL_FALSE, proj.m);
    gl.glUniformMatrix4fv(uView, 1, GL_FALSE, view.m);
    gl.glUniform3f(uSun, sunX, sunY, sunZ);
    gl.glUniform1f(uFog, fogDist);
    gl.glUniform3f(uSky, skyR, skyG, skyB);
    gl.glUniform3f(uFogColor, fogR, fogG, fogB);
    gl.glUniform1f(uAmbient, ambient);
    gl.glUniform1f(uSunAmt, sunAmount);
    // The cars sit on the asphalt. A small pull toward the camera keeps the
    // road texture from showing through the body and the wheels.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.5f, -6.0f);
}

void Renderer::endTraffic() {
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void Renderer::drawTrafficInstance(int model, const Mat4& m) {
    if (model < 0 || model >= (int)batches.size()) return;
    Batch& b = batches[(size_t)model];
    // View-distance cull for a *placed* instance: the model matrix translation
    // is the instance's world position, and b.radius is how far its geometry
    // reaches from there.
    const float dx = m.m[12] - viewPos_[0], dy = m.m[13] - viewPos_[1],
                dz = m.m[14] - viewPos_[2];
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d - b.radius > farPlane()) { ++trafficCulled_; return; }
    ++trafficDrawn_;
    gl.glUniformMatrix4fv(uModel, 1, GL_FALSE, m.m);
    drawRanges(b.vao, b.ranges);
}

bool Renderer::buildSceneMesh() {
    return buildMeshFor(sceneMeshes, sceneTex, svao, svbo, sebo, striCount, sceneRanges);
}

bool Renderer::buildVehicleMesh() {
    return buildMeshFor(vehicleMeshes, vehicleTex, vvao, vvbo, vvebo, vvTriCount, vehicleRanges);
}

bool Renderer::buildMeshFor(const std::vector<B3D::SubMesh>* meshes, const SceneTextures* stex,
                            unsigned& outVao, unsigned& outVbo, unsigned& outEbo,
                            unsigned& outCount, std::vector<Range>& outRanges) {
    const SceneTextures* sceneTex = stex;
    std::vector<Range>& sceneRanges = outRanges;
    unsigned& svao = outVao; unsigned& svbo = outVbo; unsigned& sebo = outEbo;
    unsigned& striCount = outCount;
    if (!meshes || meshes->empty()) return true;
    std::vector<TVert> verts;
    std::vector<unsigned> idx;
    sceneRanges.clear();
    // Batch submeshes by (spatial chunk, texture).  Batching by texture alone
    // would make every batch span the whole 10 km island, and a batch whose
    // bounding sphere covers the world can never be culled by the view
    // distance.  kChunk units is small enough that only a handful of chunks are
    // inside the fog at once, and large enough to keep the draw-call count of a
    // fully visible view in the same range as the texture-only batching it
    // replaces (366 batches before, docs/terrain_road.md has the numbers).
    static constexpr float kChunk = 1024.0f;
    std::vector<unsigned> groupTex;
    std::vector<std::vector<const B3D::SubMesh*>> groups;
    std::unordered_map<uint64_t, size_t> groupIndex;
    auto chunkOf = [](float v) { return (int64_t)std::floor(v / kChunk); };
    for (const auto& sm : *meshes) {
        unsigned tex = sceneTex ? sceneTex->textureFor(sm.material) : 0u;
        uint64_t key = ((uint64_t)(uint32_t)chunkOf(sm.center[0]) << 40) ^
                       ((uint64_t)(uint32_t)chunkOf(sm.center[1]) << 12) ^ (uint64_t)tex;
        auto it = groupIndex.find(key);
        size_t g;
        if (it == groupIndex.end()) {
            g = groups.size();
            groupIndex.emplace(key, g);
            groupTex.push_back(tex);
            groups.emplace_back();
        } else g = it->second;
        groups[g].push_back(&sm);
    }
    size_t textured = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        Range r;
        r.offset = (unsigned)idx.size();
        r.tex = groupTex[g];
        if (r.tex) ++textured;
        // Bounding sphere of the batch in world space (b3d (x, z, height) maps
        // to (x, y, z)), so the view-distance cull can reject the whole batch.
        {
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            for (const B3D::SubMesh* smp : groups[g]) {
                const B3D::SubMesh& sm = *smp;
                // b3d (x, z, height) -> world (x, y, z)
                const float c[3] = {sm.center[0], sm.center[2], sm.center[1]};
                for (int k = 0; k < 3; ++k) {
                    mn[k] = std::min(mn[k], c[k] - sm.radius);
                    mx[k] = std::max(mx[k], c[k] + sm.radius);
                }
            }
            r.cx = 0.5f * (mn[0] + mx[0]);
            r.cy = 0.5f * (mn[1] + mx[1]);
            r.cz = 0.5f * (mn[2] + mx[2]);
            float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
            r.radius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
        }
        for (const B3D::SubMesh* smp : groups[g]) {
            const B3D::SubMesh& sm = *smp;
            float col[3];
            materialColor(sm.material, col);
            if (r.tex) { col[0] = col[1] = col[2] = 1.0f; }   // texture = albedo
            unsigned base = (unsigned)verts.size();
            for (size_t i = 0; i + 2 < sm.pos.size(); i += 3) {
                TVert v;
                // b3d frame == world frame: (x, z, height) -> (x, y, z)
                v.x = sm.pos[i + 0];
                v.y = sm.pos[i + 2];
                v.z = sm.pos[i + 1];
                size_t uvi = i / 3 * 2;
                v.u = uvi + 1 < sm.uv.size() ? sm.uv[uvi + 0] : 0.0f;
                v.v = uvi + 1 < sm.uv.size() ? sm.uv[uvi + 1] : 0.0f;
                v.nx = sm.nrm[i + 0];
                v.ny = sm.nrm[i + 2];
                v.nz = sm.nrm[i + 1];
                v.r = col[0]; v.g = col[1]; v.b = col[2];
                verts.push_back(v);
            }
            for (uint32_t k : sm.idx) idx.push_back(base + k);
        }
        r.count = (unsigned)idx.size() - r.offset;
        sceneRanges.push_back(r);
    }
    if (idx.empty()) { LOG("b3d scene mesh: empty"); return true; }
    upload3D(verts, idx, svao, svbo, sebo, striCount);
    LOG("b3d scene mesh: %zu submeshes in %zu batches (%zu textured), %u tris",
        meshes->size(), sceneRanges.size(), textured, striCount / 3);
    return true;
}

void Renderer::drawVehicleScene(float cx, float cy, float cz, float yawDeg, float pitchDeg) {
    if (!vvTriCount) return;
    // The player's truck rides on the model matrix and is always at the camera,
    // so its local-space ranges must not be distance-culled either.
    cullByRange = false;
    viewPos_[0] = cx; viewPos_[1] = cy; viewPos_[2] = cz;
    float yaw = yawDeg * 3.14159265f / 180.0f;
    float pitch = pitchDeg * 3.14159265f / 180.0f;
    float tx = cx + sinf(yaw) * cosf(pitch);
    float ty = cy + sinf(pitch);
    float tz = cz - cosf(yaw) * cosf(pitch);
    Mat4 proj = Mat4::perspective(60.0f, (float)renderW_ / (float)renderH_, nearPlane, farPlane());
    if (mirrorX) proj.m[0] = -proj.m[0];        // Direct3D -> OpenGL orientation
    Mat4 view = Mat4::lookAt(cx, cy, cz, tx, ty, tz);
    // inside the cab the shell must be two-sided so the interior (dashboard,
    // pillars, wheel) is visible, like the original cockpit view
    if (culling && !vehicleTwoSided) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    gl.glUseProgram(prog);
    gl.glUniformMatrix4fv(uProj, 1, GL_FALSE, proj.m);
    gl.glUniformMatrix4fv(uView, 1, GL_FALSE, view.m);
    gl.glUniform3f(uSun, sunX, sunY, sunZ);
    gl.glUniform1f(uFog, fogDist);
    gl.glUniform3f(uSky, skyR, skyG, skyB);
    gl.glUniform3f(uFogColor, fogR, fogG, fogB);
    gl.glUniform1f(uAmbient, ambient);
    gl.glUniform1f(uSunAmt, sunAmount);
    gl.glUniformMatrix4fv(uModel, 1, GL_FALSE, model.m);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.5f, -6.0f);
    drawRanges(vvao, vehicleRanges);
    glDisable(GL_POLYGON_OFFSET_FILL);
    Mat4 ident;
    gl.glUniformMatrix4fv(uModel, 1, GL_FALSE, ident.m);
}

void Renderer::setModel(const Mat4& m) { model = m; }
void Renderer::clearModel() { model = Mat4(); }

void Renderer::setVehicleScene(const B3D* s, const SceneTextures* tex) {
    vehicle = s;
    vehicleMeshes = s ? &s->submeshes() : nullptr;
    vehicleTex = tex;
    if (glReady) buildVehicleMesh();
}

void Renderer::setVehicleMeshes(const std::vector<B3D::SubMesh>* m, const SceneTextures* tex) {
    vehicle = nullptr;
    vehicleMeshes = m;
    vehicleTex = tex;
    if (glReady) buildVehicleMesh();
}

void Renderer::clear() {
    // The clear colour stays the flat sky fallback; drawSky() paints the
    // gradient over it before the 3D passes.
    glClearColor(fogR, fogG, fogB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Two-stop sky.  The horizon stop is the fog colour, so distant geometry fades
// into the background with no visible seam; the zenith stop is the weather's
// sky colour.  The horizon line is placed with the camera pitch and the same
// 60-degree vertical FOV the 3D pass uses.
void Renderer::drawSky(float pitchDeg) {
    const float h = (float)renderH_, w = (float)renderW_;
    const float f = (h * 0.5f) / tanf(30.0f * 3.14159265f / 180.0f);
    float hy = h * 0.5f + f * tanf(pitchDeg * 3.14159265f / 180.0f);
    if (hy < 0.0f) hy = 0.0f;
    if (hy > h) hy = h;
    const float top[3] = {zenR, zenG, zenB};
    const float hor[3] = {fogR, fogG, fogB};
    // The zenith colour holds over most of the dome and blends into the fog
    // colour in the last fifth, so the horizon still matches the fog that
    // distant geometry fades into.
    float mid[3];
    for (int k = 0; k < 3; ++k) mid[k] = top[k] * 0.6f + hor[k] * 0.4f;
    begin2D();
    curTex2 = white;
    if (hy > 0.5f) {
        float split = hy * 0.80f;
        quad2DGradient(0.0f, 0.0f, w, split, top, mid, 1.0f);
        quad2DGradient(0.0f, split, w, hy - split, mid, hor, 1.0f);
    }
    quad2DGradient(0.0f, hy, w, h - hy > 0.5f ? h - hy : 0.5f, hor, hor, 1.0f);
    end2D();
}

void Renderer::quad2DGradient(float x, float y, float w, float h,
                              const float* cTop, const float* cBot, float a) {
    if (!sprites.empty() && curTex2 != white) flush2D();
    curTex2 = white;
    SpriteVert v[4];
    const float px[4] = {x, x + w, x + w, x};
    const float py[4] = {y, y, y + h, y + h};
    const float tu[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    const float tv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        const float* c = (i < 2) ? cTop : cBot;
        v[i].x = px[i]; v[i].y = py[i]; v[i].u = tu[i]; v[i].v = tv[i];
        v[i].r = c[0]; v[i].g = c[1]; v[i].b = c[2]; v[i].a = a;
    }
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) sprites.push_back(v[order[i]]);
}

void Renderer::drawRanges(unsigned targetVao, const std::vector<Range>& ranges) {
    // The shipped B3D geometry was authored for Direct3D (clockwise front faces
    // in a left-handed world).  Mirroring the projection flips the on-screen
    // winding, so the front face becomes counter-clockwise.
    glFrontFace(mirrorX ? GL_CCW : GL_CW);
    gl.glBindVertexArray(targetVao);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glUniform1i(uTex, 0);
    for (const Range& r : ranges) {
        if (!r.count) continue;
        if (cullByRange && r.radius >= 0.0f) {
            // Draw distance: everything whose nearest point is beyond the fog
            // end is invisible, so it is not submitted at all.
            const float dx = r.cx - viewPos_[0], dy = r.cy - viewPos_[1],
                        dz = r.cz - viewPos_[2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d - r.radius > farPlane()) { ++rangesCulled_; continue; }
        }
        ++rangesDrawn_;
        gl.glUniform1f(uUseTex, r.tex ? 1.0f : 0.0f);
        gl.glBindTexture(GL_TEXTURE_2D, r.tex ? r.tex : white);
        glDrawElements(GL_TRIANGLES, r.count, GL_UNSIGNED_INT,
                       (void*)(size_t)(r.offset * sizeof(unsigned)));
    }
}

void Renderer::draw(float cx, float cy, float cz, float yawDeg, float pitchDeg) {
    if (!triCount) return;
    viewPos_[0] = cx; viewPos_[1] = cy; viewPos_[2] = cz;
    cullByRange = false;
    float yaw = yawDeg * 3.14159265f / 180.0f;
    float pitch = pitchDeg * 3.14159265f / 180.0f;
    float tx = cx + sinf(yaw) * cosf(pitch);
    float ty = cy + sinf(pitch);
    float tz = cz - cosf(yaw) * cosf(pitch);

    Mat4 proj = Mat4::perspective(60.0f, (float)renderW_ / (float)renderH_, nearPlane, farPlane());
    if (mirrorX) proj.m[0] = -proj.m[0];        // Direct3D -> OpenGL orientation
    Mat4 view = Mat4::lookAt(cx, cy, cz, tx, ty, tz);

    if (culling) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glFrontFace(mirrorX ? GL_CW : GL_CCW);   // terrain/roads are authored CCW
    gl.glUseProgram(prog);
    gl.glUniformMatrix4fv(uProj, 1, GL_FALSE, proj.m);
    gl.glUniformMatrix4fv(uView, 1, GL_FALSE, view.m);
    gl.glUniform3f(uSun, sunX, sunY, sunZ);
    gl.glUniform1f(uFog, fogDist);
    gl.glUniform3f(uSky, skyR, skyG, skyB);
    gl.glUniform3f(uFogColor, fogR, fogG, fogB);
    gl.glUniform1f(uAmbient, ambient);
    gl.glUniform1f(uSunAmt, sunAmount);
    gl.glUniformMatrix4fv(uModel, 1, GL_FALSE, model.m);
    std::vector<Range> rs;
    unsigned t3 = triCount / 3;
    unsigned gt = (groundTex && groundTris) ? std::min(groundTris, t3) : 0;
    if (gt) {
        Range a; a.offset = 0; a.count = gt * 3; a.tex = groundTex;
        Range b; b.offset = gt * 3; b.count = triCount - gt * 3; b.tex = 0;
        rs.push_back(a);
        if (b.count) rs.push_back(b);
    } else {
        Range r; r.offset = 0; r.count = triCount; r.tex = 0;
        rs.push_back(r);
    }
    // The generated ground is a *terrain* sheet that runs under the authored
    // asphalt; the tiles are drawn afterwards (drawScene) and must always win the
    // depth test where the two are near each other, at every distance and camera
    // pitch.  A polygon offset on the ground pass (positive = away from the
    // camera) is the depth-space half of that; the geometric half is the
    // clearance groundProfile() keeps below the tile surface.
    // Positive offset pushes the terrain sheet away from the camera so the
    // tile asphalt, drawn next with no offset, wins even at a grazing angle.
    // A factor of 1 was not enough: the road still flickered as the cab moved.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(4.0f, 24.0f);
    drawRanges(vao, rs);
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void Renderer::clearDepth() {
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void Renderer::drawScene(float cx, float cy, float cz, float yawDeg, float pitchDeg) {
    if (!striCount) return;
    viewPos_[0] = cx; viewPos_[1] = cy; viewPos_[2] = cz;
    cullByRange = true;
    rangesCulled_ = 0; rangesDrawn_ = 0;
    float yaw = yawDeg * 3.14159265f / 180.0f;
    float pitch = pitchDeg * 3.14159265f / 180.0f;
    float tx = cx + sinf(yaw) * cosf(pitch);
    float ty = cy + sinf(pitch);
    float tz = cz - cosf(yaw) * cosf(pitch);

    Mat4 proj = Mat4::perspective(60.0f, (float)renderW_ / (float)renderH_, nearPlane, farPlane());
    if (mirrorX) proj.m[0] = -proj.m[0];        // Direct3D -> OpenGL orientation
    Mat4 view = Mat4::lookAt(cx, cy, cz, tx, ty, tz);

    if (culling) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    gl.glUseProgram(prog);
    gl.glUniformMatrix4fv(uProj, 1, GL_FALSE, proj.m);
    gl.glUniformMatrix4fv(uView, 1, GL_FALSE, view.m);
    gl.glUniform3f(uSun, sunX, sunY, sunZ);
    gl.glUniform1f(uFog, fogDist);
    gl.glUniform3f(uSky, skyR, skyG, skyB);
    gl.glUniform3f(uFogColor, fogR, fogG, fogB);
    gl.glUniform1f(uAmbient, ambient);
    gl.glUniform1f(uSunAmt, sunAmount);
    gl.glUniformMatrix4fv(uModel, 1, GL_FALSE, model.m);
    drawRanges(svao, sceneRanges);
}

// --- 2D overlay ------------------------------------------------------------
void Renderer::begin2D() {
    sprites.clear();
    in2D = true;
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Renderer::quad2D(unsigned tex, float x, float y, float w, float h,
                      float r, float g, float b, float a,
                      float u0, float v0, float u1, float v1) {
    if (!sprites.empty() && tex != curTex2) flush2D();
    curTex2 = tex;
    SpriteVert v[4];
    const float px[4] = {x, x + w, x + w, x};
    const float py[4] = {y, y, y + h, y + h};
    const float tu[4] = {u0, u1, u1, u0};
    const float tv[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; ++i) {
        v[i].x = px[i]; v[i].y = py[i];
        v[i].u = tu[i]; v[i].v = tv[i];
        v[i].r = r; v[i].g = g; v[i].b = b; v[i].a = a;
    }
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) sprites.push_back(v[order[i]]);
}

void Renderer::flush2D() {
    if (sprites.empty()) return;
    gl.glUseProgram(prog2);
    gl.glUniform2f(uOrtho2, 2.0f / (float)width_, 2.0f / (float)height_);
    gl.glUniform1i(uTex2, 0);
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, curTex2);
    gl.glBindVertexArray(vao2);
    gl.glBindBuffer(GL_ARRAY_BUFFER, vbo2);
    gl.glBufferData(GL_ARRAY_BUFFER, sprites.size() * sizeof(SpriteVert), sprites.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)sprites.size());
    sprites.clear();
}

void Renderer::end2D() {
    flush2D();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    in2D = false;
}

void Renderer::pushQuad2D(unsigned tex, const float* xy4, const float* uv4,
                          float r, float g, float b, float a) {
    if (!sprites.empty() && tex != curTex2) flush2D();
    curTex2 = tex;
    SpriteVert v[4];
    for (int i = 0; i < 4; ++i) {
        v[i].x = xy4[i * 2 + 0]; v[i].y = xy4[i * 2 + 1];
        v[i].u = uv4[i * 2 + 0]; v[i].v = uv4[i * 2 + 1];
        v[i].r = r; v[i].g = g; v[i].b = b; v[i].a = a;
    }
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) sprites.push_back(v[order[i]]);
}

void Renderer::line2D(unsigned tex, float x0, float y0, float x1, float y1, float thick,
                      float r, float g, float b, float a) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * thick * 0.5f, ny = dx / len * thick * 0.5f;
    const float xy[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
    const float uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    pushQuad2D(tex, xy, uv, r, g, b, a);
}

// Resolve the extension and choose the level.  GL_TEXTURE_MAX_ANISOTROPY_EXT is
// a *per-texture* sampler parameter, so it has to be set on every texture that
// can be seen at a grazing angle - applyAnisotropy() does that in createTexture().
bool Renderer::probeAnisotropy() {
    s_texParameterf = (PFN_TexParameterf)SDL_GL_GetProcAddress("glTexParameterf");
    s_getFloatv = (PFN_GetFloatv)SDL_GL_GetProcAddress("glGetFloatv");
    s_getTexParameterfv = (PFN_GetTexParameterfv)SDL_GL_GetProcAddress("glGetTexParameterfv");
    s_getTexParameteriv = (PFN_GetTexParameteriv)SDL_GL_GetProcAddress("glGetTexParameteriv");
    s_getTexLevelParameteriv =
        (PFN_GetTexLevelParameteriv)SDL_GL_GetProcAddress("glGetTexLevelParameteriv");

    const bool ext = SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") == SDL_TRUE;
    maxAniso_ = 1.0f;
    if (ext && s_getFloatv) {
        GLfloat m = 1.0f;
        s_getFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &m);
        if (m >= 2.0f && m <= 64.0f) maxAniso_ = m;      // ignore nonsense
    }
    if (ext && !s_texParameterf) maxAniso_ = 1.0f;       // cannot set it anyway

    float want = maxAniso_ > 1.0f ? std::min(kAnisoCap, maxAniso_) : 1.0f;
    if (const char* e = getenv("HT2_ANISO")) {           // measurement override
        const float v = (float)atof(e);
        want = (v <= 0.0f) ? 1.0f : std::min(v, maxAniso_);
    }
    aniso_ = want;

    // Drain any GL error the probe may have queued (an unsupported pname would
    // raise GL_INVALID_ENUM); the engine never reads the error queue itself, so
    // leaving one behind would only confuse a future debugging session.
    if (auto getError = (GLenum (*)())SDL_GL_GetProcAddress("glGetError")) {
        for (int i = 0; i < 4 && getError() != GL_NO_ERROR; ++i) {}
    }

    if (aniso_ > 1.0f)
        LOG("gl: anisotropic filtering ON at %.0fx (max supported %.0fx, "
            "GL_EXT_texture_filter_anisotropic present%s)",
            aniso_, maxAniso_, getenv("HT2_ANISO") ? ", HT2_ANISO override" : "");
    else if (!ext)
        LOG("gl: anisotropic filtering OFF (GL_EXT_texture_filter_anisotropic not advertised)");
    else if (!s_texParameterf)
        LOG("gl: anisotropic filtering OFF (glTexParameterf unavailable)");
    else
        LOG("gl: anisotropic filtering OFF (HT2_ANISO override; driver supports up to %.0fx)",
            maxAniso_);
    return aniso_ > 1.0f;
}

void Renderer::applyAnisotropy(unsigned tex) {
    if (aniso_ <= 1.0f || !s_texParameterf) return;
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    s_texParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso_);
    ++texStats_.aniso;
    if (getenv("HT2_TEX_DEBUG") && s_getTexParameterfv) {
        GLfloat got = 0.0f;
        s_getTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, &got);
        LOG("gl: texture %u anisotropy = %.0fx (requested %.0fx)", tex, got, aniso_);
    }
}

// Post-condition check for the "complete mip chain" requirement.  glGenerateMipmap
// produces levels 0..floor(log2(max(w,h))) ending in a 1x1 level; walk them with
// glGetTexLevelParameteriv (GL 1.1, resolved here) and confirm both.
bool Renderer::verifyMipChain(int w, int h) {
    if (!s_getTexLevelParameteriv) return false;
    int levels = 1, maxWH = w > h ? w : h;
    while (maxWH > 1) { maxWH >>= 1; ++levels; }
    GLint lw = 0, lh = 0;
    for (int l = 0; l < levels; ++l) {
        lw = lh = 0;
        s_getTexLevelParameteriv(GL_TEXTURE_2D, l, GL_TEXTURE_WIDTH, &lw);
        s_getTexLevelParameteriv(GL_TEXTURE_2D, l, GL_TEXTURE_HEIGHT, &lh);
        if (lw <= 0 || lh <= 0) return false;
    }
    return lw == 1 && lh == 1;
}

std::string Renderer::TexStats::line() const {
    char buf[192];
    snprintf(buf, sizeof buf,
             "%u textures: %u complete mip chains (to 1x1), %u with GL_LINEAR_MIPMAP_LINEAR, "
             "%u with anisotropic filtering", created, mipComplete, mipMinFilter, aniso);
    return std::string(buf);
}

unsigned Renderer::createTexture(const uint8_t* rgba, int w, int h) {
    GLuint t = 0;
    gl.glGenTextures(1, &t);
    gl.glBindTexture(GL_TEXTURE_2D, t);
    // The shipped meshes tile their textures (a road quad repeats its asphalt
    // 5x across 12 units), so wrapping must repeat - clamping stretched the
    // border texels across the whole polygon, which is what made road surfaces
    // look like they were "flowing".  Mipmaps remove the shimmer at distance.
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (gl.glGenerateMipmap) gl.glGenerateMipmap(GL_TEXTURE_2D);
    // Isotropic trilinear filtering must pick a single mip level for a footprint
    // that is a long thin (and, on a curve, rotating) parallelogram, so one axis
    // ends up over-blurred and the other under-filtered - the residual is what
    // crawls between frames.  Anisotropic filtering integrates along the major
    // axis instead: 41.6% less error against a 4x supersampled reference on the
    // road pixels of a grazing curve view, see docs/road_filtering.md.
    applyAnisotropy(t);

    // Book-keeping for the "every 3D texture has a complete mip chain and a
    // mipmapping min filter" invariant (reported in shutdown()/SceneTextures).
    ++texStats_.created;
    if (verifyMipChain(w, h)) ++texStats_.mipComplete;
    if (s_getTexParameteriv) {
        GLint f = 0;
        s_getTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &f);
        if (f == GL_LINEAR_MIPMAP_LINEAR) ++texStats_.mipMinFilter;
    }
    return t;
}

void Renderer::deleteTexture(unsigned& t) {
    if (t) { GLuint x = t; gl.glDeleteTextures(1, &x); t = 0; }
}

// Streaming upload for textures created with createTexture(): the video player
// pushes one frame per displayed frame into the same texture.  glTexSubImage2D
// is GL 1.1 but is not part of the explicitly loaded entry points, so it is
// resolved here; Video.cpp keeps its own fallback path for other callers.
bool Renderer::updateTexture(unsigned tex, const uint8_t* rgba, int w, int h) {
    if (!tex || !rgba || w <= 0 || h <= 0 || !glReady) return false;
    typedef void (*PFN_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei,
                                      GLenum, GLenum, const void*);
    static PFN_TexSubImage2D sub = nullptr;
    static bool probed = false;
    if (!probed) {
        probed = true;
        sub = (PFN_TexSubImage2D)SDL_GL_GetProcAddress("glTexSubImage2D");
    }
    if (!sub) return false;
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    sub(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    // createTexture() built the mip chain for *its* own frame only; a streamed
    // frame would otherwise blend with those stale levels when drawn smaller
    // than native, so the min filter drops to plain linear (the levels are
    // simply never sampled again).
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ++textureUpdates_;
    lastUpdatedTex_ = tex;
    return true;
}

// --- render targets (offscreen framebuffers) -------------------------------
bool Renderer::createRenderTarget(RenderTarget& rt, int w, int h) {
    destroyRenderTarget(rt);
    if (!glReady || w <= 0 || h <= 0) return false;
    FboApi& a = fboApi();
    if (!a.ready) return false;

    RenderTarget t;
    t.w = w; t.h = h;
    gl.glGenTextures(1, &t.color);
    gl.glBindTexture(GL_TEXTURE_2D, t.color);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // Deliberately no mipmaps: the target is blitted at (near) its own size and
    // is not part of the 3D texture/mip-chain invariant (texStats_).

    a.genRenderbuffers(1, &t.depth);
    a.bindRenderbuffer(GL_RENDERBUFFER, t.depth);
    a.renderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    a.genFramebuffers(1, &t.fbo);
    a.bindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    a.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
    a.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
    GLenum st = a.checkFramebufferStatus(GL_FRAMEBUFFER);
    a.bindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOG("gl: render target %dx%d incomplete (status 0x%x)", w, h, (unsigned)st);
        if (t.fbo) { GLuint f = t.fbo; a.deleteFramebuffers(1, &f); }
        if (t.color) { GLuint c = t.color; gl.glDeleteTextures(1, &c); }
        if (t.depth) { GLuint d = t.depth; a.deleteRenderbuffers(1, &d); }
        return false;
    }
    rt = t;
    targets_.push_back(t);
    return true;
}

void Renderer::destroyRenderTarget(RenderTarget& rt) {
    FboApi& a = fboApi();
    for (size_t i = 0; i < targets_.size(); ++i) {
        if (targets_[i].fbo == rt.fbo && rt.fbo) { targets_.erase(targets_.begin() + (long)i); break; }
    }
    if (boundRT && boundRT->fbo == rt.fbo) unbindRenderTarget();
    if (rt.fbo && a.deleteFramebuffers) { GLuint f = rt.fbo; a.deleteFramebuffers(1, &f); }
    if (rt.color) { GLuint c = rt.color; gl.glDeleteTextures(1, &c); }
    if (rt.depth && a.deleteRenderbuffers) { GLuint d = rt.depth; a.deleteRenderbuffers(1, &d); }
    rt = RenderTarget();
}

bool Renderer::bindRenderTarget(const RenderTarget& rt, bool clearColorAndDepth) {
    if (!glReady || !rt.live()) return false;
    bool known = false;
    for (const RenderTarget& t : targets_) if (t.fbo == rt.fbo) { known = true; break; }
    if (!known) return false;
    FboApi& a = fboApi();
    a.bindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glViewport(0, 0, rt.w, rt.h);
    renderW_ = rt.w; renderH_ = rt.h;
    boundRT = &rt;
    if (clearColorAndDepth) {
        glClearColor(skyR, skyG, skyB, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    return true;
}

void Renderer::unbindRenderTarget() {
    if (!boundRT) return;
    fboApi().bindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    renderW_ = width_; renderH_ = height_;
    boundRT = nullptr;
}

bool Renderer::screenshot(const std::string& path) {
    std::string base = path.substr(0, path.find_last_of('.'));
    std::string bmpPath = base + ".bmp";
    GLubyte* px = new GLubyte[width_ * height_ * 3];
    glReadPixels(0, 0, width_, height_, GL_RGB, GL_UNSIGNED_BYTE, px);
    // glReadPixels returns bottom-up rows; BMP expects top-down.
    {
        const size_t stride = (size_t)width_ * 3;
        std::vector<GLubyte> tmp(stride);
        for (int y = 0; y < height_ / 2; ++y) {
            GLubyte* a = px + (size_t)y * stride;
            GLubyte* b = px + (size_t)(height_ - 1 - y) * stride;
            memcpy(tmp.data(), a, stride);
            memcpy(a, b, stride);
            memcpy(b, tmp.data(), stride);
        }
    }
    SDL_Surface* s = SDL_CreateRGBSurfaceFrom(px, width_, height_, 24, width_ * 3,
                                              0x0000ff, 0x00ff00, 0xff0000, 0);
    int res = SDL_SaveBMP(s, bmpPath.c_str());
    SDL_FreeSurface(s);
    delete[] px;
    LOG("screenshot: %s (%s)", bmpPath.c_str(), res == 0 ? "ok" : "fail");
    return res == 0;
}

} // namespace ht2
