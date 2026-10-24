#include "B3D.h"
#include "../util/Log.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <map>

namespace ht2 {

// --- token constants (identical to tools/b3d_full.py) -----------------------
static const uint32_t T_OPEN = 0x6f;   // 111
static const uint32_t T_EOF  = 0xde;   // 222
static const uint32_t T_REC  = 0x14d;  // 333
static const uint32_t T_BSEC = 0x1bc;  // 444
static const uint32_t T_ESEC = 0x22b;  // 555
static const uint32_t MARK_F = 0x3f800000;  // 1.0f
static const uint32_t MARK_T = 0x00007fff;

// floats per vertex for subtag 37 vertex formats (0x0 -> 5)
static uint32_t nfm(uint32_t fmt) {
    switch (fmt) {
        case 0x0:   return 5;
        case 0x2:   return 8;
        case 0x3:   return 6;
        case 0x102: return 10;
        case 0x202: return 12;
        case 0x203: return 10;   // refer_TextureSizeKey / waterfall objects
        default:    return 0;
    }
}

// fixed parameter sizes (dwords) per subtag
static int fixedParams(uint32_t sub) {
    switch (sub) {
        case 0:  return 10; case 1: return 16; case 2: return 8;
        case 4:  return 20; case 5: return 12; case 9: return 8;
        case 10: return 8;  case 12: return 10; case 13: return 6;
        case 14: return 10; case 18: return 20; case 19: return 0;
        case 21: return 6;  case 24: return 13; case 25: return 22;
        case 28: return 29; case 29: return 15; case 30: return 18;
        case 33: return 22; case 40: return 32;
        default: return -1;
    }
}
static int polyHdrDw(uint32_t sub) { return sub == 35 ? 7 : (sub == 8 ? 5 : 0); }
static bool isScan(uint32_t sub) { return sub == 20 || sub == 23; }
static bool isMesh(uint32_t sub) { return sub == 7 || sub == 37; }
static bool isFace(uint32_t sub) { return sub == 35 || sub == 8; }

// UV attribute offset (in floats) inside a vertex record.  Derived from the
// asset set: the normal triple is unit length at 0x2->5, 0x3->(none),
// 0x102->7, 0x202->9; the primary UV pair sits directly before it:
//   0x0  pos[3]              + uv[2]                       -> 3
//   0x2  pos[3]              + uv[2] + normal[3]           -> 3
//   0x3  pos[3] + e[1]       + uv[2]                       -> 4
//   0x102 pos[3] + foo[4]    + normal[3]                   -> 3
//   0x202 pos[3] + foo[6]    + normal[3]                   -> 3
// The stage-0 texture coordinates are the two floats directly after the
// position in *every* format: pos[3] | uv[2] | [extra uv sets] | normal[3] | w.
// Recovered from the original loader (king.exe 0x5f2800, reached from both the
// subtag-7 and subtag-37 handlers).  fmt 0x03 used to be read at offset 4 here,
// which made v the constant 1.0 - the texture collapsed to a single texel row
// (the "smeared" look on 2307 meshes, 794 of them in TRUCKS.B3D).
static int uvOffsetOf(uint32_t sub, uint32_t fmt, uint32_t stride) {
    (void)sub; (void)fmt;
    return stride >= 5 ? 3 : -1;
}

std::string B3D::name32(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string((const char*)p, len);
}

uint32_t B3D::u32(size_t p) const {
    if (p + 4 > d.size()) return 0xFFFFFFFFu;
    uint32_t v;
    memcpy(&v, d.data() + p, 4);
    return v;
}
bool B3D::known(uint32_t v) const {
    return v == T_REC || v == T_BSEC || v == T_ESEC || v == T_OPEN || v == T_EOF;
}
size_t B3D::skipClosers(size_t q) const {
    while ((u32(q) == T_BSEC || u32(q) == T_ESEC) && known(u32(q + 4))) q += 4;
    return q;
}
size_t B3D::findCloser(size_t start) const {
    if (d.size() < 8) return (size_t)-1;
    for (size_t r = start; r + 8 <= d.size(); r += 4) {
        uint32_t v = u32(r);
        if ((v == T_ESEC || v == T_BSEC) && known(u32(r + 4))) return r;
    }
    return (size_t)-1;
}
bool B3D::okEnd(size_t e) const {
    if (e > d.size() || e + 8 > d.size()) return false;
    uint32_t v = u32(e);
    return v == T_ESEC || (v < 100000u && u32(e + 4) == T_REC);
}
uint32_t B3D::strideOf(size_t voff, uint32_t vc) const {
    for (uint32_t s = 1; s < 20; ++s)
        if (okEnd(voff + 4 * (size_t)s * vc)) return s;
    return 0;
}

// --- anchors (subtag 24) and links (subtag 18) ------------------------------
// sub 24: m9[9f] (rotation, b3d space) + pos[3f] + 1 f32.  sub 18: 4f + name32
// (anchor) + name32 (container).  A container named by a link is drawn once per
// referenced anchor, with that anchor's rotation+translation applied to its
// whole subtree.  Verified against ENV/aa.b3d: the "STOS" warehouse (local
// x -5.9..4.0) lands exactly on the .way node STO_AA (4750, -5675, 450).
struct XForm {
    float m[9] = {1,0,0, 0,1,0, 0,0,1};
    float t[3] = {0,0,0};
    bool identity = true;
};

static std::string paramsName(const std::vector<uint32_t>& p, size_t dwOff) {
    if (dwOff + 8 > p.size()) return std::string();
    char buf[33];
    for (int i = 0; i < 8; ++i) {
        uint32_t w = p[dwOff + i];
        buf[i * 4 + 0] = (char)(w & 0xFF);
        buf[i * 4 + 1] = (char)((w >> 8) & 0xFF);
        buf[i * 4 + 2] = (char)((w >> 16) & 0xFF);
        buf[i * 4 + 3] = (char)((w >> 24) & 0xFF);
    }
    buf[32] = 0;
    size_t len = 0;
    while (len < 32 && buf[len]) ++len;
    return std::string(buf, len);
}

static float bitsToFloat(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// Expands a polygon (triangle list or strip) into triangles with a consistent
// winding.  `cb` is called with the three vertex indices of every triangle.
template <typename F>
static void triangulate(const std::vector<uint32_t>& idx, bool strip, F cb) {
    const size_t n = idx.size();
    if (n < 3) return;
    if (!strip) {
        for (size_t t = 2; t < n; ++t) cb(idx[0], idx[t - 1], idx[t]);
        return;
    }
    for (size_t i = 2; i < n; ++i) {
        if ((i & 1) == 0) cb(idx[i - 2], idx[i - 1], idx[i]);
        else              cb(idx[i - 1], idx[i - 2], idx[i]);
    }
}

static void xfPoint(const XForm& x, const float* v, float* out) {
    out[0] = x.m[0] * v[0] + x.m[1] * v[1] + x.m[2] * v[2] + x.t[0];
    out[1] = x.m[3] * v[0] + x.m[4] * v[1] + x.m[5] * v[2] + x.t[1];
    out[2] = x.m[6] * v[0] + x.m[7] * v[1] + x.m[8] * v[2] + x.t[2];
}
static void xfDir(const XForm& x, const float* v, float* out) {
    out[0] = x.m[0] * v[0] + x.m[1] * v[1] + x.m[2] * v[2];
    out[1] = x.m[3] * v[0] + x.m[4] * v[1] + x.m[5] * v[2];
    out[2] = x.m[6] * v[0] + x.m[7] * v[1] + x.m[8] * v[2];
}
// parent ∘ child
static XForm xfMul(const XForm& a, const XForm& b) {
    XForm r;
    r.identity = a.identity && b.identity;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            float v = 0;
            for (int k = 0; k < 3; ++k) v += a.m[row * 3 + k] * b.m[k * 3 + col];
            r.m[row * 3 + col] = v;
        }
    for (int row = 0; row < 3; ++row) {
        float v = a.t[row];
        for (int k = 0; k < 3; ++k) v += a.m[row * 3 + k] * b.t[k];
        r.t[row] = v;
    }
    return r;
}

bool B3D::parsePolys(size_t q, uint32_t npoly, Face& f, size_t& outQ) const {
    // Walk npoly material-poly records; each = {w0, 1.0f, 0x7fff, mat, cnt, <data>}
    // and the next header is found by scanning for the {1.0f, 0x7fff} marker
    // (the trailing arrays vary in stride: bare u32 indices, or expanded
    // idx+uv(+normals) entries), or by hitting a 555/444 closer.
    f.polys.clear();
    f.polys.reserve(npoly);
    for (uint32_t i = 0; i < npoly; ++i) {
        if (q + 20 > d.size()) return false;
        Poly p;
        p.color    = u32(q);
        p.material = u32(q + 12);
        uint32_t cnt = u32(q + 16);
        size_t dataStart = q + 20;
        size_t j = dataStart;
        size_t hi = d.size() > 8 ? d.size() - 8 : 0;
        size_t lim = dataStart + (1u << 20);
        if (hi > lim) hi = lim;
        size_t nq = (size_t)-1;
        bool last = (i + 1 == npoly);
        while (j < hi) {
            uint32_t v = u32(j);
            if ((v == T_ESEC || v == T_BSEC) && known(u32(j + 4))) break;
            if (!last && v == MARK_F && u32(j + 4) == MARK_T) { nq = j - 4; break; }
            j += 4;
        }
        size_t end = (nq != (size_t)-1) ? nq : j;
        // Decode indices.  Between two polygon headers the payload holds
        // cnt * stride dwords *plus* the next header's flags dword, so the
        // candidate strides are (dw-1)/cnt and dw/cnt; the vertex index is the
        // first dword of every entry.  Tested over 428,865 polygons of the
        // shipped data: with this rule 0 polygons yield an out-of-range vertex,
        // while the naive dw/cnt rule broke 15,905 of them (subtag-7 / fmt 0
        // meshes with stride 3/4/6 - mat_roadgrn, mat_troad4, mat_gravel,
        // mat_slope1/2, mat_stog, mat_lit_window), whose faces were then dropped
        // together with their texture coordinates.
        p.strip = (p.color & 0x80u) != 0;
        if (cnt > 0 && end >= dataStart) {
            size_t dw = (end - dataStart) / 4;
            uint32_t s = 1;
            if (dw > cnt && (dw - 1) % cnt == 0) s = (uint32_t)((dw - 1) / cnt);
            else if (dw >= cnt && dw % cnt == 0 && dw / cnt >= 1) s = (uint32_t)(dw / cnt);
            p.idx.reserve(cnt);
            for (uint32_t k = 0; k < cnt; ++k)
                p.idx.push_back(u32(dataStart + 4 * (size_t)s * k));
        }
        f.polys.push_back(std::move(p));
        q = end;
    }
    outQ = q;
    return true;
}

bool B3D::parseRecord(size_t p, Node& out) {
    if (u32(p) != T_REC) { err = "expect 333"; return false; }
    out = Node();
    out.off = p;
    out.name = name32(d.data() + p + 4, 32);
    uint32_t sub = u32(p + 36);
    out.subtag = sub;
    size_t q = p + 40;

    if (isMesh(sub)) {
        Mesh m;
        m.name = out.name;
        m.subtag = sub;
        if (q + 16 > d.size()) { err = "mesh bbox"; return false; }
        memcpy(m.bbox, d.data() + q, 16);
        q += 48;                                  // bbox[4f] + 8 dw
        uint32_t fmt = 0;
        if (sub == 37) { fmt = u32(q); q += 4; }
        m.fmt = fmt;
        m.vcount = u32(q); q += 4;
        size_t voff = q;
        uint32_t nf = nfm(fmt);
        if (nf == 0) {
            nf = strideOf(voff, m.vcount);
            if (nf == 0) { err = "mesh stride"; return false; }
        } else if (!okEnd(voff + 4 * (size_t)nf * m.vcount)) {
            uint32_t alt = strideOf(voff, m.vcount);
            if (alt) nf = alt;
        }
        m.stride = nf;
        size_t vbytes = 4 * (size_t)nf * m.vcount;
        if (voff + vbytes > d.size()) { err = "mesh verts"; return false; }
        m.verts.assign((const float*)(d.data() + voff),
                       (const float*)(d.data() + voff) + (size_t)nf * m.vcount);
        out.meshIndex = (int)meshList.size();
        meshList.push_back(std::move(m));
        q = voff + vbytes;
    } else if (polyHdrDw(sub)) {
        Face f;
        f.subtag = sub;
        if (q + 16 > d.size()) { err = "face bbox"; return false; }
        memcpy(f.bbox, d.data() + q, 16);
        int extra = polyHdrDw(sub) - 1;             // bbox + extra, npoly last
        if (extra >= 2) {
            f.color = u32(q + 16);
            f.material = u32(q + 20);
        } else if (extra == 1) {
            f.color = u32(q + 16);
        }
        q += (size_t)extra * 4;
        uint32_t npoly = u32(q); q += 4;
        if (!parsePolys(q, npoly, f, q)) return false;
        uint32_t v = u32(q);
        if (v != T_ESEC && v != T_BSEC) { err = "face close"; return false; }
        out.faceIndex = (int)faceList.size();
        faceList.push_back(std::move(f));
        out.end = skipClosers(q);
        return true;
    } else {
        int fp = fixedParams(sub);
        if (fp >= 0) {
            for (int i = 0; i < fp; ++i) out.params.push_back(u32(q + 4 * (size_t)i));
            q += 4 * (size_t)fp;
            uint32_t v = u32(q);
            if (v != T_ESEC && v != T_BSEC &&
                (v > 500000u || (u32(q + 4) != T_REC && u32(q + 4) != T_BSEC && u32(q + 4) != T_ESEC))) {
                size_t r = findCloser(p + 44);      // variable-length tail
                if (r == (size_t)-1) { err = "scan-fail fixed"; return false; }
                out.end = skipClosers(r + 4);
                return true;
            }
        } else if (isScan(sub)) {
            size_t r = q;
            size_t lim = q + (1u << 20);
            if (lim > d.size()) lim = d.size();
            while (r + 4 < d.size() && r < lim) {
                if (u32(r) == T_ESEC && known(u32(r + 4))) {
                    out.end = skipClosers(r + 4);
                    return true;
                }
                r += 4;
            }
            err = "scan-fail var";
            return false;
        } else {
            size_t r = findCloser(p + 44);
            if (r == (size_t)-1) { err = "unknown subtag"; return false; }
            out.end = skipClosers(r + 4);
            return true;
        }
    }

    uint32_t v = u32(q);
    if (v == T_ESEC || v == T_BSEC) {          // leaf: params then a closer
        out.end = skipClosers(q);
        return true;
    }
    uint32_t n = v;
    if (n > 500000u || (n && u32(q + 4) != T_REC && u32(q + 4) != T_BSEC)) {
        err = "bad count";
        return false;
    }
    q += 4;
    out.kids.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        q = skipClosers(q);
        Node kid;
        if (!parseRecord(q, kid)) return false;
        q = kid.end;
        out.kids.push_back(std::move(kid));
    }
    out.end = skipClosers(q);
    return true;
}

