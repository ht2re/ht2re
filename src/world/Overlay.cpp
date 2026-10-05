#include "Overlay.h"
#include "B3D.h"
#include "../util/Log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace ht2 {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// One whitespace-separated token stream per line.
std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> t;
    std::istringstream in(line);
    std::string w;
    while (in >> w) t.push_back(w);
    return t;
}

bool parseU32(const std::string& s, uint32_t& out) {
    char* end = nullptr;
    errno = 0;
    unsigned long v = strtoul(s.c_str(), &end, 0);   // 0x... / decimal
    if (end == s.c_str() || *end != '\0' || errno) return false;
    out = (uint32_t)v;
    return true;
}

bool parseD(const std::string& s, double& out) {
    char* end = nullptr;
    errno = 0;
    double v = strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno) return false;
    out = v;
    return true;
}

bool parseF(const std::string& s, float& out) {
    double v = 0;
    if (!parseD(s, v)) return false;
    out = (float)v;
    return true;
}

std::string fmtD(double v) {
    char b[64];
    snprintf(b, sizeof b, "%.4f", v);
    return b;
}

std::string fmtF(float v) {
    char b[64];
    snprintf(b, sizeof b, "%.6g", (double)v);
    return b;
}

// "aa" matches "aa"; "*" matches everything; "" is treated as "any tile"
// (older/hand-written overlays).
bool tileMatch(const std::string& rec, const std::string& tile) {
    if (rec.empty() || rec == "*" || tile == "*") return true;
    return rec == tile;
}

std::string stemOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

Way::Room& roomFor(Way& w, const std::string& name) {
    for (auto& r : w.rooms) if (r.name == name) return r;
    w.rooms.emplace_back();
    w.rooms.back().name = name;
    return w.rooms.back();
}

size_t dropNamed(Way& w, const std::string& name) {
    size_t n = 0;
    for (auto& r : w.rooms) {
        std::vector<Way::Node> keep;
        keep.reserve(r.nodes.size());
        for (auto& nd : r.nodes) {
            if (nd.name == name) { ++n; continue; }
            keep.push_back(std::move(nd));
        }
        r.nodes.swap(keep);
    }
    return n;
}

