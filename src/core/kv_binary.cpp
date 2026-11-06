#include "kv_binary.h"

#include <cstring>

#include "str.h"

namespace vs::kv {

static const Node& invalid_node() {
    static const Node n;
    return n;
}

const Node& Node::operator[](std::string_view key) const {
    for (auto& c : children)
        if (iequals(c.name, key)) return c;
    return invalid_node();
}

std::string Node::as_string(const std::string& def) const {
    if (!valid) return def;
    switch (type) {
        case Type::String: return str;
        case Type::Int32: return std::to_string(i);
        case Type::UInt64: return std::to_string(u);
        case Type::Float32: return std::to_string(f);
        default: return def;
    }
}

int64_t Node::as_int(int64_t def) const {
    if (!valid) return def;
    switch (type) {
        case Type::Int32:
        case Type::Pointer:
        case Type::Color: return i;
        case Type::UInt64: return (int64_t)u;
        case Type::Float32: return (int64_t)f;
        case Type::String: {
            int64_t v;
            return parse_i64(trim(str), v) ? v : def;
        }
        default: return def;
    }
}

double Node::as_float(double def) const {
    if (!valid) return def;
    switch (type) {
        case Type::Float32: return f;
        case Type::Int32:
        case Type::Pointer:
        case Type::Color: return (double)i;
        case Type::UInt64: return (double)u;
        case Type::String: {
            try {
                size_t pos = 0;
                double v = std::stod(str, &pos);
                return pos ? v : def;
            } catch (...) {
                return def;
            }
        }
        default: return def;
    }
}

bool Node::as_bool(bool def) const {
    if (!valid) return def;
    if (type == Type::String) {
        std::string s = to_lower(trim(str));
        if (s == "true") return true;
        if (s == "false") return false;
    }
    return as_int(def ? 1 : 0) != 0;
}

namespace {
struct Reader {
    const std::string& d;
    size_t p = 0;
    bool u8(uint8_t& v) {
        if (p + 1 > d.size()) return false;
        v = (uint8_t)d[p++];
        return true;
    }
    bool u32(uint32_t& v) {
        if (p + 4 > d.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) v |= (uint32_t)(uint8_t)d[p + k] << (8 * k);
        p += 4;
        return true;
    }
    bool u64(uint64_t& v) {
        if (p + 8 > d.size()) return false;
        v = 0;
        for (int k = 0; k < 8; ++k) v |= (uint64_t)(uint8_t)d[p + k] << (8 * k);
        p += 8;
        return true;
    }
    bool cstr(std::string& out) {
        size_t z = d.find('\0', p);
        if (z == std::string::npos) return false;
        out = d.substr(p, z - p);
        p = z + 1;
        return true;
    }
};

bool read_children(Reader& r, Node& parent, int depth) {
    if (depth > 64) return false;
    for (;;) {
        uint8_t t;
        if (!r.u8(t)) return false;
        if ((Type)t == Type::End) return true;
        Node n;
        n.type = (Type)t;
        if (!r.cstr(n.name)) return false;
        switch (n.type) {
            case Type::None:
                if (!read_children(r, n, depth + 1)) return false;
                n.valid = true;
                break;
            case Type::String:
                if (!r.cstr(n.str)) return false;
                n.valid = true;
                break;
            case Type::Int32:
            case Type::Pointer:
            case Type::Color: {
                uint32_t v;
                if (!r.u32(v)) return false;
                n.i = n.type == Type::Int32 ? (int64_t)(int32_t)v : (int64_t)v;
                n.valid = true;
                break;
            }
            case Type::Float32: {
                uint32_t v;
                if (!r.u32(v)) return false;
                std::memcpy(&n.f, &v, 4);
                n.valid = true;
                break;
            }
            case Type::UInt64:
                if (!r.u64(n.u)) return false;
                n.valid = true;
                break;
            default:
                return false;  // wide strings and unknown types are not used by stats schemas
        }
        parent.children.push_back(std::move(n));
    }
}
}  // namespace

bool parse_binary(const std::string& data, Node& root) {
    root = Node{};
    Reader r{data};
    if (!read_children(r, root, 0)) return false;
    root.valid = true;
    return true;
}

}  // namespace vs::kv
