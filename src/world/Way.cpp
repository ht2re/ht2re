#include "Way.h"
#include "../util/Log.h"
#include <cstdio>
#include <cstring>

namespace ht2 {

static bool readU32(FILE* f, uint32_t& v) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    v = (uint32_t)(b[0]) | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static std::string assignStr(const uint8_t* d, uint32_t len) {
    std::string s((const char*)d, len);
    size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    return s;
}

static void walk(const std::vector<uint8_t>& body, size_t p, size_t end,
                 Way::Room* room, std::vector<Way::Room>& rooms,
                 std::vector<Way::Seg>& segs, std::string& mapName) {
    while (p + 8 <= end) {
        char tag[5] = {0};
        memcpy(tag, body.data() + p, 4);
        uint32_t n;
        memcpy(&n, body.data() + p + 4, 4);
        const uint8_t* data = body.data() + p + 8;
        size_t dataEnd = p + 8 + n;
        if (dataEnd > end) break;

        if (!memcmp(tag, "MNAM", 4)) {
            mapName = assignStr(data, n);
        } else if (!memcmp(tag, "RNAM", 4)) {
            rooms.emplace_back();
            room = &rooms.back();
            room->name = assignStr(data, n);
        } else if (!memcmp(tag, "RNOD", 4)) {
            Way::Node nd;
            for (size_t cp = p + 8; cp + 8 <= dataEnd;) {
                char ctag[5] = {0};
                memcpy(ctag, body.data() + cp, 4);
                uint32_t cn;
                memcpy(&cn, body.data() + cp + 4, 4);
                const uint8_t* cdata = body.data() + cp + 8;
                if (cp + 8 + cn > dataEnd) break;
                if (!memcmp(ctag, "NNAM", 4)) nd.name = assignStr(cdata, cn);
                else if (!memcmp(ctag, "POSN", 4) && cn == 24) {
                    // 3 raw LE doubles (king.exe parser 0x488990: 3x strided 8-byte
                    // copy, stored as qword pair + 2 qwords). v0=x, v1=z, v2=height.
                    memcpy(&nd.pos.x, cdata + 0, 8);
                    memcpy(&nd.pos.z, cdata + 8, 8);
                    memcpy(&nd.pos.y, cdata + 16, 8);
                } else if (!memcmp(ctag, "FLAG", 4) && cn >= 4) {
                    memcpy(&nd.flags, cdata, 4);
                }
                cp += 8 + ((cn + 3) & ~3u);
            }
            if (room) room->nodes.push_back(nd);
        } else if (!memcmp(tag, "RSEG", 4)) {
            // RSEG = sub-chunk container: ATTR / RTEN / WDTH / VDAT
            Way::Seg sg;
            for (size_t cp = p + 8; cp + 8 <= dataEnd;) {
                char ctag[5] = {0};
                memcpy(ctag, body.data() + cp, 4);
                uint32_t cn;
                memcpy(&cn, body.data() + cp + 4, 4);
                const uint8_t* cdata = body.data() + cp + 8;
                if (cp + 8 + cn > dataEnd) break;
                if (!memcmp(ctag, "ATTR", 4) && cn >= 4) {
                    memcpy(&sg.attr, cdata, 4);
                } else if (!memcmp(ctag, "RTEN", 4)) {
                    sg.name = assignStr(cdata, cn);
                } else if (!memcmp(ctag, "WDTH", 4) && cn >= 8) {
                    // king.exe 0x488f93: reads 8 B double -> width; second double
                    // (when present) stored separately (taper end?)
                    memcpy(&sg.width, cdata, 8);
                    if (cn >= 16) memcpy(&sg.width1, cdata + 8, 8);
                } else if (!memcmp(ctag, "VDAT", 4) && cn >= 4) {
                    // u32 count + count * 24 B records, same 3-doubles unpack
                    // as POSN (king.exe: count + rep movsd 6 into vector [+0x10])
                    uint32_t cnt;
                    memcpy(&cnt, cdata, 4);
                    if (cnt && cn >= 4 + (size_t)cnt * 24) {
                        sg.pts.reserve(cnt);
                        for (uint32_t k = 0; k < cnt; ++k) {
                            Way::Pos pt;
                            memcpy(&pt.x, cdata + 4 + k * 24 + 0, 8);
                            memcpy(&pt.z, cdata + 4 + k * 24 + 8, 8);
                            memcpy(&pt.y, cdata + 4 + k * 24 + 16, 8);
                            sg.pts.push_back(pt);
                        }
                    }
                }
                cp += 8 + ((cn + 3) & ~3u);
            }
            if (!sg.pts.empty()) {
                LOG("way: seg %-28s attr=%d w=%.1f/%.1f pts=%zu", sg.name.c_str(),
                    sg.attr, sg.width, sg.width1, sg.pts.size());
                segs.push_back(std::move(sg));
            }
        } else if (!memcmp(tag, "GDAT", 4) || !memcmp(tag, "GROM", 4)) {
            walk(body, p + 8, dataEnd, room, rooms, segs, mapName);
        }
        size_t dataEndAligned = dataEnd + ((4 - (dataEnd & 3)) & 3);
        p = dataEndAligned;
    }
}

bool Way::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { LOG("way: cannot open %s", path.c_str()); return false; }

    char magic[4];
    uint32_t total = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "WTWR", 4) != 0) { fclose(f); return false; }
    if (!readU32(f, total)) { fclose(f); return false; }

    std::vector<uint8_t> body(total);
    if (total && fread(body.data(), 1, total, f) != total) { fclose(f); return false; }
    fclose(f);

    walk(body, 0, body.size(), nullptr, rooms, segs, mapName);
    size_t totalPts = 0;
    for (auto& s : segs) totalPts += s.pts.size();
    LOG("way: %s loaded, map=%s rooms=%zu nodes=%zu segs=%zu pts=%zu", path.c_str(),
        mapName.c_str(), rooms.size(), totalNodes(), segs.size(), totalPts);
    for (auto& r : rooms)
        for (auto& n : r.nodes)
            LOG("way: node %s pos=(%.1f, %.1f, %.1f) flags=0x%x", n.name.c_str(), n.pos.x, n.pos.z, n.pos.y, n.flags);
    return true;
}

size_t Way::totalNodes() const {
    size_t s = 0;
    for (auto& r : rooms) s += r.nodes.size();
    return s;
}

} // namespace ht2