bool hasNamed(const Way& w, const std::string& name) {
    for (const auto& r : w.rooms)
        for (const auto& nd : r.nodes)
            if (nd.name == name) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------
void Overlay::clear() {
    nodeRecs.clear();
    segEdits.clear();
    segDels.clear();
    segAdds.clear();
    partRecs.clear();
    matRecs.clear();
    anchorRecs.clear();
    err_.clear();
}

bool Overlay::empty() const {
    return nodeRecs.empty() && segEdits.empty() && segDels.empty() && segAdds.empty() &&
           partRecs.empty() && matRecs.empty() && anchorRecs.empty();
}

size_t Overlay::records() const {
    return nodeRecs.size() + segEdits.size() + segDels.size() + segAdds.size() +
           partRecs.size() + matRecs.size() + anchorRecs.size();
}

bool Overlay::load(const std::string& path) {
    clear();
    path_ = path;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        // Not an error: an overlay that has not been written yet simply means
        // "no edits".  The engine and the editor both start from the shipped
        // data, which is never modified.
        LOG("overlay: %s not present (no edits)", path.c_str());
        return true;
    }
    std::string text;
    {
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
    }
    fclose(f);

    size_t lineNo = 0, bad = 0;
    bool header = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        ++lineNo;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::vector<std::string> t = tokens(line);
        if (t.empty()) continue;
        auto fail = [&](const char* why) {
            LOG("overlay: %s:%zu: bad %s record ('%s')", path.c_str(), lineNo, why, line.c_str());
            ++bad;
        };
        if (!header) {
            if (t[0] != "HT2EDIT") { LOG("overlay: %s: missing 'HT2EDIT <version>' header", path.c_str()); err_ = "no header"; return false; }
            if (t.size() < 2 || t[1] != "1") { LOG("overlay: %s: unsupported version '%s'", path.c_str(), t.size() > 1 ? t[1].c_str() : "?"); err_ = "version"; return false; }
            header = true;
            continue;
        }
        const std::string& k = t[0];
        if (k == "NODEPOS" && t.size() == 6) {
            NodeRec r; r.kind = NodeRec::Pos; r.tile = t[1]; r.name = t[2];
            r.node.name = t[2];
            if (!parseD(t[3], r.node.pos.x) || !parseD(t[4], r.node.pos.y) ||
                !parseD(t[5], r.node.pos.z)) { fail("NODEPOS"); continue; }
            nodeRecs.push_back(r);
        } else if (k == "NODEFLAGS" && t.size() == 4) {
            NodeRec r; r.kind = NodeRec::Flags; r.tile = t[1]; r.name = t[2];
            r.node.name = t[2];
            if (!parseU32(t[3], r.node.flags)) { fail("NODEFLAGS"); continue; }
            nodeRecs.push_back(r);
        } else if (k == "NODEADD" && t.size() == 8) {
            NodeRec r; r.kind = NodeRec::Add; r.tile = t[1]; r.room = t[2]; r.name = t[3];
            r.node.name = t[3];
            if (!parseD(t[4], r.node.pos.x) || !parseD(t[5], r.node.pos.y) ||
                !parseD(t[6], r.node.pos.z) || !parseU32(t[7], r.node.flags)) {
                fail("NODEADD"); continue;
            }
            nodeRecs.push_back(r);
        } else if (k == "NODEDEL" && t.size() == 3) {
            NodeRec r; r.kind = NodeRec::Del; r.tile = t[1]; r.name = t[2];
            r.node.name = t[2];
            nodeRecs.push_back(r);
        } else if (k == "SEGEDIT" && t.size() == 6) {
            SegEdit e; e.tile = t[1];
            double idx = 0;
            uint32_t attr = 0;
            if (!parseD(t[2], idx) || !parseU32(t[3], attr) ||
                !parseD(t[4], e.width) || !parseD(t[5], e.width1)) { fail("SEGEDIT"); continue; }
            e.attr = (int)(int32_t)attr;
            e.index = (int)idx;
            segEdits.push_back(e);
        } else if (k == "SEGDEL" && t.size() == 3) {
            SegDel d; d.tile = t[1];
            double idx = 0;
            if (!parseD(t[2], idx)) { fail("SEGDEL"); continue; }
            d.index = (int)idx;
            segDels.push_back(d);
        } else if (k == "SEGADD" && t.size() >= 7) {
            SegAdd s; s.tile = t[1]; s.name = t[2];
            double n = 0;
            uint32_t attr = 0;
            if (!parseU32(t[3], attr) || !parseD(t[4], s.width) ||
                !parseD(t[5], s.width1) || !parseD(t[6], n)) { fail("SEGADD"); continue; }
            s.attr = (int)(int32_t)attr;
            s.pts.resize((size_t)(n > 0 ? n : 0));
            if (t.size() != 7 + s.pts.size() * 3) { fail("SEGADD point count"); continue; }
            bool ok = true;
            for (size_t i = 0; i < s.pts.size() && ok; ++i)
                ok = parseD(t[7 + i * 3 + 0], s.pts[i].x) &&
                     parseD(t[7 + i * 3 + 1], s.pts[i].y) &&
                     parseD(t[7 + i * 3 + 2], s.pts[i].z);
            if (!ok) { fail("SEGADD points"); continue; }
            segAdds.push_back(std::move(s));
        } else if (k == "PART" && t.size() == 4) {
            PartRec p; p.model = t[1]; p.part = t[2];
            if (t[3] == "hidden") p.hidden = true;
            else if (t[3] != "shown") { fail("PART"); continue; }
            partRecs.push_back(p);
        } else if (k == "PARTMAT" && t.size() == 4) {
            MatRec m; m.model = t[1]; m.part = t[2]; m.material = t[3];
            matRecs.push_back(m);
        } else if (k == "ANCHORT" && t.size() == 6) {
            AnchorRec a; a.model = t[1]; a.anchor = t[2]; a.hasT = true;
            if (!parseF(t[3], a.t[0]) || !parseF(t[4], a.t[1]) || !parseF(t[5], a.t[2])) {
                fail("ANCHORT"); continue;
            }
            anchorRecs.push_back(a);
        } else if (k == "ANCHORM" && t.size() == 12) {
            AnchorRec a; a.model = t[1]; a.anchor = t[2]; a.hasM = true;
            bool ok = true;
            for (int i = 0; i < 9 && ok; ++i) ok = parseF(t[3 + i], a.m[i]);
            if (!ok) { fail("ANCHORM"); continue; }
            anchorRecs.push_back(a);
        } else {
            fail("unknown");
        }
    }
    if (!header && !text.empty()) {
        LOG("overlay: %s: empty/headerless file", path.c_str());
        err_ = "no header";
        return false;
    }
    LOG("overlay: %s loaded, %zu record(s), %zu bad line(s)", path.c_str(), records(), bad);
    return bad == 0;
}

