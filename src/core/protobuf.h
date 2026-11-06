// vsemvamdam-switcher — minimal protobuf wire-format writer/reader (enough for Steam's auth service)
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vs::pb {

class Writer {
public:
    std::string out;
    void varint(uint64_t v) {
        while (v >= 0x80) { out += (char)((v & 0x7F) | 0x80); v >>= 7; }
        out += (char)v;
    }
    void tag(int field, int wire) { varint((uint64_t)field << 3 | (uint64_t)wire); }
    void u64(int field, uint64_t v) { tag(field, 0); varint(v); }
    void i32(int field, int32_t v) { tag(field, 0); varint((uint64_t)(int64_t)v); }  // negative -> 10 bytes, as protobuf does
    void boolean(int field, bool v) { tag(field, 0); varint(v ? 1 : 0); }
    void fixed64(int field, uint64_t v) {
        tag(field, 1);
        for (int i = 0; i < 8; ++i) out += (char)(v >> (8 * i));
    }
    void bytes(int field, const std::string& v) { tag(field, 2); varint(v.size()); out += v; }
    void str(int field, const std::string& v) { bytes(field, v); }
    void message(int field, const Writer& w) { bytes(field, w.out); }
};

struct Field {
    int number = 0;
    int wire = 0;
    uint64_t num = 0;      // varint / fixed64 / fixed32 value
    std::string data;      // length-delimited payload
    float as_float() const { uint32_t u = (uint32_t)num; float f; std::memcpy(&f, &u, 4); return f; }
};

// Parses a message into a flat list of fields. Returns false on malformed input.
inline bool parse(const std::string& in, std::vector<Field>& out) {
    out.clear();
    size_t p = 0;
    auto rd_varint = [&](uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            if (p >= in.size()) return false;
            uint8_t b = (uint8_t)in[p++];
            v |= (uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    };
    while (p < in.size()) {
        uint64_t key;
        if (!rd_varint(key)) return false;
        Field f;
        f.number = (int)(key >> 3);
        f.wire = (int)(key & 7);
        if (f.number <= 0) return false;
        switch (f.wire) {
            case 0:
                if (!rd_varint(f.num)) return false;
                break;
            case 1:
                if (p + 8 > in.size()) return false;
                for (int i = 0; i < 8; ++i) f.num |= (uint64_t)(uint8_t)in[p + i] << (8 * i);
                p += 8;
                break;
            case 2: {
                uint64_t len;
                if (!rd_varint(len) || len > in.size() - p) return false;
                f.data = in.substr(p, (size_t)len);
                p += (size_t)len;
                break;
            }
            case 5:
                if (p + 4 > in.size()) return false;
                for (int i = 0; i < 4; ++i) f.num |= (uint64_t)(uint8_t)in[p + i] << (8 * i);
                p += 4;
                break;
            default:
                return false;
        }
        out.push_back(std::move(f));
    }
    return true;
}

inline const Field* find(const std::vector<Field>& v, int number) {
    for (auto& f : v)
        if (f.number == number) return &f;
    return nullptr;
}
inline std::string get_str(const std::vector<Field>& v, int number) {
    auto* f = find(v, number);
    return f ? f->data : std::string();
}
inline uint64_t get_num(const std::vector<Field>& v, int number) {
    auto* f = find(v, number);
    return f ? f->num : 0;
}

}  // namespace vs::pb
