#include "steam_core.h"

#include <cctype>
#include <cstring>

#include "nlohmann/json.hpp"
#include "str.h"
#include "vdf.h"

namespace vs::steam {

bool is_valid_id64(const std::string& s) {
    uint64_t v;
    if (s.size() < 15 || s.size() > 20 || !parse_u64(s, v)) return false;
    return v > kId64Base && v <= kId64Base + 0xFFFFFFFFULL;
}

uint32_t account_id(uint64_t id64) { return (uint32_t)(id64 & 0xFFFFFFFFULL); }
std::string steam3(uint64_t id64) { return "[U:1:" + std::to_string(account_id(id64)) + "]"; }
std::string steam2(uint64_t id64) {
    uint32_t a = account_id(id64);
    return "STEAM_0:" + std::to_string(a & 1) + ":" + std::to_string(a >> 1);
}
std::string profile_url(const std::string& id64) { return "https://steamcommunity.com/profiles/" + id64; }

static const vdf::Node* users_node(const vdf::Node& root) {
    if (const vdf::Node* u = root.find("users"); u && u->is_object) return u;
    // Some tools write rows at top level
    if (!root.children.empty() && is_valid_id64(trim(root.children.front().key))) return &root;
    return nullptr;
}
static vdf::Node* users_node(vdf::Node& root) { return const_cast<vdf::Node*>(users_node(static_cast<const vdf::Node&>(root))); }

bool parse_loginusers(const std::string& text, std::vector<LoginUser>& out, std::string* err) {
    out.clear();
    vdf::Node root;
    if (!vdf::parse(text, root, err)) return false;
    const vdf::Node* users = users_node(root);
    if (!users) return true;  // empty file
    for (auto& row : users->children) {
        if (!row.is_object) continue;
        LoginUser u;
        u.id64 = trim(row.key);
        u.account = row.get("AccountName");
        u.persona = row.get("PersonaName");
        if (u.account.empty() && u.persona.empty()) continue;
        parse_i64(row.get("Timestamp", "0"), u.timestamp);
        u.most_recent = row.get("MostRecent") == "1";
        u.remember = row.get("RememberPassword") == "1";
        u.offline = row.get("WantsOfflineMode") == "1";
        u.allow_autologin = row.get("AllowAutoLogin") == "1" || row.get("AutoLogin") == "1";
        out.push_back(std::move(u));
    }
    return true;
}

std::string active_id64(const std::vector<LoginUser>& users) {
    std::string found;
    int n = 0;
    for (auto& u : users)
        if (u.most_recent) { found = u.id64; ++n; }
    return n == 1 ? found : std::string();
}

bool rewrite_loginusers(const std::string& text, const SwitchEdit& e, std::string& out, std::string& target_account, std::string* err) {
    vdf::Node root;
    if (!trim(text).empty()) {
        if (!vdf::parse(text, root, err)) return false;
    } else {
        root.is_object = true;
    }
    vdf::Node* users = users_node(root);
    if (!users) users = &root.obj("users");
    target_account.clear();

    bool found = false;
    for (auto& row : users->children) {
        if (!row.is_object) continue;
        bool is_target = !e.target_id64.empty() && trim(row.key) == e.target_id64;
        if (is_target) {
            found = true;
            row.set("MostRecent", "1");
            row.set("RememberPassword", "1");
            row.set("AllowAutoLogin", "1");
            if (row.find("AutoLogin")) row.set("AutoLogin", "1");
            row.set("WantsOfflineMode", e.offline ? "1" : "0");
            row.set("SkipOfflineModeWarning", e.offline ? "1" : "0");
            target_account = row.get("AccountName");
        } else {
            row.set("MostRecent", "0");
            if (row.find("AllowAutoLogin")) row.set("AllowAutoLogin", "0");
            if (row.find("AutoLogin")) row.set("AutoLogin", "0");
        }
    }
    if (!e.target_id64.empty() && !found) {
        if (e.fallback.account.empty()) {
            if (err) *err = "account is unknown to Steam";
            return false;
        }
        vdf::Node row;
        row.key = e.target_id64;
        row.is_object = true;
        row.set("AccountName", e.fallback.account);
        row.set("PersonaName", e.fallback.persona.empty() ? e.fallback.account : e.fallback.persona);
        row.set("RememberPassword", "1");
        row.set("WantsOfflineMode", e.offline ? "1" : "0");
        row.set("SkipOfflineModeWarning", e.offline ? "1" : "0");
        row.set("AllowAutoLogin", "1");
        row.set("MostRecent", "1");
        row.set("Timestamp", std::to_string(e.fallback.timestamp > 0 ? e.fallback.timestamp : unix_now()));
        users->children.push_back(std::move(row));
        target_account = e.fallback.account;
    }
    if (users == &root) {
        // normalise to the standard layout
        vdf::Node fixed;
        fixed.is_object = true;
        vdf::Node u;
        u.key = "users";
        u.is_object = true;
        u.children = std::move(root.children);
        fixed.children.push_back(std::move(u));
        root = std::move(fixed);
    }
    out = vdf::write(root);
    return true;
}

bool remove_loginuser(const std::string& text, const std::string& id64, std::string& out) {
    vdf::Node root;
    if (!vdf::parse(text, root)) return false;
    vdf::Node* users = users_node(root);
    if (!users) { out = text; return true; }
    std::vector<vdf::Node> keep;
    for (auto& r : users->children)
        if (trim(r.key) != id64) keep.push_back(std::move(r));
    users->children = std::move(keep);
    out = vdf::write(root);
    return true;
}

// ---------------------------------------------------------------- persona state
namespace {

size_t ifind(const std::string& hay, const std::string& needle, size_t from) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && std::tolower((unsigned char)hay[i + k]) == std::tolower((unsigned char)needle[k])) ++k;
        if (k == needle.size()) return i;
    }
    return std::string::npos;
}