bool Overlay::save(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { LOG("overlay: cannot write %s", path.c_str()); return false; }
    std::string out;
    char b[512];
    out += "HT2EDIT 1\n";
    out += "# HT2:RE editor overlay - written by ht2editor, read by ht2re --overlay\n";
    out += "# The shipped data is never modified; delete this file to get it back.\n";
    out += "# Format: docs/editor.md\n";
    if (!nodeRecs.empty() || !segEdits.empty() || !segDels.empty() || !segAdds.empty())
        out += "\n# ---- map (.way) ----------------------------------------------------\n";
    for (const NodeRec& r : nodeRecs) {
        switch (r.kind) {
            case NodeRec::Pos:
                snprintf(b, sizeof b, "NODEPOS %s %s %s %s %s\n", r.tile.c_str(), r.name.c_str(),
                         fmtD(r.node.pos.x).c_str(), fmtD(r.node.pos.y).c_str(),
                         fmtD(r.node.pos.z).c_str());
                break;
            case NodeRec::Flags:
                snprintf(b, sizeof b, "NODEFLAGS %s %s 0x%x\n", r.tile.c_str(),
                         r.name.c_str(), r.node.flags);
                break;
            case NodeRec::Add:
                snprintf(b, sizeof b, "NODEADD %s %s %s %s %s %s 0x%x\n", r.tile.c_str(),
                         r.room.c_str(), r.name.c_str(), fmtD(r.node.pos.x).c_str(),
                         fmtD(r.node.pos.y).c_str(), fmtD(r.node.pos.z).c_str(), r.node.flags);
                break;
            case NodeRec::Del:
                snprintf(b, sizeof b, "NODEDEL %s %s\n", r.tile.c_str(), r.name.c_str());
                break;
        }
        out += b;
    }
    for (const SegEdit& e : segEdits) {
        snprintf(b, sizeof b, "SEGEDIT %s %d %d %s %s\n", e.tile.c_str(), e.index, e.attr,
                 fmtD(e.width).c_str(), fmtD(e.width1).c_str());
        out += b;
    }
    for (const SegDel& d : segDels) {
        snprintf(b, sizeof b, "SEGDEL %s %d\n", d.tile.c_str(), d.index);
        out += b;
    }
    for (const SegAdd& s : segAdds) {
        snprintf(b, sizeof b, "SEGADD %s %s %d %s %s %zu", s.tile.c_str(), s.name.c_str(),
                 s.attr, fmtD(s.width).c_str(), fmtD(s.width1).c_str(), s.pts.size());
        out += b;
        for (const Way::Pos& p : s.pts) {
            snprintf(b, sizeof b, " %s %s %s", fmtD(p.x).c_str(), fmtD(p.y).c_str(),
                     fmtD(p.z).c_str());
            out += b;
        }
        out += "\n";
    }
    if (!partRecs.empty() || !matRecs.empty() || !anchorRecs.empty())
        out += "\n# ---- vehicle models (COMMON/TRUCKS.B3D) ---------------------------\n";
    for (const PartRec& p : partRecs) {
        snprintf(b, sizeof b, "PART %s %s %s\n", p.model.c_str(), p.part.c_str(),
                 p.hidden ? "hidden" : "shown");
        out += b;
    }
    for (const MatRec& m : matRecs) {
        snprintf(b, sizeof b, "PARTMAT %s %s %s\n", m.model.c_str(), m.part.c_str(),
                 m.material.c_str());
        out += b;
    }
    for (const AnchorRec& a : anchorRecs) {
        if (a.hasT) {
            snprintf(b, sizeof b, "ANCHORT %s %s %s %s %s\n", a.model.c_str(),
                     a.anchor.c_str(), fmtF(a.t[0]).c_str(), fmtF(a.t[1]).c_str(),
                     fmtF(a.t[2]).c_str());
            out += b;
        }
        if (a.hasM) {
            snprintf(b, sizeof b, "ANCHORM %s %s", a.model.c_str(), a.anchor.c_str());
            out += b;
            for (int i = 0; i < 9; ++i) { snprintf(b, sizeof b, " %s", fmtF(a.m[i]).c_str()); out += b; }
            out += "\n";
        }
    }
    if (fwrite(out.data(), 1, out.size(), f) != out.size()) {
        fclose(f);
        LOG("overlay: short write to %s", path.c_str());
        return false;
    }
    fclose(f);
    LOG("overlay: %s written, %zu record(s)", path.c_str(), records());
    return true;
}

