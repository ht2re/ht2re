#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace ht2 {

// .way chunked container: WTWR magic; chunks <tag[4]><u32 size><data>, 4-aligned.
// GDAT/GROM contain the road-graph tree (rooms -> nodes).
// POSN (24 B) = 3 raw little-endian doubles (confirmed vs king.exe parser
// 0x488990/0x488c1e..c4a; NOT encrypted). Provisional mapping: v0=x, v1=z,
// v2=height — height-scale calibration against terrain is M5 work.
// FLAG = 4-byte dword (seen: 0 / 0x10100 / 0x10700).
class Way {
public:
    struct Pos { double x = 0, y = 0, z = 0; };
    struct Node {
        std::string name;
        Pos pos;
        uint32_t flags = 0;   // FLAG payload
    };
    struct Room {
        std::string name;
        std::vector<Node> nodes;
    };
    // RSEG (road segment): sub-chunks ATTR (4-B int params), RTEN (name),
    // WDTH (width double(s)), VDAT (u32 count + count*24 B = polyline points).
    struct Seg {
        std::string name;      // RTEN
        int attr = 0;          // ATTR dword
        double width = 8.0;    // WDTH first double
        double width1 = 0.0;   // WDTH second double (0 if absent)
        std::vector<Pos> pts;  // VDAT polyline
    };

    std::string mapName;      // from MNAM
    std::vector<Room> rooms;
    std::vector<Seg> segs;    // all RSEG (flat, order of appearance)

    bool load(const std::string& path);
    size_t totalNodes() const;
    const std::vector<Seg>& segments() const { return segs; }
};

} // namespace ht2