bool B3D::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { LOG("b3d: cannot open %s", path.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    d.resize((size_t)n);
    size_t got = fread(d.data(), 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { d.clear(); return false; }

    if (d.size() < 0x1C || memcmp(d.data(), "b3d\0", 4) != 0) {
        LOG("b3d: bad magic in %s", path.c_str());
        d.clear();
        return false;
    }
    uint32_t nnames = u32(0x18);
    size_t pos = 0x1C;
    nameTable.clear();
    nameTable.reserve(nnames);
    for (uint32_t i = 0; i < nnames; ++i) {
        if (pos + 32 > d.size()) break;
        nameTable.push_back(name32(d.data() + pos, 32));
        pos += 32;
    }
    size_t body = pos;
    topNodes.clear();
    meshList.clear();
    faceList.clear();
    if (u32(body) != T_OPEN) LOG("b3d: %s body is not 111 (0x%x)", path.c_str(), u32(body));
    pos += 4;
    size_t guard = 0;
    while (guard < d.size()) {
        guard += 4;
        uint32_t v = u32(pos);
        if (v == T_EOF) { pos += 4; break; }
        if (v == T_BSEC || v == T_ESEC || v == T_OPEN) { pos += 4; continue; }
        if (v != T_REC) { LOG("b3d: top stray %x @0x%zx", v, pos); break; }
        Node k;
        if (!parseRecord(pos, k)) {
            LOG("b3d: %s parse failed: %s @0x%zx", path.c_str(), err.c_str(), pos);
            d.clear();
            return false;
        }
        topNodes.push_back(std::move(k));
        pos = topNodes.back().end;
    }
    // collect anchors (24) and links (18) over the whole tree
    anchors.clear();
    links.clear();
    {
        struct Collect {
            B3D* self;
            void go(const Node& n) const {
                if (n.subtag == 24 && !n.name.empty() && n.params.size() >= 13) {
                    // king.exe stores the 9 floats as four rows of three and
                    // applies them as p' = p·M + t (row vector, row-major M).
                    // Our helpers use the column convention (p' = M·p), so the
                    // matrix is transposed once here.  GetAbsSpace composes
                    // local·parentAbs, which transposes to parentAbs·local —
                    // exactly what xfMul(parent, child) computes.
                    float raw[9];
                    for (int i = 0; i < 9; ++i) raw[i] = bitsToFloat(n.params[i]);
                    Anchor a;
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            a.m[r * 3 + c] = raw[c * 3 + r];
                    for (int i = 0; i < 3; ++i) a.t[i] = bitsToFloat(n.params[9 + i]);
                    self->anchors[n.name] = a;
                } else if (n.subtag == 18 && n.params.size() >= 20) {
                    std::string from = paramsName(n.params, 4);
                    std::string to = paramsName(n.params, 12);
                    if (!from.empty() && !to.empty())
                        self->links[to].push_back(from);
                }
                for (auto& k : n.kids) go(k);
            }
        } c{this};
        for (auto& n : topNodes) c.go(n);
    }
    anchorList.assign(anchors.begin(), anchors.end());
    linkList.assign(links.begin(), links.end());
    nodeIndex.clear();
    {
        struct Idx {
            B3D* self;
            void go(const Node& n) const {
                if (!n.name.empty() && !self->nodeIndex.count(n.name))
                    self->nodeIndex[n.name] = &n;
                for (auto& k : n.kids) go(k);
            }
        } i{this};
        for (auto& n : topNodes) i.go(n);
    }

    totalBytes = d.size() - body;
    consumedBytes = pos - body;
    LOG("b3d: %s names=%u top=%zu meshes=%zu faces=%zu anchors=%zu links=%zu consumed %.2f%%",
        path.c_str(), nnames, topNodes.size(), meshList.size(), faceList.size(),
        anchors.size(), links.size(),
        100.0 * (double)consumedBytes / (double)(totalBytes ? totalBytes : 1));
    return true;
}

std::vector<uint32_t> B3D::subtagHistogram() const {
    std::vector<uint32_t> h(256, 0);
    struct W {
        const std::vector<Node>* v;
        std::vector<uint32_t>* h;
        void go(const Node& n) const {
            if (n.subtag < h->size()) (*h)[n.subtag]++;
            for (auto& k : n.kids) go(k);
        }
    } w{&topNodes, &h};
    for (auto& n : topNodes) w.go(n);
    return h;
}

bool B3D::anchorOf(const std::string& name, Anchor& out) const {
    auto it = anchors.find(name);
    if (it == anchors.end()) return false;
    out = it->second;
    return true;
}

bool B3D::setAnchor(const std::string& name, const Anchor& a) {
    auto it = anchors.find(name);
    if (it == anchors.end()) return false;
    it->second = a;
    // anchorList is the public mirror the editor reads; keep both in sync.
    for (auto& p : anchorList)
        if (p.first == name) { p.second = a; return true; }
    anchorList.emplace_back(name, a);
    return true;
}

void B3D::buildScene() {
    sceneName_ = "";
    std::vector<const Node*> roots;
    for (auto& n : topNodes) roots.push_back(&n);
    buildFrom(roots, /*skipLinked=*/true);
}

bool B3D::buildNamedScene(const std::string& rootName, int bodyState) {
    sceneName_ = rootName;
    std::vector<const Node*> roots;
    for (auto& n : topNodes)
        if (n.name == rootName) roots.push_back(&n);
    if (roots.empty()) {
        LOG("b3d: no top-level container named '%s'", rootName.c_str());
        subs.clear();
        return false;
    }
    buildFrom(roots, /*skipLinked=*/false, bodyState);
    return !subs.empty();
}

void B3D::buildFrom(const std::vector<const Node*>& roots, bool skipLinked, int bodyState) {
    subs.clear();
    usedAnchors.clear();
    bbMin[0] = bbMin[1] = bbMin[2] = 1e30f;
    bbMax[0] = bbMax[1] = bbMax[2] = -1e30f;
    if (meshList.empty() || faceList.empty()) return;

    // 0) attach every face node to the enclosing mesh record
    std::vector<std::vector<int>> meshFaces(meshList.size());
    {
        struct Attach {
            std::vector<std::vector<int>>* mf;
            void go(const Node& n, int curMesh) const {
                if (n.meshIndex >= 0) curMesh = n.meshIndex;
                if (n.faceIndex >= 0 && curMesh >= 0) (*mf)[curMesh].push_back(n.faceIndex);
                for (auto& k : n.kids) go(k, curMesh);
            }
        } at{&meshFaces};
        for (auto& n : topNodes) at.go(n, -1);
    }

    // 1) per-mesh smooth normals from every face that references the mesh
    for (size_t mi = 0; mi < meshList.size(); ++mi) {
        Mesh& m = meshList[mi];
        if (!m.normals.empty()) continue;                 // computed once
        m.normals.assign((size_t)m.vcount * 3, 0.0f);
        std::vector<double> acc((size_t)m.vcount * 3, 0.0);
        for (int fi : meshFaces[mi]) {
            const Face& fc = faceList[(size_t)fi];
            for (auto& p : fc.polys) {
                triangulate(p.idx, p.strip, [&](uint32_t a, uint32_t b, uint32_t c) {
                    if (a >= m.vcount || b >= m.vcount || c >= m.vcount) return;
                    const float* pa = &m.verts[(size_t)a * m.stride];
                    const float* pb = &m.verts[(size_t)b * m.stride];
                    const float* pc = &m.verts[(size_t)c * m.stride];
                    double e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
                    double e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
                    double nx = e1[1] * e2[2] - e1[2] * e2[1];
                    double ny = e1[2] * e2[0] - e1[0] * e2[2];
                    double nz = e1[0] * e2[1] - e1[1] * e2[0];
                    const uint32_t vs[3] = {a, b, c};
                    for (int k = 0; k < 3; ++k) {
                        acc[(size_t)vs[k] * 3 + 0] += nx;
                        acc[(size_t)vs[k] * 3 + 1] += ny;
                        acc[(size_t)vs[k] * 3 + 2] += nz;
                    }
                });
            }
        }
        for (uint32_t i = 0; i < m.vcount; ++i) {
            double nx = acc[(size_t)i * 3 + 0], ny = acc[(size_t)i * 3 + 1], nz = acc[(size_t)i * 3 + 2];
            double l = sqrt(nx * nx + ny * ny + nz * nz);
            if (l < 1e-12) { nx = 0; ny = 1; nz = 0; l = 1; }
            m.normals[(size_t)i * 3 + 0] = (float)(nx / l);
            m.normals[(size_t)i * 3 + 1] = (float)(ny / l);
            m.normals[(size_t)i * 3 + 2] = (float)(nz / l);
        }
    }

    // 2) flatten.  A subtag-18 record places the container it names at the anchor
    //    it names; containers that are link targets are skipped where they stand.
    struct Walker {
        B3D* self;
        bool skipLinked;
        int selectedBody;
        int instances = 0;
        std::vector<std::pair<std::string, std::string>> emitted;   // (target, anchor)

        bool isLinkedTarget(const std::string& name) const {
            return !name.empty() && self->links.count(name) != 0;
        }
        // A container that is both present in the tree and linked onto the same
        // spot (or linked twice) is emitted twice, byte for byte.  On the road
        // that is two copies of the same asphalt, and the depth test flips
        // between them as the camera moves — the carriageway "shakes".
        std::vector<uint64_t> placed;
        int dropped = 0;
        static uint64_t fingerprint(const SubMesh& sm) {
            uint64_t h = 14695981039346656037ull;
            auto mix = [&](uint32_t v) { h ^= v; h *= 1099511628211ull; };
            auto mixStr = [&](const std::string& s) {
                mix((uint32_t)s.size());
                for (unsigned char c : s) mix(c);
            };
            mixStr(sm.group);
            mixStr(sm.material);
            mix((uint32_t)sm.pos.size());
            mix((uint32_t)sm.uv.size());
            mix((uint32_t)sm.idx.size());
            for (float f : sm.pos) { uint32_t u; std::memcpy(&u, &f, 4); mix(u); }
            for (float f : sm.uv) { uint32_t u; std::memcpy(&u, &f, 4); mix(u); }
            for (uint32_t i : sm.idx) mix(i);
            return h;
        }
        bool coincident(const SubMesh& sm, uint64_t h) const {
            for (size_t i = 0; i < placed.size(); ++i) {
                if (placed[i] != h) continue;
                const SubMesh& o = self->subs[i];
                if (o.group == sm.group && o.material == sm.material &&
                    o.pos == sm.pos && o.uv == sm.uv && o.idx == sm.idx)
                    return true;
            }
            return false;
        }

        void go(const Node& n, int curMesh, const XForm& xf, int depth,
                const std::string& group = std::string(), int body = -1, int tyre = -1) {
            // a subtag-21 record is a "key" node: everything below it can be
            // toggled by the scripts (people by day, roadworks, floods, ...)
            const std::string grp = (n.subtag == 21 && !n.name.empty()) ? n.name : group;
            if (depth > 8) return;                        // cycle / runaway guard
            if (selectedBody >= 0 && n.subtag == 21 && !n.kids.empty()) {
                if (n.name == "TestKey") return; // authoring helper, not vehicle geometry
                if (n.name.find("Damage") != std::string::npos) {
                    const bool wheel = n.name.find("Wheel") != std::string::npos;
                    const int state = wheel ? 0 : std::min(selectedBody, (int)n.kids.size() - 1);
                    go(n.kids[(size_t)state], curMesh, xf, depth, grp,
                       wheel ? body : state, wheel ? state : tyre);
                    return;
                }
                // These children are alternative LODs, not cumulative parts.
                if (n.name == "refer_GeometryKey") {
                    const Node* lod = nullptr;
                    float nearest = 1e30f;
                    for (const auto& child : n.kids)
                        if (child.subtag == 10 && child.params.size() >= 8) {
                            const float distance = bitsToFloat(child.params[7]);
                            if (distance < nearest) { nearest = distance; lod = &child; }
                        }
                    if (lod) { go(*lod, curMesh, xf, depth, grp, body, tyre); return; }
                }
            }
            if (!n.name.empty() && skipLinked && depth == 0 && isLinkedTarget(n.name))
                return;                                   // drawn at its anchors instead
            if (n.name.rfind("hit_", 0) == 0 && depth == 0)
                return;                                   // collision-only geometry
            if (n.meshIndex >= 0) curMesh = n.meshIndex;
            if (n.faceIndex >= 0 && curMesh >= 0) emit(curMesh, n.faceIndex, n.name, xf, grp, body, tyre);

            if (n.subtag == 18 && n.params.size() >= 20) {
                // "link": include the named container.  With an anchor name it is
                // placed at that anchor's transform; with an empty (or unknown)
                // anchor name it is included at the current frame (this is how a
                // vehicle pulls in its variant parts, whose geometry is already
                // in vehicle coordinates).
                std::string from = paramsName(n.params, 4);
                std::string to = paramsName(n.params, 12);
                auto ib = self->nodeIndex.find(to);
                if (ib != self->nodeIndex.end() && ib->second != &n) {
                    // The data contains duplicate 18 records for the same
                    // (container, anchor) pair -> pure overdraw.  Only a NAMED
                    // anchor is safe to dedup: an empty anchor means "at the
                    // current frame", so the same pair legitimately occurs under
                    // differently placed parents (e.g. left/right wheel).
                    if (!from.empty()) {
                        std::pair<std::string, std::string> key(to, from);
                        bool dup = false;
                        for (auto& p : emitted) if (p == key) { dup = true; break; }
                        if (dup) return;
                        emitted.push_back(key);
                    }
                    XForm comb = xf;
                    auto ia = self->anchors.find(from);
                    if (!from.empty() && ia != self->anchors.end()) {
                        XForm local;
                        memcpy(local.m, ia->second.m, sizeof(local.m));
                        memcpy(local.t, ia->second.t, sizeof(local.t));
                        local.identity = false;
                        comb = xfMul(xf, local);
                        // The editor draws these (tools/editor/ModelEditor.cpp);
                        // the container also holds every other vehicle's anchors.
                        bool seen = false;
                        for (const std::string& u : self->usedAnchors)
                            if (u == from) { seen = true; break; }
                        if (!seen) self->usedAnchors.push_back(from);
                    }
                    ++instances;
                    go(*ib->second, -1, comb, depth + 1, grp, body, tyre);
                }
                return;
            }
            for (auto& k : n.kids) {
                if (selectedBody >= 0 && n.subtag == 10 && k.subtag == 10) continue;
                go(k, curMesh, xf, depth, grp, body, tyre);
            }
        }

        void emit(int mi, int fi, const std::string& nodeName, const XForm& xf,
                  const std::string& group, int body, int tyre) {
            Mesh& m = self->meshList[mi];
            const Face& f = self->faceList[fi];
            std::vector<uint32_t> mats;
            for (auto& p : f.polys) {
                bool found = false;
                for (uint32_t mm : mats) if (mm == p.material) { found = true; break; }
                if (!found) mats.push_back(p.material);
            }
            for (uint32_t mat : mats) {
                SubMesh sm;
                sm.name = nodeName.empty() ? m.name : nodeName;
                sm.group = group;
                sm.damageState = body;
                sm.tyreState = tyre;
                sm.materialIndex = mat;
                if (mat < self->nameTable.size()) sm.material = self->nameTable[mat];
                std::vector<uint32_t> remap((size_t)m.vcount, 0xFFFFFFFFu);
                for (auto& p : f.polys) {
                    if (p.material != mat) continue;
                    triangulate(p.idx, p.strip, [&](uint32_t A, uint32_t B, uint32_t C) {
                        const uint32_t tri[3] = {A, B, C};
                        for (int k = 0; k < 3; ++k) {
                            uint32_t vi = tri[k];
                            if (vi >= m.vcount) continue;
                            uint32_t li = remap[vi];
                            if (li == 0xFFFFFFFFu) {
                                li = (uint32_t)(sm.pos.size() / 3);
                                remap[vi] = li;
                                const float* v = &m.verts[(size_t)vi * m.stride];
                                float p3[3] = {v[0], v[1], v[2]}, q3[3];
                                xfPoint(xf, p3, q3);
                                sm.pos.push_back(q3[0]); sm.pos.push_back(q3[1]); sm.pos.push_back(q3[2]);
                                const float* nn = &m.normals[(size_t)vi * 3];
                                float n3[3] = {nn[0], nn[1], nn[2]}, nq[3];
                                xfDir(xf, n3, nq);
                                float nl = sqrtf(nq[0]*nq[0] + nq[1]*nq[1] + nq[2]*nq[2]);
                                if (nl > 1e-6f) { nq[0] /= nl; nq[1] /= nl; nq[2] /= nl; }
                                sm.nrm.push_back(nq[0]); sm.nrm.push_back(nq[1]); sm.nrm.push_back(nq[2]);
                                int uo = uvOffsetOf(m.subtag, m.fmt, m.stride);
                                float u = 0, vv = 0;
                                if (uo >= 0 && (uint32_t)(uo + 1) < m.stride) { u = v[uo]; vv = v[uo + 1]; }
                                sm.uv.push_back(u); sm.uv.push_back(vv);
                            }
                            sm.idx.push_back(li);
                        }
                    });
                }
                if (sm.idx.empty()) continue;
                float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
                for (size_t i = 0; i + 2 < sm.pos.size(); i += 3)
                    for (int k = 0; k < 3; ++k) {
                        mn[k] = std::min(mn[k], sm.pos[i + k]);
                        mx[k] = std::max(mx[k], sm.pos[i + k]);
                    }
                for (int k = 0; k < 3; ++k) {
                    sm.center[k] = 0.5f * (mn[k] + mx[k]);
                    self->bbMin[k] = std::min(self->bbMin[k], mn[k]);
                    self->bbMax[k] = std::max(self->bbMax[k], mx[k]);
                }
                float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
                sm.radius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
                const uint64_t h = fingerprint(sm);
                if (coincident(sm, h)) { ++dropped; continue; }
                placed.push_back(h);
                self->subs.push_back(std::move(sm));
            }
        }
    } w{this, skipLinked, bodyState};
    XForm ident;
    for (const Node* n : roots) w.go(*n, -1, ident, 0);
    // A show-group often stores the same patch twice: a dense mesh and a coarse
    // one with the same centre, the same radius and the same material.  Both
    // were drawn, and on a slope the coarse chords cross the dense surface by
    // a few centimetres.  At a grazing angle that crossing band is a metre wide
    // on screen and flips as the camera moves.  The dense mesh is the one to
    // keep; the coarse one exists only as a distance stand-in, and drawing it
    // on top of the dense mesh is what shakes the carriageway and the verge.
    if (bodyState < 0) {
        std::vector<char> drop(subs.size(), 0);
        int coarse = 0;
        for (size_t b = 0; b < subs.size(); ++b) {
            const SubMesh& B = subs[b];
            if (B.material.empty() || B.idx.size() < 3) continue;
            for (size_t a = 0; a < subs.size(); ++a) {
                if (a == b || drop[a]) continue;
                const SubMesh& A = subs[a];
                if (A.material != B.material || A.group != B.group) continue;
                if (A.idx.size() < B.idx.size() * 2) continue;
                float dx = A.center[0] - B.center[0];
                float dz = A.center[1] - B.center[1];
                float dy = A.center[2] - B.center[2];
                float lim = std::max(5.0f, 0.08f * A.radius);
                if (dx * dx + dz * dz > lim * lim || std::fabs(dy) > 2.0f) continue;
                float rr = A.radius > 1.0f ? B.radius / A.radius : 1.0f;
                if (rr < 0.85f || rr > 1.15f) continue;
                // Same sphere is not enough: two neighbouring patches of one
                // material can share it.  The coarse mesh is a cover only when
                // its own triangles lie on the dense mesh.  The bbox centre is
                // the wrong probe — a road ribbon's centre is often off the
                // asphalt — so this samples the coarse triangles themselves.
                int samples = 0, hits = 0;
                const size_t step = std::max<size_t>(3, (B.idx.size() / 3 / 8) * 3);
                for (size_t bi = 0; bi + 2 < B.idx.size(); bi += step) {
                    const uint32_t i0 = B.idx[bi], i1 = B.idx[bi + 1], i2 = B.idx[bi + 2];
                    if ((i0 + 1) * 3 > B.pos.size() || (i1 + 1) * 3 > B.pos.size() ||
                        (i2 + 1) * 3 > B.pos.size()) continue;
                    const float qx = (B.pos[i0 * 3] + B.pos[i1 * 3] + B.pos[i2 * 3]) / 3.0f;
                    const float qz = (B.pos[i0 * 3 + 1] + B.pos[i1 * 3 + 1] + B.pos[i2 * 3 + 1]) / 3.0f;
                    const float qy = (B.pos[i0 * 3 + 2] + B.pos[i1 * 3 + 2] + B.pos[i2 * 3 + 2]) / 3.0f;
                    ++samples;
                    for (size_t ti = 0; ti + 2 < A.idx.size(); ti += 3) {
                        const uint32_t ia = A.idx[ti], ib = A.idx[ti + 1], ic = A.idx[ti + 2];
                        if ((ia + 1) * 3 > A.pos.size() || (ib + 1) * 3 > A.pos.size() ||
                            (ic + 1) * 3 > A.pos.size()) continue;
                        const float ax = A.pos[ia * 3], az = A.pos[ia * 3 + 1], ay = A.pos[ia * 3 + 2];
                        const float bx = A.pos[ib * 3], bz = A.pos[ib * 3 + 1], by = A.pos[ib * 3 + 2];
                        const float cx = A.pos[ic * 3], cz = A.pos[ic * 3 + 1], cy = A.pos[ic * 3 + 2];
                        const float v0x = bx - ax, v0z = bz - az;
                        const float v1x = cx - ax, v1z = cz - az;
                        const float v2x = qx - ax, v2z = qz - az;
                        const float den = v0x * v1z - v0z * v1x;
                        if (std::fabs(den) < 1e-4f) continue;
                        const float v = (v2x * v1z - v2z * v1x) / den;
                        const float w = (v0x * v2z - v0z * v2x) / den;
                        const float u = 1.0f - v - w;
                        if (u < -1e-3f || v < -1e-3f || w < -1e-3f) continue;
                        const float py = u * ay + v * by + w * cy;
                        if (std::fabs(py - qy) < 0.5f) { ++hits; break; }
                    }
                }
                if (samples < 1 || hits * 4 < samples * 3) continue;
                drop[b] = 1;
                ++coarse;
                break;
            }
        }
        if (coarse) {
            std::map<std::string, int> byMat;
            for (size_t i = 0; i < subs.size(); ++i)
                if (drop[i]) ++byMat[subs[i].material];
            std::vector<SubMesh> kept;
            kept.reserve(subs.size() - (size_t)coarse);
            for (size_t i = 0; i < subs.size(); ++i)
                if (!drop[i]) kept.push_back(std::move(subs[i]));
            subs.swap(kept);
            std::string hist;
            for (const auto& kv : byMat) {
                if (!hist.empty()) hist += " ";
                hist += kv.first + ":" + std::to_string(kv.second);
            }
            LOG("b3d: %d coarse covers dropped (%s)", coarse, hist.c_str());
        }
    }
    collectHitProxies();

    LOG("b3d scene%s: %zu submeshes, %d placements, %d coincident duplicates dropped, "
        "bbox (%.1f %.1f %.1f)..(%.1f %.1f %.1f)",
        sceneName_.empty() ? "" : (" '" + sceneName_ + "'").c_str(),
        subs.size(), w.instances, w.dropped, bbMin[0], bbMin[1], bbMin[2], bbMax[0], bbMax[1], bbMax[2]);
}

// The authored collision proxies: top-level `hit_*` containers hold the shapes the
// engine collides with (docs/hit_proxies.md).  They are never drawn, so this runs
// as its own walk over the record tree and does not disturb the render list.
void B3D::collectHitProxies() {
    hitProxies_.clear();
    if (d.empty()) return;
    // A plane record nests further plane records (each with its own subtag-23
    // extents), so both are walked recursively: the collision of one object is a
    // *tree* of bounded surfaces.
    std::function<void(const Node&, std::vector<float>&, bool)> extents =
        [&](const Node& n, std::vector<float>& ring, bool root) {
            if (!root && n.subtag == 9) return;        // a nested plane is its own wall
            if (n.subtag == 23) {
                size_t pos = n.off + 40 + 16;          // the count sits at +16
                while (pos + 4 <= n.end && pos + 4 <= d.size()) {
                    uint32_t cnt = 0;
                    memcpy(&cnt, d.data() + pos, 4);
                    if (cnt < 2 || cnt > 256) break;
                    const size_t p = pos + 4;
                    if (p + 12 * (size_t)cnt > n.end || p + 12 * (size_t)cnt > d.size()) break;
                    for (uint32_t v = 0; v < cnt; ++v) {
                        float p3[3];
                        memcpy(p3, d.data() + p + 12 * (size_t)v, 12);
                        ring.push_back(p3[0]);
                        ring.push_back(p3[1]);
                        ring.push_back(p3[2]);
                    }
                    pos = p + 12 * (size_t)cnt;
                }
            }
            for (const Node& k : n.kids) extents(k, ring, false);
        };
    std::function<void(const Node&, std::vector<Wall>&)> planes =
        [&](const Node& n, std::vector<Wall>& out) {
            if (n.subtag == 9 && n.params.size() >= 8) {
                Wall w;
                for (int i = 0; i < 4; ++i) w.n[i] = bitsToFloat(n.params[4 + i]);
                extents(n, w.ring, true);
                out.push_back(std::move(w));
            }
            for (const Node& k : n.kids) planes(k, out);
        };
    struct Walker {
        B3D* self;
        const std::function<void(const Node&, std::vector<Wall>&)>* planes = nullptr;
        void go(const Node& n) {
            if (n.name.rfind("hit_", 0) == 0 && !n.kids.empty()) {
                HitProxy p;
                p.name = n.name;
                const std::string rest = n.name.substr(4);        // obj_ap_048 / road_ap_048
                if (rest.rfind("obj_", 0) == 0) p.object = "obj_" + rest.substr(4);
                else if (rest.rfind("road_", 0) == 0) p.object = "obj_" + rest.substr(5);
                else p.object = rest;
                for (const Node& k : n.kids) {
                    if (k.subtag == 20) {
                        // 8 header dwords, then count x (x, z, y); count = header[4]
                        const size_t base = k.off + 40;
                        if (base + 32 > self->d.size()) continue;
                        uint32_t cnt = 0;
                        memcpy(&cnt, self->d.data() + base + 16, 4);
                        if (cnt < 2 || cnt > 256) continue;
                        if (base + 32 + 12 * (size_t)cnt > self->d.size()) continue;
                        if (base + 32 + 12 * (size_t)cnt > k.end) continue;
                        for (uint32_t v = 0; v < cnt; ++v) {
                            float p3[3];
                            memcpy(p3, self->d.data() + base + 32 + 12 * (size_t)v, 12);
                            if (cnt == 2) {
                                p.segments.push_back(p3[0]);
                                p.segments.push_back(p3[1]);
                            } else {
                                p.poly.push_back(p3[0]);
                                p.poly.push_back(p3[1]);
                                p.poly.push_back(p3[2]);
                            }
                        }
                    } else if (k.subtag == 9) {
                        if (planes) (*planes)(k, p.walls);   // the proxy's plane tree
                    } else if (k.subtag == 12 && k.params.size() >= 8) {
                        p.yMin = -bitsToFloat(k.params[7]);           // params[7] = -y_min
                        p.haveBottom = true;
                    } else if (k.subtag == 18 && k.params.size() >= 20) {
                        p.linkAnchor = paramsName(k.params, 4);
                        p.linkTarget = paramsName(k.params, 12);
                    }
                }
                for (Wall& w : p.walls) {          // the proxy's bottom applies to its walls
                    w.yMin = p.yMin;
                    w.haveBottom = p.haveBottom;
                }
                self->hitProxies_.push_back(std::move(p));
            }
            for (const Node& k : n.kids) go(k);
        }
    } w{this, &planes};
    for (const Node& n : topNodes) w.go(n);
}

} // namespace ht2
