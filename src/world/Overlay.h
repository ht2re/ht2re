#pragma once
// HT2EDIT overlay: the separate map/vehicle editor's output and the engine's
// input (tools/editor/, docs/editor.md).
//
// The shipped game data is never modified.  Everything the editor changes is
// written to one readable text file (`--overlay FILE`), and both ht2editor and
// ht2re parse it through this class, so there is exactly one implementation of
// the format and of the semantics.
//
// Record grammar (one record per line, `#` starts a comment, blank lines are
// ignored; the first non-comment line must be `HT2EDIT 1`):
//
//   NODEPOS   <tile> <name> <x> <y> <z>        node moved to an absolute spot
//   NODEFLAGS <tile> <name> <flags>            FLAG dword (dec or 0x...)
//   NODEADD   <tile> <room> <name> <x> <y> <z> <flags>
//   NODEDEL   <tile> <name>
//   SEGEDIT   <tile> <index> <attr> <width> <width1>
//   SEGDEL    <tile> <index>
//   SEGADD    <tile> <name> <attr> <width> <width1> <n> <x y z> * n
//   PART      <model> <part> hidden|shown
//   PARTMAT   <model> <part> <material>
//   ANCHORT   <model> <anchor> <tx> <ty> <tz>
//   ANCHORM   <model> <anchor> <m0..m8>
//
// `<tile>` is an ENV tile stem (`aa`), `*` matches every tile.  `<index>` is
// the position of the segment in the shipped .way's own segment order (the
// order Way::segments() returns for a freshly loaded file) - it is deliberately
// independent of the other records, so segment edits are resolved against the
// shipped list before any SEGDEL/SEGADD takes effect (see applyWay()).
//
// Coordinates are the engine's world units.  For a .way node `y` is the height
// (Way::Pos::y), i.e. the same order the .way POSN chunk stores them in.
#include <cstdint>
#include <string>
#include <vector>
#include "Way.h"
#include "B3D.h"

namespace ht2 {

class Overlay {
public:
    // ------------------------------------------------------------------
    // records
    // ------------------------------------------------------------------
    struct NodeRec {
        enum Kind { Pos, Flags, Add, Del };
        Kind kind = Pos;
        std::string tile;
        std::string name;
        std::string room;   // Add only
        Way::Node node;     // Pos / Add payload (name + pos + flags)
    };
    struct SegEdit {
        std::string tile;
        int index = -1;      // into the shipped segment list
        int attr = 0;
        double width = 0.0, width1 = 0.0;
    };
    struct SegDel {
        std::string tile;
        int index = -1;
    };
    struct SegAdd {
        std::string tile;
        std::string name;
        int attr = 0;
        double width = 8.0, width1 = 0.0;
        std::vector<Way::Pos> pts;
    };
    struct PartRec { std::string model, part; bool hidden = false; };
    struct MatRec { std::string model, part, material; };
    struct AnchorRec {
        std::string model, anchor;
        bool hasT = false, hasM = false;
        float t[3] = {0, 0, 0};
        float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    };

    std::vector<NodeRec> nodeRecs;
    std::vector<SegEdit> segEdits;
    std::vector<SegDel> segDels;
    std::vector<SegAdd> segAdds;
    std::vector<PartRec> partRecs;
    std::vector<MatRec> matRecs;
    std::vector<AnchorRec> anchorRecs;

    // ------------------------------------------------------------------
    // file I/O
    // ------------------------------------------------------------------
    // A missing file is not an error: the overlay stays empty and true is
    // returned, so `--overlay` of a file that has not been written yet works.
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void clear();
    bool empty() const;
    size_t records() const;
    const std::string& path() const { return path_; }
    const std::string& error() const { return err_; }

    // ------------------------------------------------------------------
    // authoring helpers (the editor's edit operations; each is idempotent)
    // ------------------------------------------------------------------
    // Absolute node position: replaces an earlier NODEPOS for the same node.
    void setNodePos(const std::string& tile, const Way::Node& n);
    void setNodeFlags(const std::string& tile, const std::string& name, uint32_t flags);
    void addNode(const std::string& tile, const std::string& room, const Way::Node& n);
    void delNode(const std::string& tile, const std::string& name);
    void setSeg(const std::string& tile, int index, int attr, double width, double width1);
    void delSeg(const std::string& tile, int index);
    void addSeg(const SegAdd& s);
    void setPart(const std::string& model, const std::string& part, bool hidden);
    void setPartMaterial(const std::string& model, const std::string& part,
                         const std::string& material);
    void setAnchorT(const std::string& model, const std::string& anchor, const float t[3]);
    void setAnchorM(const std::string& model, const std::string& anchor, const float m[9]);

    // ------------------------------------------------------------------
    // engine side
    // ------------------------------------------------------------------
    // Applies every map record that names `tile` (or `*`) to a freshly loaded
    // Way.  Idempotent: a NODEADD/SEGADD whose name already exists is ignored,
    // so loading the same tile twice (loadData + collectWaySegments) is safe.
    void applyWay(Way& w, const std::string& tile) const;
    // True when at least one record can affect this tile (cheap pre-check).
    bool touchesTile(const std::string& tile) const;
    // Anchor edits must run before B3D::buildNamedScene() (the anchors place the
    // linked part containers), part/material edits after it.
    void applyAnchors(B3D& b, const std::string& model) const;
    void applyParts(std::vector<B3D::SubMesh>& subs, const std::string& model) const;
    bool touchesVehicle(const std::string& model) const;

private:
    std::string path_, err_;
};

} // namespace ht2