size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && (unsigned char)s[i] <= ' ') ++i;
    return i;
}

// Given index of an opening quote, returns index of the matching closing quote (escape-aware).
size_t quoted_end(const std::string& s, size_t open) {
    for (size_t i = open + 1; i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '"') return i;
    }
    return std::string::npos;
}

// Within [a,b) find "ePersonaState" followed by optional \" or ", spaces, ':' and a number; replace the number.
bool patch_json_state(std::string& s, size_t a, size_t& b, int state, int* read_out = nullptr) {
    bool changed = false;
    size_t i = a;
    while (true) {
        size_t p = ifind(s, "ePersonaState", i);
        if (p == std::string::npos || p >= b) break;
        size_t j = p + 13;
        while (j < b && (s[j] == '\\' || s[j] == '"')) ++j;
        j = skip_ws(s, j);
        if (j < b && s[j] == ':') {
            j = skip_ws(s, j + 1);
            size_t k = j;
            if (k < b && s[k] == '-') ++k;
            while (k < b && std::isdigit((unsigned char)s[k])) ++k;
            if (k > j) {
                if (read_out) { int64_t v; if (parse_i64(s.substr(j, k - j), v)) *read_out = (int)v; return false; }
                std::string num = std::to_string(state);
                if (s.compare(j, k - j, num) != 0) {
                    s.replace(j, k - j, num);
                    b = b - (k - j) + num.size();
                    changed = true;
                }
                i = j + num.size();
                continue;
            }
        }
        i = p + 13;
    }
    return changed;
}

std::string detect_newline(const std::string& s) { return s.find("\r\n") != std::string::npos ? "\r\n" : "\n"; }

std::string line_indent_after(const std::string& s, size_t pos) {
    // indentation of the line following pos
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) return "\t";
    size_t i = nl + 1;
    std::string ind;
    while (i < s.size() && (s[i] == '\t' || s[i] == ' ')) ind += s[i++];
    if (i < s.size() && s[i] == '}') ind += '\t';
    return ind;
}

}  // namespace

bool patch_persona_state(std::string& s, uint32_t acc, int state) {
    bool changed = false, touched = false;
    // 1) Plain leaves:  "ePersonaState"  "N"
    size_t i = 0;
    while ((i = ifind(s, "\"ePersonaState\"", i)) != std::string::npos) {
        size_t j = skip_ws(s, i + 15);
        if (j < s.size() && s[j] == '"') {
            size_t e = quoted_end(s, j);
            if (e == std::string::npos) break;
            std::string num = std::to_string(state);
            if (s.compare(j + 1, e - j - 1, num) != 0) { s.replace(j + 1, e - j - 1, num); changed = true; }
            touched = true;
            i = j + 1 + num.size();
        } else {
            i += 15;
        }
    }
    // 2) JSON blobs: "FriendStoreLocalPrefs_<acc>"  "{\"ePersonaState\":1,...}"
    const std::string keys[] = {"\"FriendStoreLocalPrefs_" + std::to_string(acc) + "\"",
                                "\"FriendStoreLocalPrefs_[U:1:" + std::to_string(acc) + "]\""};
    for (auto& key : keys) {
        size_t p = 0;
        while ((p = ifind(s, key, p)) != std::string::npos) {
            size_t j = skip_ws(s, p + key.size());
            if (j < s.size() && s[j] == '"') {
                size_t e = quoted_end(s, j);
                if (e == std::string::npos) break;
                size_t end = e;
                if (ifind(s, "ePersonaState", j) < e) {
                    changed |= patch_json_state(s, j + 1, end, state);
                } else {
                    // add the field at the start of the object
                    size_t brace = s.find('{', j);
                    if (brace != std::string::npos && brace < e) {
                        std::string ins = "\\\"ePersonaState\\\":" + std::to_string(state) + ",";
                        size_t after = skip_ws(s, brace + 1);
                        if (after < e && s[after] == '}') ins.pop_back();
                        s.insert(brace + 1, ins);
                        end += ins.size();
                        changed = true;
                    }
                }
                touched = true;
                p = end;
            } else {
                p += key.size();
            }
        }
    }
    if (touched) return changed;
    // 3) Nothing present: insert prefs into WebStorage (or create WebStorage)
    std::string nl = detect_newline(s);
    std::string value = "\"{\\\"ePersonaState\\\":" + std::to_string(state) + ",\\\"strNonFriendsAllowedToMsg\\\":\\\"\\\"}\"";
    size_t ws = ifind(s, "\"WebStorage\"", 0);
    if (ws != std::string::npos) {
        size_t br = skip_ws(s, ws + 12);
        if (br < s.size() && s[br] == '{') {
            std::string ind = line_indent_after(s, br);
            s.insert(br + 1, nl + ind + "\"FriendStoreLocalPrefs_" + std::to_string(acc) + "\"\t\t" + value);
            return true;
        }
    }
    size_t root = ifind(s, "\"UserLocalConfigStore\"", 0);
    if (root != std::string::npos) {
        size_t br = skip_ws(s, root + 22);
        if (br < s.size() && s[br] == '{') {
            std::string block = nl + "\t\"WebStorage\"" + nl + "\t{" + nl + "\t\t\"FriendStoreLocalPrefs_" + std::to_string(acc) + "\"\t\t" + value +
                                nl + "\t}";
            s.insert(br + 1, block);
            return true;
        }
    }
    return false;
}

