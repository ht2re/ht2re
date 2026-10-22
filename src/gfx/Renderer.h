#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "GLCore.h"
#include "Math3D.h"
#include "../world/B3D.h"

namespace ht2 {

class Terrain;
class Way;
class SceneTextures;
class GroundMap;

class Renderer {
public:
    // Shared 3D vertex layout: terrain grid, road ribbons and b3d scene meshes.
    struct TVert { float x, y, z, u, v, nx, ny, nz, r, g, b; };
    // `radius < 0` marks a range that must never be distance-culled (the
    // terrain/road ribbons, whose geometry is not a single object).
    struct Range {
        unsigned offset = 0, count = 0, tex = 0;
        float cx = 0, cy = 0, cz = 0, radius = -1.0f;
    };
    Renderer();
    ~Renderer();

    bool init(int w, int h, bool offscreen, bool fullscreen = false);
    // The truck's cab shell is single-sided; in the cockpit view the camera sits
    // inside it, so the interior must be drawn without back-face culling.
    void setVehicleTwoSided(bool b) { vehicleTwoSided = b; }
    void shutdown();

    void setTerrain(const Terrain* t) { terrain = t; }
    void setRoads(const Way* w) { roads = w; }
    void setGroundMap(const GroundMap* g) { groundMap = g; }
    void setGroundEnabled(bool b) { groundEnabled = b; }
    // ground texture + how many triangles at the start of the terrain VAO use it
    void setGroundTexture(unsigned t) { groundTex = t; }
    void setUseRawTerrain(bool b) { useRawTerrain = b; }

    // ---- generated-ground height query (measurement / diagnostics) --------
    // Exact height of the mesh buildFineGroundMesh() uploaded, evaluated with
    // the very same code path (raw heightmap + GroundMap ceiling + margin, and
    // the exact triangle split of every cell), so a probe can compare the drawn
    // ground against the road surface without ray-casting the GPU buffers.
    // Returns false when the point is outside the mesh.  `--terrain` makes it
    // answer for the raw 415-unit mesh instead (the measurement's control).
    bool groundHeightAt(float x, float z, float& outY) const;
    // Cell size of the generated ground mesh (16 units) / of the raw fallback.
    float groundCellSize() const;
    // Margin (in units) the clamp keeps between the tile surface and the ground
    // mesh, as a function of the distance to the nearest covered tile cell.
    static float profileMargin(float dist);
    // B3D scene (menu city / world tile). buildSceneMesh() is called by init()
    // and lazily by setScene(); keeps the GL objects in this class.
    void setScene(const B3D* s, const SceneTextures* tex = nullptr);
    // Whole-world variant: the caller assembles the submesh list itself (all
    // ENV/MTR tiles merged).
    void setSceneMeshes(const std::vector<B3D::SubMesh>* m, const SceneTextures* tex);
    // Second scene slot: the drivable vehicle (drawn with the model matrix).
    void setVehicleScene(const B3D* s, const SceneTextures* tex = nullptr);
    void setVehicleMeshes(const std::vector<B3D::SubMesh>* m, const SceneTextures* tex);
    void drawVehicleScene(float camX, float camY, float camZ, float yawDeg, float pitchDeg);

    // ---- moving objects (AI traffic) ------------------------------------
    // A model is uploaded once and then drawn many times with its own model
    // matrix, so a whole traffic population costs a handful of vertex buffers.
    // `radius` is the model's own bounding radius (the ranges are in model
    // local space), so a caller can cull a *placed* instance by testing the
    // translation of its model matrix - the ranges themselves must never be
    // distance-culled while a model matrix is in flight.
    struct Batch {
        unsigned vao = 0, vbo = 0, ebo = 0, tris = 0;
        std::vector<Range> ranges;
        float radius = 0.0f;
    };
    int addTrafficModel(const std::vector<B3D::SubMesh>& meshes, const SceneTextures* tex);
    size_t trafficModelCount() const { return batches.size(); }
    size_t trafficTriangles() const;
    // camera setup for a traffic pass, then one call per instance
    void beginTraffic(float camX, float camY, float camZ, float yawDeg, float pitchDeg);
    void endTraffic();
    void drawTrafficInstance(int model, const Mat4& m);
    // Traffic instances submitted / rejected by the view-distance cull since the
    // last beginTraffic() (diagnostics, printed under HT2_CULL_DEBUG).
    size_t trafficInstancesDrawn() const { return trafficDrawn_; }
    size_t trafficInstancesCulled() const { return trafficCulled_; }