// ---------------------------------------------------------------------------
// authoring helpers
// ---------------------------------------------------------------------------
void Overlay::setNodePos(const std::string& tile, const Way::Node& n) {
    for (NodeRec& r : nodeRecs)
        if (r.kind == NodeRec::Pos && r.tile == tile && r.name == n.name) {
            r.node.pos = n.pos;
            return;
        }
    NodeRec r; r.kind = NodeRec::Pos; r.tile = tile; r.name = n.name; r.node = n;
    nodeRecs.push_back(r);
}

void Overlay::setNodeFlags(const std::string& tile, const std::string& name, uint32_t flags) {
    for (NodeRec& r : nodeRecs)
        if (r.kind == NodeRec::Flags && r.tile == tile && r.name == name) {
            r.node.flags = flags;
            return;
        }
    NodeRec r; r.kind = NodeRec::Flags; r.tile = tile; r.name = name;
    r.node.name = name; r.node.flags = flags;
    nodeRecs.push_back(r);
}

void Overlay::addNode(const std::string& tile, const std::string& room, const Way::Node& n) {
    for (const NodeRec& r : nodeRecs)
        if (r.kind == NodeRec::Add && r.tile == tile && r.name == n.name) return;
    NodeRec r; r.kind = NodeRec::Add; r.tile = tile; r.room = room; r.name = n.name; r.node = n;
    nodeRecs.push_back(r);
}

void Overlay::delNode(const std::string& tile, const std::string& name) {
    // A node that was only added by this overlay is removed outright.
    for (size_t i = 0; i < nodeRecs.size(); ++i)
        if (nodeRecs[i].kind == NodeRec::Add && nodeRecs[i].tile == tile &&
            nodeRecs[i].name == name) {
            nodeRecs.erase(nodeRecs.begin() + i);
            return;
        }
    for (NodeRec& r : nodeRecs)
        if (r.kind == NodeRec::Del && r.tile == tile && r.name == name) return;
    // Drop now-pointless position/flag records for the deleted node.
    for (size_t i = nodeRecs.size(); i-- > 0;)
        if (nodeRecs[i].tile == tile && nodeRecs[i].name == name &&
            (nodeRecs[i].kind == NodeRec::Pos || nodeRecs[i].kind == NodeRec::Flags))
            nodeRecs.erase(nodeRecs.begin() + i);
    NodeRec r; r.kind = NodeRec::Del; r.tile = tile; r.name = name; r.node.name = name;
    nodeRecs.push_back(r);
}

void Overlay::setSeg(const std::string& tile, int index, int attr, double width, double width1) {
    for (SegEdit& e : segEdits)
        if (e.tile == tile && e.index == index) {
            e.attr = attr; e.width = width; e.width1 = width1;
            return;
        }
    SegEdit e; e.tile = tile; e.index = index; e.attr = attr; e.width = width; e.width1 = width1;
    segEdits.push_back(e);
}

