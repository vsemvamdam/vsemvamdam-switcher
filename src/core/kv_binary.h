// vsemvamdam-switcher — Valve binary KeyValues (appcache\stats\UserGameStatsSchema_<appid>.bin), portable
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vs::kv {

enum class Type : uint8_t { None = 0, String = 1, Int32 = 2, Float32 = 3, Pointer = 4, WideString = 5, Color = 6, UInt64 = 7, End = 8 };

struct Node {
    std::string name;
    Type type = Type::None;
    bool valid = false;
    std::string str;
    int64_t i = 0;
    uint64_t u = 0;
    float f = 0;
    std::vector<Node> children;

    // case-insensitive child lookup; returns an invalid node when missing (chainable: n["a"]["b"])
    const Node& operator[](std::string_view key) const;
    std::string as_string(const std::string& def = {}) const;
    int64_t as_int(int64_t def = 0) const;
    double as_float(double def = 0) const;
    bool as_bool(bool def = false) const;
};

// Parses a whole file; false on malformed / truncated data.
bool parse_binary(const std::string& data, Node& root);

}  // namespace vs::kv
