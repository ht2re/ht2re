#include "Res.h"
#include "../util/Log.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace ht2 {

static bool ieq(const std::string& a, const char* b) {
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

static bool endsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

const Res::Entry* Res::find(const std::string& name) const {
    for (const auto& e : ents)
        if (ieq(e.name, name.c_str())) return &e;
    return nullptr;
}

bool Res::load(const std::string& path) {
    srcPath = path;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { LOG("res: cannot open %s", path.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    d.resize((size_t)n);
    size_t got = fread(d.data(), 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { d.clear(); return false; }

    ents.clear();
    size_t pos = 0;
    auto readStr = [&](std::string& out) -> bool {
        if (pos >= d.size()) return false;
        size_t z = pos;
        while (z < d.size() && d[z]) ++z;
        if (z >= d.size()) return false;
        out.assign((const char*)d.data() + pos, z - pos);
        pos = z + 1;
        return true;
    };

    while (pos < d.size()) {
        std::string key;
        size_t keyStart = pos;
        if (!readStr(key)) break;
        // A section header looks like "<KEY> <count>" and is followed by payload
        // for one of the known sections; the file ends after the last payload.
        size_t sp = key.find(' ');
        if (sp == std::string::npos) {
            LOG("res: %s: unexpected token '%s' at 0x%zx", path.c_str(), key.c_str(), keyStart);
            break;
        }
        std::string k = key.substr(0, sp);
        long count = strtol(key.c_str() + sp + 1, nullptr, 10);
        if (count < 0) count = 0;

        if (ieq(k, "COLORS")) {
            for (long i = 0; i < count; ++i) { std::string s; if (!readStr(s)) break; }
            continue;
        }
        Kind kind = Kind::Unknown;
        if (endsWith(k, "FILES")) {
            if (ieq(k, "MASKFILES")) kind = Kind::Mask;
            else if (ieq(k, "TEXTUREFILES")) kind = Kind::Texture;
            else if (ieq(k, "PALETTEFILES")) kind = Kind::Palette;
            else if (ieq(k, "SOUNDFILES")) kind = Kind::Sound;
        } else {
            // "SOUNDS <n>" and friends carry no inline payload
            continue;
        }
        for (long i = 0; i < count; ++i) {
            std::string nm;
            if (!readStr(nm)) {
                LOG("res: %s: truncated %s at entry %ld", path.c_str(), k.c_str(), i);
                break;
            }
            if (pos + 4 > d.size()) break;
            Entry e;
            e.kind = kind;
            size_t nameSp = nm.find(' ');
            if (nameSp != std::string::npos) { e.name = nm.substr(0, nameSp); e.flags = nm.substr(nameSp + 1); }
            else e.name = nm;
            e.sizeOff = pos;
            memcpy(&e.size, d.data() + pos, 4);
            pos += 4;
            if (pos + 4 > d.size()) break;
            e.tagOff = pos;
            e.tag.assign((const char*)d.data() + pos, 4);
            while (!e.tag.empty() && (e.tag.back() == '\0' || e.tag.back() == ' ')) e.tag.pop_back();
            e.dataOff = pos + 4;
            e.dataSize = e.size >= 4 ? e.size - 4 : 0;
            if (e.dataOff + e.dataSize > d.size()) {
                LOG("res: %s: entry %s overruns file (%u bytes)", path.c_str(), e.name.c_str(), e.size);
                e.dataSize = (uint32_t)(d.size() - e.dataOff);
            }
            ents.push_back(e);
            pos = e.dataOff + e.dataSize;
        }
    }
    end = pos;
    LOG("res: %s entries=%zu parsed=%.2f%% of %zu B", path.c_str(), ents.size(),
        100.0 * (double)pos / (double)d.size(), d.size());
    return true;
}

} // namespace ht2