void Overlay::delSeg(const std::string& tile, int index) {
    for (SegEdit& e : segEdits) if (e.tile == tile && e.index == index) e.index = -1;
    for (SegDel& d : segDels) if (d.tile == tile && d.index == index) return;
    SegDel d; d.tile = tile; d.index = index;
    segDels.push_back(d);
}

void Overlay::addSeg(const SegAdd& s) {
    for (SegAdd& e : segAdds)
        if (e.tile == s.tile && !e.name.empty() && e.name == s.name) { e = s; return; }
    segAdds.push_back(s);
}

void Overlay::setPart(const std::string& model, const std::string& part, bool hidden) {
    for (PartRec& p : partRecs)
        if (p.model == model && p.part == part) { p.hidden = hidden; return; }
    PartRec p; p.model = model; p.part = part; p.hidden = hidden;
    partRecs.push_back(p);
}

void Overlay::setPartMaterial(const std::string& model, const std::string& part,
                              const std::string& material) {
    for (MatRec& m : matRecs)
        if (m.model == model && m.part == part) { m.material = material; return; }
    MatRec m; m.model = model; m.part = part; m.material = material;
    matRecs.push_back(m);
}

void Overlay::setAnchorT(const std::string& model, const std::string& anchor, const float t[3]) {
    for (AnchorRec& a : anchorRecs)
        if (a.model == model && a.anchor == anchor) {
            a.hasT = true;
            memcpy(a.t, t, sizeof a.t);
            return;
        }
    AnchorRec a; a.model = model; a.anchor = anchor; a.hasT = true;
    memcpy(a.t, t, sizeof a.t);
    anchorRecs.push_back(a);
}

void Overlay::setAnchorM(const std::string& model, const std::string& anchor, const float m[9]) {
    for (AnchorRec& a : anchorRecs)
        if (a.model == model && a.anchor == anchor) {
            a.hasM = true;
            memcpy(a.m, m, sizeof a.m);
            return;
        }
    AnchorRec a; a.model = model; a.anchor = anchor; a.hasM = true;
    memcpy(a.m, m, sizeof a.m);
    anchorRecs.push_back(a);
}

// ---------------------------------------------------------------------------
// engine side
// ---------------------------------------------------------------------------
bool Overlay::touchesTile(const std::string& tile) const {
    auto any = [&](const std::string& t) { return tileMatch(t, tile); };
    for (const NodeRec& r : nodeRecs) if (any(r.tile)) return true;
    for (const SegEdit& e : segEdits) if (any(e.tile)) return true;
    for (const SegDel& d : segDels) if (any(d.tile)) return true;
    for (const SegAdd& s : segAdds) if (any(s.tile)) return true;
    return false;
}

bool Overlay::touchesVehicle(const std::string& model) const {
    for (const PartRec& p : partRecs) if (p.model.empty() || p.model == model) return true;
    for (const MatRec& m : matRecs) if (m.model.empty() || m.model == model) return true;
    for (const AnchorRec& a : anchorRecs) if (a.model.empty() || a.model == model) return true;
    return false;
}