    void setSky(float r, float g, float b) { skyR = r; skyG = g; skyB = b; }
    // `setFog(d)` is the distance at which geometry is fully fogged - the
    // original's WEATHER.INI `fogf_*` value (900 units in fair weather,
    // 1300 on the mountain highway, 150..350 in the tunnels).  The camera far
    // plane is derived from it and every world submesh farther than it (plus its
    // own radius) is culled in the scene pass, so this is the draw distance.
    void setFog(float d) { fogDist = d > 1.0f ? d : 1.0f; }
    float fogDistance() const { return fogDist; }
    float viewDistance() const { return fogDist; }
    // Camera far plane: beyond the fog end the image is uniformly fogged, so
    // twice that is pure headroom.  Never below 800 so a high debug camera still
    // sees the ground.
    float farPlane() const { return fogDist * 2.0f > 800.0f ? fogDist * 2.0f : 800.0f; }
    // The colour distant geometry fades into.  It is also the horizon end of the
    // sky gradient, so both must be set together (App::applyLighting).
    void setFogColor(float r, float g, float b) { fogR = r; fogG = g; fogB = b; }
    // Zenith end of the sky gradient; the horizon end is the fog colour.
    void setSkyZenith(float r, float g, float b) { zenR = r; zenG = g; zenB = b; }
    // Weight of the directional (sun) term: rain and snow are overcast.
    void setSunAmount(float a) { sunAmount = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
    float sunAmountValue() const { return sunAmount; }
    // World near plane is 2.0. The nearby loaded cockpit is rendered separately
    // with 0.4 after clearDepth(), without reducing the world's depth precision.
    void setNearPlane(float n) { nearPlane = n > 0.01f ? n : 0.01f; }
    float nearPlaneValue() const { return nearPlane; }
    // Start a foreground depth layer without erasing the rendered world colour.
    void clearDepth();
    void setSun(float x, float y, float z) { sunX = x; sunY = y; sunZ = z; }
    void setAmbient(float a) { ambient = a; }
    void setCulling(bool on) { culling = on; }

    // Two-stop sky: the fog colour at the horizon (so distant geometry blends
    // into the background) and the zenith colour above it.  Drawn after clear()
    // and before the 3D passes; `pitchDeg` places the horizon line.
    void drawSky(float pitchDeg);
    // Diagnostics: submeshes skipped by the view-distance cull in the last pass.
    size_t rangesCulled() const { return rangesCulled_; }
    size_t rangesDrawn() const { return rangesDrawn_; }

    // 3D draws (no clear; caller clears with clearColor())
    void draw(float camX, float camY, float camZ, float yawDeg, float pitchDeg);
    void drawScene(float camX, float camY, float camZ, float yawDeg, float pitchDeg);
    // Model matrix applied to draw()/drawScene() (identity by default).  Used to
    // place the assembled vehicle mesh in the world.
    void setModel(const Mat4& m);
    void clearModel();
    const Mat4& modelMatrix() const { return model; }
    void clear();

    // --- 2D overlay (pixel coordinates, origin top-left, y down) ---
    void begin2D();
    // True while a begin2D()/end2D() block is open (the mirror blits itself into
    // the HUD's batch when the parent already opened one).
    bool overlayActive() const { return in2D; }
    void quad2D(unsigned tex, float x, float y, float w, float h,
                float r, float g, float b, float a,
                float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f);
    // Switch the 2D batch to additive blending (GL_ONE, GL_ONE) and back.  The
    // weather particles are drawn from the original's rain/snow textures, which
    // are bright shapes on black: alpha blending would paint their black
    // background as a dark rectangle, addition leaves it alone.
    void setBlendAdditive(bool on) {
        glBlendFunc(on ? GL_ONE : GL_SRC_ALPHA, on ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    }
    // Vertical two-colour gradient quad (opaque): the sky pass and the weather
    // overlays are built from these.
    void quad2DGradient(float x, float y, float w, float h,
                        const float* cTop, const float* cBot, float a);
    void end2D();
    void flush2D();
    // Generic quad with explicit corners (needed for rotated/stroke geometry).
    // xy4 = 8 floats (x0,y0,x1,y1,x2,y2,x3,y3) in pixel coords, clockwise from TL.
    void pushQuad2D(unsigned tex, const float* xy4, const float* uv4, float r, float g, float b, float a);
    void line2D(unsigned tex, float x0, float y0, float x1, float y1, float thick,
                float r, float g, float b, float a);
    unsigned whiteTex() const { return white; }
    // Every texture created here gets GL_REPEAT + GL_LINEAR_MIPMAP_LINEAR and a
    // full glGenerateMipmap chain (so it is safe to draw in 3D at any distance),
    // plus anisotropic filtering when GL_EXT_texture_filter_anisotropic is
    // available - min(8, driver maximum), see anisotropy()/maxAnisotropy().
    unsigned createTexture(const uint8_t* rgba, int w, int h);
    // Re-uploads a texture created with createTexture() from a full RGBA frame
    // (glBindTexture + glTexSubImage2D) and drops its mipmaps so the caller can
    // stream frames into it (video playback).  Returns false for an unknown
    // texture, without a live GL context, or when glTexSubImage2D is missing -
    // the caller then falls back to rebuilding the texture.
    //
    // EXCEPTION to the mip rule: a streamed texture is switched to
    // GL_LINEAR (no mipmap) because its mip chain would contain stale frames.
    // Those textures are only ever drawn as 1:1 screen quads (movie / test
    // pattern), never in 3D, so they cannot shimmer.
    bool updateTexture(unsigned tex, const uint8_t* rgba, int w, int h);
    // Diagnostics: number of successful updateTexture() calls since init().
    unsigned textureUpdates() const { return textureUpdates_; }
    unsigned lastUpdatedTexture() const { return lastUpdatedTex_; }
    void deleteTexture(unsigned& t);
    // measure helper for the built-in font
    int width() const { return width_; }

    // ---- context / filtering diagnostics ---------------------------------
    // GPU and driver identification, read from the live GL context in init().
    const std::string& gpuVendor() const { return vendor_; }
    const std::string& gpuRenderer() const { return renderer_; }
    const std::string& glVersion() const { return version_; }
    // Anisotropic filtering actually requested for every texture created here
    // (1 = off).  maxAnisotropy() is the driver limit, 1 when the extension is
    // absent.  HT2_ANISO=N overrides the default cap (0 disables, 1 is "off").
    float anisotropy() const { return aniso_; }
    float maxAnisotropy() const { return maxAniso_; }
    unsigned anisotropyTextures() const { return texStats_.aniso; }
    // Invariant bookkeeping for every texture createTexture() built: how many
    // were created, how many have a complete mip chain down to 1x1, how many
    // kept GL_LINEAR_MIPMAP_LINEAR, and how many got anisotropic filtering.
    // Logged once per process in shutdown() and by SceneTextures after each set.
    struct TexStats {
        unsigned created = 0, mipComplete = 0, mipMinFilter = 0, aniso = 0;
        std::string line() const;
    };
    const TexStats& textureStats() const { return texStats_; }

    bool screenshot(const std::string& path);
    SDL_Window* getWindowHandle() const { return window; }

    // ---- render targets (offscreen framebuffers) -------------------------
    // A colour texture + depth renderbuffer the caller can bind as the draw
    // target and sample afterwards with quad2D() (the rear-view mirror renders
    // its reflection into one of these; see src/gfx/Mirror.cpp).
    //
    // The colour texture is created here and is deliberately NOT routed through
    // createTexture(): it has no mip chain and is never drawn in 3D, so it must
    // not enter the "every 3D texture has a complete mip chain" bookkeeping
    // (texStats_).
    struct RenderTarget {
        unsigned fbo = 0, color = 0, depth = 0;
        int w = 0, h = 0;
        bool live() const { return fbo != 0 && color != 0; }
    };
    // Creates rt (any previous contents are destroyed first).  Returns false
    // without a live GL context, for a degenerate size, when the entry points
    // are missing, or when the framebuffer is incomplete - then rt is empty.
    bool createRenderTarget(RenderTarget& rt, int w, int h);
    void destroyRenderTarget(RenderTarget& rt);
    // Binds rt as the draw target: viewport (0,0,w,h), 3D aspect ratio w/h and
    // (optionally) a clear with the current sky colour + depth.  While bound,
    // draw()/drawScene() render into rt; the 2D overlay keeps using the window
    // resolution (flush2D() is never called inside a bound pass).
    bool bindRenderTarget(const RenderTarget& rt, bool clearColorAndDepth = true);
    // Restores the default framebuffer, the window viewport and aspect ratio.
    void unbindRenderTarget();
    bool renderTargetBound() const { return boundRT != nullptr; }
    // Number of live render targets (diagnostics).
    size_t renderTargetCount() const { return targets_.size(); }

    int width_ = 1280, height_ = 720;

private:
    bool buildTerrainMesh();
    // Fine ground mesh from the raw heightmap clamped under GroundMap's tile
    // ceiling; buildRawTerrainMesh() is the no-ground-map fallback.
    bool buildFineGroundMesh();
    bool buildRawTerrainMesh();
    bool buildRoadMesh(std::vector<TVert>& verts, std::vector<unsigned>& idx);
    bool buildSceneMesh();
    bool buildProgram();
    bool buildProgram2D();
    // Resolve GL_EXT_texture_filter_anisotropic and pick the level applied to
    // every texture (once, from init()); applyAnisotropy() puts it on a texture.
    bool probeAnisotropy();
    void applyAnisotropy(unsigned tex);
    // True when levels 0..log2(max(w,h)) all exist and the smallest one is 1x1.
    bool verifyMipChain(int w, int h);
    void upload3D(std::vector<TVert>& verts, std::vector<unsigned>& idx,
                  unsigned& vao, unsigned& vbo, unsigned& ebo, unsigned& count);
    void drawRanges(unsigned vao, const std::vector<Range>& ranges);

    GL gl;
    SDL_Window* window = nullptr;
    SDL_GLContext ctx = nullptr;
    bool offscreen = false;

    // Render targets: the ids live in the caller's RenderTarget, these are the
    // bookkeeping copies used to delete everything in shutdown() and to reject
    // a bind of a target whose GL objects are gone.
    std::vector<RenderTarget> targets_;
    const RenderTarget* boundRT = nullptr;
    // Draw-target size used by the 3D passes (== width_/height_ unless a render
    // target is bound).  The 2D overlay always uses width_/height_.
    int renderW_ = 1280, renderH_ = 720;

    const Terrain* terrain = nullptr;
    const Way* roads = nullptr;
    const GroundMap* groundMap = nullptr;
    bool useRawTerrain = false;
    bool groundEnabled = true;
    unsigned groundTex = 0;
    unsigned groundTris = 0;
    const std::vector<B3D::SubMesh>* sceneMeshes = nullptr;
    const B3D* scene = nullptr;
    const SceneTextures* sceneTex = nullptr;
    std::vector<Range> sceneRanges;
    Range terrainRange;

    unsigned int prog = 0, vao = 0, vbo = 0, ebo = 0;
    unsigned int triCount = 0;
    int uProj = -1, uView = -1, uSun = -1, uFog = -1;
    int uSky = -1, uAmbient = -1, uUseTex = -1, uTex = -1, uModel = -1;
    int uFogColor = -1, uSunAmt = -1;
    Mat4 model;
    float fogDist = 1500.0f;   // original-like short visibility (far plane = 2x)
    float nearPlane = 2.0f;      // camera near plane (see setNearPlane)
    float skyR = 0.47f, skyG = 0.65f, skyB = 0.87f;
    float fogR = 0.47f, fogG = 0.65f, fogB = 0.87f;   // fog / horizon colour
    float zenR = 0.30f, zenG = 0.45f, zenB = 0.72f;   // sky gradient zenith
    float sunX = 0.35f, sunY = 0.75f, sunZ = 0.20f;
    float ambient = 0.55f;
    float sunAmount = 1.0f;      // 1 = clear, lower = overcast
    float viewPos_[3] = {0, 0, 0};   // camera of the last draw(), for culling
    bool cullByRange = false;         // set by drawScene() only
    size_t rangesCulled_ = 0, rangesDrawn_ = 0;
    size_t trafficDrawn_ = 0, trafficCulled_ = 0;
    bool culling = true;
    bool vehicleTwoSided = false;
    bool mirrorX = true;

    // b3d scene buffers
    unsigned int svao = 0, svbo = 0, sebo = 0, striCount = 0;
    const std::vector<B3D::SubMesh>* vehicleMeshes = nullptr;
    const B3D* vehicle = nullptr;
    const SceneTextures* vehicleTex = nullptr;
    unsigned int vvao = 0, vvbo = 0, vvebo = 0, vvTriCount = 0;
    std::vector<Range> vehicleRanges;
    bool buildVehicleMesh();
    bool buildVehicleMeshImpl();
    bool buildMeshFor(const std::vector<B3D::SubMesh>* meshes, const SceneTextures* stex,
                      unsigned& vao, unsigned& vbo, unsigned& ebo, unsigned& count,
                      std::vector<Range>& ranges);
    bool glReady = false;
    std::vector<Batch> batches;
    unsigned textureUpdates_ = 0, lastUpdatedTex_ = 0;

    // anisotropic filtering (GL_EXT_texture_filter_anisotropic)
    float maxAniso_ = 1.0f;          // driver maximum, 1 = unsupported
    float aniso_ = 1.0f;             // level applied to new textures, 1 = off
    TexStats texStats_;
    bool statsLogged_ = false;       // shutdown() may run twice (dtor + explicit)
    std::string vendor_, renderer_, version_;

    // 2D overlay
    struct SpriteVert { float x, y, u, v, r, g, b, a; };
    unsigned int prog2 = 0, vao2 = 0, vbo2 = 0;
    int uOrtho2 = -1, uTex2 = -1;
    std::vector<SpriteVert> sprites;
    unsigned curTex2 = 0;
    unsigned white = 0;
    bool in2D = false;
};

} // namespace ht2
