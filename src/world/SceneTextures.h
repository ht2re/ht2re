#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace ht2 {

class Renderer;

// Textures for a B3D scene, loaded from its companion .res files
// (<scene>.res / <scene>1.res / <scene>2.res) and looked up by the material
// names stored in the B3D name table: material "mat_road2" -> "txr\road2.txr".
// 60/64 materials of MENV/dr.b3d map 1:1 this way; a few have aliases.
class SceneTextures {
public:
    // `skipNoLoad` ignores entries flagged `noload` in the container (the
    // original defers those; COMMON.RES keeps nine 256x256 sky textures there
    // that only matter to the sky dome the renderer does not build).
    bool load(Renderer& r, const std::string& resBase, bool skipNoLoad = false);
    // The shared .plm palette (COMMON.RES) - needed to resolve the colour keys
    // declared by the .res MATERIALS table ("ttx N [col C]").
    static void loadPalette(const std::string& commonResPath);
    // Last path component, lower case, underscores dropped.  The same key
    // SceneTextures uses to qualify a texture by the .res it came from.
    static std::string tileKey(const std::string& resBase);
    // 0 means "no texture" (the renderer falls back to the material colour).
    // `tile` (from SubMesh::texTile) prefers that .res over the first file
    // that happened to use the same stem.
    unsigned textureFor(const std::string& material, const std::string& tile = {}) const;
    unsigned white() const { return white_; }
    size_t count() const { return tex_.size(); }
    size_t decoded() const { return decoded_; }
    const std::string& base() const { return base_; }

private:
    std::unordered_map<std::string, unsigned> map_;   // normalized tex name -> GL id
    std::vector<unsigned> tex_;                       // for cleanup
    unsigned white_ = 0;
    size_t decoded_ = 0;
    size_t keyed_ = 0;
    std::string base_;
};

} // namespace ht2
