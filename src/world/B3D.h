#pragma once
// .b3d — the game's binary scene/geometry container (see NOTES.md "B3D").
//
// Layout (fully binary, validated byte-exact by tools/b3d_full.py):
//   header 0x1C: "b3d\0" + 6 dwords   (name table at 0x1C, N x 32 B)
//   body:   [111] { record | 444 | 555 }* [222]
//   record: [333] name32[32] subtag params <count | 555> { count x record }
//
// The record tree is data-driven: the parameters of a record are a fixed number
// of dwords per subtag (FIXED), except for mesh (7/37), face (35/8) and a few
// variable-length leaf subtags, which are located by the {1.0f, 0x7fff} polygon
// marker and by the 555/444 terminators.  This mirrors tools/b3d_full.py 1:1.
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <utility>

namespace ht2 {

class B3D {
public:
    // One material polygon: vertex indices + the color id (first dword) and the
    // material name id (dword #3 in the poly header, indexes names()).
    struct Poly {
        uint32_t color = 0;
        uint32_t material = 0;
        // Bit 0x80 of the first dword is the "triangle strip" flag; without it
        // the indices are a triangle fan (i.e. a quad cycle for cnt == 4).
        // Verified against the shipped data: every non-simple (self-crossing)
        // 4-index polygon has 0x80 set (144/128/129), while 0/1/16/50 are
        // always simple perimeter quads.  Treating a quad as a strip produces a
        // bow-tie (road signs rendered as half a sign), and treating a strip as
        // a fan produces long blades (the airport light masts).
        bool strip = false;
        std::vector<uint32_t> idx;
    };
    struct Face {
        uint32_t subtag = 0;
        uint32_t color = 0;      // record's 1st extra dword
        uint32_t material = 0;   // record's 2nd extra dword (names() index)
        float bbox[4] = {0, 0, 0, 0};
        std::vector<Poly> polys;
    };
    struct Mesh {
        std::string name;
        uint32_t subtag = 0;
        uint32_t fmt = 0;        // vertex format flag (subtag 37 only)
        uint32_t vcount = 0;
        uint32_t stride = 0;     // floats per vertex
        float bbox[4] = {0, 0, 0, 0};
        std::vector<float> verts;    // vcount * stride
        std::vector<Face> faces;
        std::vector<float> normals;  // computed, vcount * 3
    };
    struct Node {
        uint32_t subtag = 0;
        std::string name;
        size_t off = 0, end = 0;
        std::vector<uint32_t> params;  // fixed-size parameter block (if known)
        std::vector<Node> kids;
        int meshIndex = -1;
        int faceIndex = -1;
    };
    // Flattened, GPU-ready batch: one per (scene node x face x material).
    struct SubMesh {
        std::string name;        // node name
        std::string material;    // material name ("" if unknown)
        // Enclosing `subtag 21` "key" node name - the scripts toggle visibility
        // of these groups (people/cows by day, roadworks, floods, ...).  Empty
        // means "always drawn".
        std::string group;
        uint32_t materialIndex = 0;
        float center[3] = {0, 0, 0};
        float radius = 0;
        std::vector<float> pos;  // 3 floats/vertex
        std::vector<float> nrm;  // 3 floats/vertex
        std::vector<float> uv;   // 2 floats/vertex
        std::vector<uint32_t> idx;
    };

    // Placed instance of a container: the game's scene files keep object
    // geometry in local coordinates and place it with subtag-24 anchors via
    // subtag-18 links ("<Anchor> -> <Container>").
    struct Anchor {
        float m[9] = {1,0,0, 0,1,0, 0,0,1};   // rotation (b3d space)
        float t[3] = {0,0,0};                 // translation (b3d space)
    };

    bool load(const std::string& path);
    bool loaded() const { return !d.empty(); }
    size_t anchorCount() const { return anchors.size(); }
    size_t linkCount() const { return links.size(); }

    const std::vector<std::string>& names() const { return nameTable; }
    const std::vector<Node>& tree() const { return topNodes; }
    const std::vector<Mesh>& meshes() const { return meshList; }
    const std::vector<SubMesh>& submeshes() const { return subs; }
    std::vector<SubMesh>& submeshesMutable() { return subs; }
    size_t consumed() const { return consumedBytes; }
    size_t total() const { return totalBytes; }
    const float* bboxMin() const { return bbMin; }
    const float* bboxMax() const { return bbMax; }
    float bboxCenter(int i) const { return 0.5f * (bbMin[i] + bbMax[i]); }
    // Build the flat render list (also computes vertex normals + bbox).
    // Whole-file scene: unlinked containers are drawn as-is (world geometry),
    // containers named by a subtag-18 link are drawn at their anchors instead.
    void buildScene();
    // One top-level container only (a vehicle: TRUCKS.B3D "<Model>"), following
    // its link records into the shared part containers.
    bool buildNamedScene(const std::string& rootName);
    const std::string& sceneName() const { return sceneName_; }
    // Count of records per subtag (validation aid).
    std::vector<uint32_t> subtagHistogram() const;

    std::vector<std::pair<std::string, Anchor>> anchorList;   // name -> transform
    std::vector<std::pair<std::string, std::vector<std::string>>> linkList;  // container -> anchors

private:
    std::map<std::string, Anchor> anchors;
    std::map<std::string, std::vector<std::string>> links;
    std::map<std::string, const Node*> nodeIndex;   // name -> node (for link targets)
    std::string sceneName_;
    void buildFrom(const std::vector<const Node*>& roots, bool skipLinked);

    uint32_t u32(size_t p) const;
    bool known(uint32_t v) const;
    size_t skipClosers(size_t q) const;
    size_t findCloser(size_t start) const;
    bool okEnd(size_t e) const;
    uint32_t strideOf(size_t voff, uint32_t vc) const;
    bool parsePolys(size_t q, uint32_t npoly, Face& f, size_t& outQ) const;
    bool parseRecord(size_t p, Node& out);
    static std::string name32(const uint8_t* p, size_t n);

    std::vector<uint8_t> d;
    std::vector<std::string> nameTable;
    std::vector<Node> topNodes;
    std::vector<Mesh> meshList;
    std::vector<Face> faceList;
    std::vector<SubMesh> subs;
    size_t consumedBytes = 0, totalBytes = 0;
    float bbMin[3] = {1e30f, 1e30f, 1e30f};
    float bbMax[3] = {-1e30f, -1e30f, -1e30f};
    mutable std::string err;
};

} // namespace ht2
