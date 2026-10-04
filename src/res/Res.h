#pragma once
// .RES container (LOAD.RES, *.res) — see NOTES.md.
//
// Text header of NUL-separated tokens, interleaved with the payloads:
//   "COLORS <n>\0"      <n> float strings
//   "SOUNDFILES <n>\0"  <n> file entries
//   "SOUNDS <n>\0"      (count only)
//   "MASKFILES <n>\0"   <n> file entries
//   "TEXTUREFILES <n>\0" <n> file entries
// file entry = "<name> <flags>\0" then <u32 size> then `size` bytes, where the
// first 4 payload bytes are an ASCII tag ("MASK", "LOFF"/"TXR", ...).
#include <cstdint>
#include <string>
#include <vector>

namespace ht2 {

class Res {
public:
    enum class Kind { Unknown, Mask, Texture, Palette, Sound };

    struct Entry {
        std::string name;      // e.g. "m\\singl_a.msk"
        std::string flags;     // e.g. "noload"
        Kind kind = Kind::Unknown;
        uint32_t size = 0;     // bytes after the u32 size field
        size_t sizeOff = 0;    // file offset of the u32 size field
        std::string tag;       // first 4 payload bytes, trimmed
        size_t tagOff = 0;     // file offset of the 4-byte tag
        size_t dataOff = 0;    // file offset of the payload after the tag
        uint32_t dataSize = 0; // payload size after the tag
    };

    bool load(const std::string& path);
    bool loaded() const { return !d.empty(); }
    const std::vector<Entry>& entries() const { return ents; }
    const Entry* find(const std::string& name) const;
    // Pointer to the entry payload INCLUDING its 4-byte ASCII tag (this is what
    // Mask::decode / Texture::decode expect).
    const uint8_t* data(const Entry& e) const { return d.data() + e.tagOff; }
    const std::vector<uint8_t>& bytes() const { return d; }
    size_t fileSize() const { return d.size(); }
    size_t parsedEnd() const { return end; }
    const std::string& path() const { return srcPath; }

private:
    std::vector<uint8_t> d;
    std::vector<Entry> ents;
    std::string srcPath;
    size_t end = 0;
};

} // namespace ht2