int read_persona_state(const std::string& s, uint32_t acc) {
    std::string key = "\"FriendStoreLocalPrefs_" + std::to_string(acc) + "\"";
    size_t p = ifind(s, key, 0);
    if (p != std::string::npos) {
        size_t j = skip_ws(s, p + key.size());
        if (j < s.size() && s[j] == '"') {
            size_t e = quoted_end(s, j);
            if (e != std::string::npos) {
                int v = -1;
                size_t end = e;
                std::string copy = s;
                patch_json_state(copy, j + 1, end, 0, &v);
                if (v >= 0) return v;
            }
        }
    }
    size_t i = ifind(s, "\"ePersonaState\"", 0);
    if (i != std::string::npos) {
        size_t j = skip_ws(s, i + 15);
        if (j < s.size() && s[j] == '"') {
            size_t e = quoted_end(s, j);
            int64_t v;
            if (e != std::string::npos && parse_i64(s.substr(j + 1, e - j - 1), v)) return (int)v;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- library
std::vector<std::string> library_paths(const std::string& text) {
    std::vector<std::string> out;
    vdf::Node root;
    if (vdf::parse(text, root)) {
        const vdf::Node* lf = root.find("libraryfolders");
        if (!lf) lf = root.find("LibraryFolders");
        if (lf) {
            for (auto& c : lf->children) {
                if (c.is_object) {
                    std::string p = c.get("path");
                    if (!p.empty()) out.push_back(p);
                } else if (!c.value.empty() && (c.value.find(':') != std::string::npos || c.value.find('/') != std::string::npos)) {
                    out.push_back(c.value);  // legacy format: "1" "D:\\SteamLibrary"
                }
            }
        }
    }
    return out;
}

bool parse_appmanifest(const std::string& text, AppManifest& out) {
    vdf::Node root;
    if (!vdf::parse(text, root)) return false;
    const vdf::Node* st = root.find("AppState");
    if (!st) return false;
    out.appid = st->get("appid");
    out.name = st->get("name");
    out.installdir = st->get("installdir");
    return !out.appid.empty();
}

// ---------------------------------------------------------------- profile xml
static std::string xml_decode(std::string s) {
    s = replace_all(s, "&lt;", "<");
    s = replace_all(s, "&gt;", ">");
    s = replace_all(s, "&quot;", "\"");
    s = replace_all(s, "&apos;", "'");
    s = replace_all(s, "&#39;", "'");
    s = replace_all(s, "&amp;", "&");
    return s;
}

static bool xml_tag(const std::string& xml, const std::string& tag, std::string& out) {
    std::string open = "<" + tag + ">", close = "</" + tag + ">";
    size_t a = xml.find(open);
    if (a == std::string::npos) return false;
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::string::npos) return false;
    std::string v = xml.substr(a, b - a);
    if (starts_with(v, "<![CDATA[") && ends_with(v, "]]>")) v = v.substr(9, v.size() - 12);
    else v = xml_decode(v);
    out = trim(v);
    return true;
}

bool parse_profile_xml(const std::string& xml, ProfileInfo& out) {
    out = ProfileInfo{};
    std::string v, id;
    bool priv_msg = xml_tag(xml, "privacyMessage", v) && !v.empty();
    if (!xml_tag(xml, "steamID64", id) || id.empty()) {
        if (priv_msg) { out.ok = true; out.is_private = true; return true; }
        return false;
    }
    out.ok = true;
    out.is_private = priv_msg;
    if (xml_tag(xml, "steamID", v)) out.persona = v;
    if (xml_tag(xml, "avatarFull", v)) out.avatar_full = v;
    if (xml_tag(xml, "vacBanned", v)) out.vac = v == "1";
    if (xml_tag(xml, "isLimitedAccount", v)) out.limited = v == "1";
    if (xml_tag(xml, "tradeBanState", v)) out.trade_ban = !v.empty() && !iequals(v, "None");
    if (xml_tag(xml, "privacyState", v) && !iequals(v, "public")) out.is_private = true;
    return true;
}

}  // namespace vs::steam
