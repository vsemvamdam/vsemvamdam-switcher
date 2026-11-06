#include "steam_stats.h"

#include "nlohmann/json.hpp"
#include "str.h"

using nlohmann::json;

namespace vs::stats {

static std::string localized(const kv::Node& n, const std::string& lang, const std::string& def) {
    std::string s = n[lang].as_string();
    if (!s.empty()) return s;
    if (lang != "english") {
        s = n["english"].as_string();
        if (!s.empty()) return s;
    }
    s = n.as_string();
    return s.empty() ? def : s;
}

bool parse_schema(const kv::Node& root, uint32_t appid, const std::string& lang, Schema& out) {
    out = Schema{};
    const kv::Node& stats = root[std::to_string(appid)]["stats"];
    if (!stats.valid || stats.children.empty()) return false;
    for (const auto& st : stats.children) {
        if (!st.valid) continue;
        int type = StatInvalid;
        const kv::Node& tn = st["type"];
        if (tn.valid && tn.type == kv::Type::String) {
            std::string t = to_lower(tn.str);
            if (t == "int" || t == "integer") type = StatInt;
            else if (t == "float") type = StatFloat;
            else if (t == "avgrate") type = StatAvgRate;
            else if (t == "achievements") type = StatAchievements;
            else if (t == "groupachievements") type = StatGroupAchievements;
        }
        if (type == StatInvalid) {
            const kv::Node& ti = st["type_int"];
            type = (int)(ti.valid ? ti.as_int(0) : tn.as_int(0));
        }
        switch (type) {
            case StatInt:
            case StatFloat:
            case StatAvgRate: {
                StatDef d;
                d.id = st["name"].as_string();
                if (d.id.empty()) break;
                d.name = localized(st["display"]["name"], lang, d.id);
                d.type = type;
                if (type == StatInt) {
                    d.min = (double)st["min"].as_int(INT32_MIN);
                    d.max = (double)st["max"].as_int(INT32_MAX);
                    d.maxchange = (double)st["maxchange"].as_int(0);
                    d.def = (double)st["default"].as_int(0);
                } else {
                    d.min = st["min"].as_float(-3.4e38);
                    d.max = st["max"].as_float(3.4e38);
                    d.maxchange = st["maxchange"].as_float(0);
                    d.def = st["default"].as_float(0);
                }
                d.incrementonly = st["incrementonly"].as_bool(false);
                d.set_by_gs = st["bSetByTrustedGS"].as_bool(false);
                d.permission = (int)st["permission"].as_int(0);
                out.stats.push_back(d);
                break;
            }
            case StatAchievements:
            case StatGroupAchievements:
                for (const auto& bits : st.children) {
                    if (!iequals(bits.name, "bits")) continue;
                    for (const auto& bit : bits.children) {
                        AchievementDef a;
                        a.id = bit["name"].as_string();
                        if (a.id.empty()) continue;
                        a.name = localized(bit["display"]["name"], lang, a.id);
                        a.desc = localized(bit["display"]["desc"], lang, "");
                        a.icon = bit["display"]["icon"].as_string();
                        a.icon_gray = bit["display"]["icon_gray"].as_string();
                        a.hidden = bit["display"]["hidden"].as_bool(false);
                        a.permission = (int)bit["permission"].as_int(0);
                        out.achievements.push_back(a);
                    }
                }
                break;
            default:
                break;
        }
    }
    return true;
}

// ---- JSON
std::string to_json(const GameStats& g) {
    json j = {{"appid", g.appid}, {"name", g.name}, {"steamid", std::to_string(g.steamid)}, {"error", g.error}};
    json a = json::array();
    for (auto& x : g.achievements)
        a.push_back({{"id", x.def.id}, {"name", x.def.name}, {"desc", x.def.desc}, {"icon", x.def.icon}, {"icon_gray", x.def.icon_gray},
                     {"hidden", x.def.hidden}, {"permission", x.def.permission}, {"achieved", x.achieved}, {"unlock_time", x.unlock_time}});
    j["achievements"] = a;
    json s = json::array();
    for (auto& x : g.stats)
        s.push_back({{"id", x.def.id}, {"name", x.def.name}, {"type", x.def.type}, {"min", x.def.min}, {"max", x.def.max},
                     {"maxchange", x.def.maxchange}, {"default", x.def.def}, {"incrementonly", x.def.incrementonly},
                     {"set_by_gs", x.def.set_by_gs}, {"permission", x.def.permission}, {"value", x.value}});
    j["stats"] = s;
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

template <class T>
static void rd(const json& j, const char* k, T& v) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return;
    try { v = it->get<T>(); } catch (...) {}
}

bool from_json(const std::string& text, GameStats& g) {
    g = GameStats{};
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    rd(j, "appid", g.appid);
    rd(j, "name", g.name);
    rd(j, "error", g.error);
    std::string sid;
    rd(j, "steamid", sid);
    parse_u64(sid, g.steamid);
    if (auto it = j.find("achievements"); it != j.end() && it->is_array())
        for (auto& x : *it) {
            if (!x.is_object()) continue;
            Achievement a;
            rd(x, "id", a.def.id);
            rd(x, "name", a.def.name);
            rd(x, "desc", a.def.desc);
            rd(x, "icon", a.def.icon);
            rd(x, "icon_gray", a.def.icon_gray);
            rd(x, "hidden", a.def.hidden);
            rd(x, "permission", a.def.permission);
            rd(x, "achieved", a.achieved);
            rd(x, "unlock_time", a.unlock_time);
            if (!a.def.id.empty()) g.achievements.push_back(a);
        }
    if (auto it = j.find("stats"); it != j.end() && it->is_array())
        for (auto& x : *it) {
            if (!x.is_object()) continue;
            Stat s;
            rd(x, "id", s.def.id);
            rd(x, "name", s.def.name);
            rd(x, "type", s.def.type);
            rd(x, "min", s.def.min);
            rd(x, "max", s.def.max);
            rd(x, "maxchange", s.def.maxchange);
            rd(x, "default", s.def.def);
            rd(x, "incrementonly", s.def.incrementonly);
            rd(x, "set_by_gs", s.def.set_by_gs);
            rd(x, "permission", s.def.permission);
            rd(x, "value", s.value);
            if (!s.def.id.empty()) g.stats.push_back(s);
        }
    return true;
}

std::string to_json(const Changes& c) {
    json j = {{"reset_all", c.reset_all}, {"reset_with_achievements", c.reset_with_achievements}};
    json a = json::object();
    for (auto& [k, v] : c.achievements) a[k] = v;
    json s = json::object();
    for (auto& [k, v] : c.stats) s[k] = v;
    j["achievements"] = a;
    j["stats"] = s;
    return j.dump();
}

bool from_json(const std::string& text, Changes& c) {
    c = Changes{};
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    rd(j, "reset_all", c.reset_all);
    rd(j, "reset_with_achievements", c.reset_with_achievements);
    if (auto it = j.find("achievements"); it != j.end() && it->is_object())
        for (auto& [k, v] : it->items())
            if (v.is_boolean()) c.achievements[k] = v.get<bool>();
    if (auto it = j.find("stats"); it != j.end() && it->is_object())
        for (auto& [k, v] : it->items())
            if (v.is_number()) c.stats[k] = v.get<double>();
    return true;
}

std::string steam_language(bool english_ui) { return english_ui ? "english" : "russian"; }

}  // namespace vs::stats