void Overlay::applyWay(Way& w, const std::string& tile) const {
    if (!touchesTile(tile)) return;
    size_t nodesTouched = 0, segsTouched = 0;

    // --- pass A: nodes, in record order ---------------------------------
    for (const NodeRec& r : nodeRecs) {
        if (!tileMatch(r.tile, tile)) continue;
        switch (r.kind) {
            case NodeRec::Del:
                nodesTouched += dropNamed(w, r.name);
                break;
            case NodeRec::Add:
                // idempotent: loading the same tile twice must not duplicate
                if (hasNamed(w, r.name)) break;
                roomFor(w, r.room).nodes.push_back(r.node);
                ++nodesTouched;
                break;
            case NodeRec::Pos:
                for (auto& room : w.rooms)
                    for (auto& nd : room.nodes)
                        if (nd.name == r.name) { nd.pos = r.node.pos; ++nodesTouched; }
                break;
            case NodeRec::Flags:
                for (auto& room : w.rooms)
                    for (auto& nd : room.nodes)
                        if (nd.name == r.name) { nd.flags = r.node.flags; ++nodesTouched; }
                break;
        }
    }

    // --- pass B: segments against the shipped order ----------------------
    // Every index in the file refers to the segment list as the shipped .way
    // parsed it, so all index lookups happen against that pristine list before
    // any deletion or addition is folded in.
    if (!segEdits.empty() || !segDels.empty() || !segAdds.empty()) {
        std::vector<bool> drop(w.segs.size(), false);
        std::vector<char> edited(w.segs.size(), 0);
        for (const SegDel& d : segDels) {
            if (!tileMatch(d.tile, tile)) continue;
            if (d.index < 0 || (size_t)d.index >= w.segs.size()) continue;
            drop[(size_t)d.index] = true;
        }
        for (const SegEdit& e : segEdits) {
            if (!tileMatch(e.tile, tile)) continue;
            if (e.index < 0 || (size_t)e.index >= w.segs.size()) continue;
            Way::Seg& sg = w.segs[(size_t)e.index];
            sg.attr = e.attr;
            sg.width = e.width;
            sg.width1 = e.width1;
            edited[(size_t)e.index] = 1;
        }
        segsTouched = (size_t)std::count(edited.begin(), edited.end(), (char)1) +
                      (size_t)std::count(drop.begin(), drop.end(), true);
        std::vector<Way::Seg> out;
        out.reserve(w.segs.size() + segAdds.size());
        for (size_t i = 0; i < w.segs.size(); ++i)
            if (!drop[i]) out.push_back(std::move(w.segs[i]));
        for (const SegAdd& s : segAdds) {
            if (!tileMatch(s.tile, tile)) continue;
            if (s.pts.size() < 2) continue;
            bool dup = false;
            if (!s.name.empty())
                for (const Way::Seg& e : out) if (e.name == s.name) { dup = true; break; }
            if (dup) continue;
            Way::Seg sg;
            sg.name = s.name;
            sg.attr = s.attr;
            sg.width = s.width;
            sg.width1 = s.width1;
            sg.pts = s.pts;
            out.push_back(std::move(sg));
            ++segsTouched;
        }
        w.segs.swap(out);
    }
    if (nodesTouched || segsTouched)
        LOG("overlay: tile %s: %zu node record(s), %zu segment record(s) applied",
            tile.c_str(), nodesTouched, segsTouched);
}

void Overlay::applyAnchors(B3D& b, const std::string& model) const {
    size_t n = 0;
    for (const AnchorRec& a : anchorRecs) {
        if (!a.model.empty() && a.model != model) continue;
        B3D::Anchor cur;
        if (!b.anchorOf(a.anchor, cur)) {
            LOG("overlay: anchor '%s' not in the container (vehicle %s)", a.anchor.c_str(),
                model.c_str());
            continue;
        }
        if (a.hasT) memcpy(cur.t, a.t, sizeof cur.t);
        if (a.hasM) memcpy(cur.m, a.m, sizeof cur.m);
        if (b.setAnchor(a.anchor, cur)) ++n;
    }
    if (n) LOG("overlay: vehicle %s: %zu anchor(s) overridden", model.c_str(), n);
}

void Overlay::applyParts(std::vector<B3D::SubMesh>& subs, const std::string& model) const {
    size_t renamed = 0, dropped = 0;
    for (const MatRec& m : matRecs) {
        if (!m.model.empty() && m.model != model) continue;
        for (B3D::SubMesh& sm : subs)
            if (sm.name == m.part) { sm.material = m.material; ++renamed; }
    }
    // Hidden last: a part that is both recoloured and hidden disappears.
    for (const PartRec& p : partRecs) {
        if (!p.model.empty() && p.model != model) continue;
        if (!p.hidden) continue;
        std::vector<B3D::SubMesh> keep;
        keep.reserve(subs.size());
        for (B3D::SubMesh& sm : subs) {
            if (sm.name == p.part) { ++dropped; continue; }
            keep.push_back(std::move(sm));
        }
        subs.swap(keep);
    }
    if (renamed || dropped)
        LOG("overlay: vehicle %s: %zu submesh(es) recoloured, %zu hidden",
            model.c_str(), renamed, dropped);
}

} // namespace ht2
