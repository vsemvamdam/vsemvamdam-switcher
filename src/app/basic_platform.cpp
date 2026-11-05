// vsemvamdam-switcher — generic platform engine driven by a TcNo-compatible Platforms.json
#include "basic_platform.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

#include "../core/basic_core.h"
#include "../core/fsx.h"
#include "../core/i18n.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../core/str.h"
#include "../os/win.h"

using nlohmann::json;

namespace vs {

using clk = std::chrono::steady_clock;

static std::vector<std::string> str_list(const json& j) {
    std::vector<std::string> v;
    if (j.is_string()) v.push_back(j.get<std::string>());
    else if (j.is_array())
        for (auto& x : j)
            if (x.is_string()) v.push_back(x.get<std::string>());
    return v;
}


// Type-tolerant descriptor accessors (a hand-edited platforms.json must never crash the app)
static std::string jstr(const json& j, const char* k, const std::string& def = std::string()) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}
static bool jbool(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}
static json jobj(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_object() ? *it : json::object();
}
static json jany(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() ? *it : json();
}
static std::string extras_str(const json& d, const char* k) { return jstr(jobj(d, "Extras"), k); }

static std::string b64(const std::vector<uint8_t>& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        uint32_t n = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        o += t[n >> 18]; o += t[(n >> 12) & 63]; o += t[(n >> 6) & 63]; o += t[n & 63];
    }
    if (i + 1 == d.size()) { uint32_t n = d[i] << 16; o += t[n >> 18]; o += t[(n >> 12) & 63]; o += "=="; }
    else if (i + 2 == d.size()) { uint32_t n = (d[i] << 16) | (d[i + 1] << 8); o += t[n >> 18]; o += t[(n >> 12) & 63]; o += t[(n >> 6) & 63]; o += '='; }
    return o;
}

static std::vector<uint8_t> unb64(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> o;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((buf >> bits) & 0xFF); }
    }
    return o;
}

static bool has_wildcard(const std::string& p) { return fsx::filename(p).find_first_of("*?") != std::string::npos; }

// Refuses to delete anything that is not clearly a client's own data path.
static bool safe_to_delete(const std::string& path) {
    std::string p = replace_all(trim(path), "/", "\\");
    while (!p.empty() && p.back() == '\\') p.pop_back();
    if (p.size() < 4 || p[1] != ':' || p[2] != '\\') return false;           // must be absolute "X:\..."
    if (p.find('%') != std::string::npos || p.find("..") != std::string::npos) return false;  // unexpanded / traversal
    int depth = 0;
    for (char c : p) if (c == '\\') ++depth;
    if (depth < 3) return false;                                              // e.g. C:\Users\Name
    static const std::vector<std::string> roots = {os::appdata_roaming(), os::appdata_local(), os::programdata(), os::user_profile(),
                                                   os::program_files(), os::program_files_x86(), os::documents_dir(), os::desktop_dir(),
                                                   fsx::join(os::appdata_local(), "Programs"), os::expand_env("%WINDIR%")};
    for (auto& r : roots) {
        std::string rr = r;
        while (!rr.empty() && rr.back() == '\\') rr.pop_back();
        if (!rr.empty() && iequals(p, rr)) return false;
    }
    std::string w = os::expand_env("%WINDIR%");
    if (!w.empty() && istarts_with(p, w + "\\")) return false;
    return true;
}

static void safe_remove(const std::string& path, const std::string& who) {
    if (!safe_to_delete(path)) { log::warn(who + ": refusing to delete " + path); return; }
    fsx::remove_all(path);
}

static bool is_value_glob(const std::string& v) { return v.find_first_of("*?") != std::string::npos; }

static std::vector<os::RegValue> reg_matching(const std::string& key, const std::string& glob) {
    std::vector<os::RegValue> out;
    for (auto& v : os::reg_values(key))
        if (glob == "*" || fsx::wildcard_match(glob, v.name)) out.push_back(v);
    return out;
}

BasicPlatform::BasicPlatform(Store& st, std::string id, json def) : store_(st), id_(std::move(id)), def_(std::move(def)) {
    name_ = jstr(def_, "vsName", jstr(def_, "_name", id_));
    help_ = jstr(def_, g_lang_en ? "vsHelpEn" : "vsHelp");
}

std::string BasicPlatform::saves_dir() const { return fsx::join(store_.dir, "platforms\\" + id_); }
std::string BasicPlatform::account_dir(const std::string& id) const { return fsx::join(saves_dir(), sanitize_filename(id)); }

std::string BasicPlatform::platform_folder() {
    std::string exe = exe_path();
    return exe.empty() ? std::string() : fsx::parent(exe);
}

std::string BasicPlatform::expand(const std::string& s) {
    static const std::map<std::string, std::string> base = [] {
        std::map<std::string, std::string> b;
        b["appdata"] = os::appdata_roaming();
        b["localappdata"] = os::appdata_local();
        b["programdata"] = os::programdata();
        b["programfiles"] = os::program_files();
        b["programfiles(x86)"] = os::program_files_x86();
        b["userprofile"] = os::user_profile();
        b["documents"] = os::documents_dir();
        b["desktop"] = os::desktop_dir();
        b["music"] = fsx::join(os::user_profile(), "Music");
        b["pictures"] = fsx::join(os::user_profile(), "Pictures");
        b["videos"] = fsx::join(os::user_profile(), "Videos");
        b["startmenuappdata"] = os::start_menu_programs();
        b["startmenuprogramdata"] = fsx::join(os::programdata(), "Microsoft\\Windows\\Start Menu\\Programs");
        b["temp"] = os::expand_env("%TEMP%");
        b["windir"] = os::expand_env("%WINDIR%");
        b["systemdrive"] = os::expand_env("%SYSTEMDRIVE%");
        b["public"] = os::expand_env("%PUBLIC%");
        return b;
    }();
    std::map<std::string, std::string> vars = base;
    if (s.find('%') != std::string::npos && icontains(s, "%Platform_Folder%")) vars["platform_folder"] = platform_folder();
    std::string r = basic::expand_vars(s, vars);
    if (r.find('%') != std::string::npos) r = os::expand_env(r);
    return r;
}

std::string BasicPlatform::exe_path() {
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        auto it = store_.settings.platforms.find(id_);
        if (it != store_.settings.platforms.end() && !it->second.exe_path.empty()) {
            std::string p = it->second.exe_path;
            if (fsx::is_dir(p)) {
                auto d = str_list(jany(def_, "ExeLocationDefault"));
                if (!d.empty()) p = fsx::join(p, fsx::filename(expand(d[0])));
            }
            return p;
        }
    }
    auto defs = str_list(jany(def_, "ExeLocationDefault"));
    // expand without %Platform_Folder% (would recurse)
    std::vector<std::string> cands;
    for (auto& d : defs)
        if (!icontains(d, "%Platform_Folder%")) cands.push_back(expand(d));
    for (auto& c : cands)
        if (fsx::exists(c)) return c;
    static std::mutex sc_mtx;
    static std::map<std::string, std::string> shortcut_cache;
    std::string sc = jstr(def_, "GetPathFromShortcutNamed");
    if (!sc.empty()) {
        std::string target;
        {
            std::lock_guard<std::mutex> lk(sc_mtx);
            auto it = shortcut_cache.find(id_);
            if (it == shortcut_cache.end()) it = shortcut_cache.emplace(id_, os::find_shortcut_target(sc)).first;
            target = it->second;
        }
        if (!target.empty() && fsx::exists(target)) {
            // a game shortcut may point to the game; keep the client exe name from defaults when possible
            if (!cands.empty()) {
                std::string alt = fsx::join(fsx::parent(target), fsx::filename(cands[0]));
                if (fsx::exists(alt)) return alt;
            }
            return target;
        }
    }
    return cands.empty() ? std::string() : cands[0];
}

bool BasicPlatform::installed() {
    std::string e = exe_path();
    return !e.empty() && fsx::exists(e);
}

std::vector<std::string> BasicPlatform::process_names() const {
    std::vector<std::string> out;
    for (auto& n : str_list(jany(def_, "ExesToEnd")))
        if (!istarts_with(n, "SERVICE:")) out.push_back(n);
    return out;
}

std::string BasicPlatform::data_folder() { return platform_folder(); }

// ---------------------------------------------------------------- unique id
// Registry value as text: strings as is, Unity-style REG_BINARY strings as UTF-8 without the trailing NUL.
static bool reg_text(const std::string& key, const std::string& name, std::string& out) {
    os::RegValue v;
    if (!os::reg_get_raw(key, name, v)) return false;
    if (v.type == 3 /*REG_BINARY*/) {
        std::string s(v.data.begin(), v.data.end());
        while (!s.empty() && s.back() == '\0') s.pop_back();
        bool printable = !s.empty();
        for (unsigned char c : s) if (c < 0x20 && c != '\t') { printable = false; break; }
        if (!printable) return false;
        out = s;
        return true;
    }
    return os::reg_get_string(key, name, out);
}

std::string BasicPlatform::unique_id_now() {
    if (epic_new()) return epic_uid();
    std::string file = jstr(def_, "UniqueIdFile");
    std::string method = jstr(def_, "UniqueIdMethod");
    std::string rx = jstr(def_, "UniqueIdRegex");
    if (file.empty()) return {};
    if (iequals(method, "LEVELDB") || iequals(method, "STEAM")) return {};  // no reliable live id: handled by autosave
    if (iequals(method, "REGKEY")) {
        basic::Spec s = basic::parse_spec(istarts_with(file, "REG:") ? file : "REG:" + file);
        std::string v;
        if (is_value_glob(s.value_name)) {
            auto m = reg_matching(s.path, s.value_name);
            std::sort(m.begin(), m.end(), [](auto& a, auto& b) { return a.name < b.name; });
            if (!m.empty()) reg_text(s.path, m[0].name, v);
        } else {
            reg_text(s.path, s.value_name, v);
        }
        return trim(v);
    }
    if (iequals(method, "BUILTIN")) {
        // Rockstar: newest cache file containing <Email>…</Email>
        auto files = fsx::glob(expand(file));
        std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return fsx::mtime_unix(a) > fsx::mtime_unix(b); });
        for (auto& f : files) {
            if (fsx::file_size(f) > 16u * 1024 * 1024) continue;
            std::string t;
            if (!fsx::read_file(f, t)) continue;
            t.erase(std::remove(t.begin(), t.end(), '\0'), t.end());
            size_t a = t.find("<Email>");
            if (a == std::string::npos) continue;
            size_t b = t.find("</Email>", a);
            if (b == std::string::npos) continue;
            std::string e = trim(t.substr(a + 7, b - a - 7));
            if (!e.empty() && e.size() < 200) return e;
        }
        return {};
    }
    if (istarts_with(method, "JSON_SELECT") || starts_with(file, "JSON_")) {
        std::string raw = starts_with(file, "JSON_") ? file : method + "::" + file;
        basic::Spec s = basic::parse_spec(raw);
        std::string text;
        if (!fsx::read_file(expand(s.path), text)) return {};
        json j = json::parse(text, nullptr, false, true);
        if (j.is_discarded()) return {};
        const json* v = basic::json_get(j, s.json_path);
        if (!v || v->is_null()) return {};
        std::string val;
        if (v->is_string()) val = v->get<std::string>();
        else if (v->is_array() && !v->empty()) val = (*v)[0].is_string() ? (*v)[0].get<std::string>() : (*v)[0].dump();
        else if (!v->is_array()) val = v->dump();
        if (s.json_mode == "JSON_SELECT_FIRST") val = basic::first_of(val, s.delimiter);
        return trim(val);
    }
    std::string path = expand(file);
    if (!fsx::exists(path)) {
        // relative name: look it up among LoginFiles destinations
        json lf = jobj(def_, "LoginFiles");
        for (auto& [src, dst] : lf.items())
            if (dst.is_string() && iequals(dst.get<std::string>(), file)) { path = expand(src); break; }
    }
    std::string text;
    if (!fsx::is_file(path) || fsx::file_size(path) > 64u * 1024 * 1024 || !fsx::read_file(path, text)) return {};
    if (iequals(method, "CREATE_ID_FILE") || rx.empty()) return trim(text.size() > 512 ? text.substr(0, 512) : text);
    return basic::regex_id(text, rx);
}

std::string BasicPlatform::current_account_id() {
    std::string uid = unique_id_now();
    if (uid.empty()) return {};
    std::string sid = sanitize_filename(uid);
    return fsx::is_dir(account_dir(sid)) ? sid : std::string();
}

// ---------------------------------------------------------------- scan
static bool is_account_dir_name(const std::string& d) {
    return !d.empty() && d[0] != '_' && d[0] != '.' && !ends_with(d, ".saving") && !ends_with(d, ".old");
}

std::vector<Account> BasicPlatform::scan() {
    std::vector<Account> out;
    std::string cur = current_account_id();
    for (auto& d : fsx::list_dir(saves_dir(), true)) {
        if (!is_account_dir_name(d)) continue;
        Account a;
        a.id = d;
        std::string dir = account_dir(d);
        for (const char* av : {"avatar.png", "avatar.jpg"})
            if (fsx::exists(fsx::join(dir, av))) { a.avatar_path = fsx::join(dir, av); break; }
        {
            std::lock_guard<std::recursive_mutex> lk(store_.mtx);
            auto& m = store_.meta[id_][d];
            a.display = m.display.empty() ? d : m.display;
            a.login = m.login;
            a.last_login = m.last_switched ? m.last_switched : m.saved_at;
        }
        a.current = a.id == cur;
        out.push_back(std::move(a));
    }
    return out;
}

// ---------------------------------------------------------------- close / launch
bool BasicPlatform::close_now(const Progress& p, OpResult& r) {
    auto names = process_names();
    if (names.empty() || !os::any_running(names)) return true;
    if (p) p(std::string(TR("Закрываю ", "Closing ")) + name_ + "…");
    std::string method = extras_str(def_, "ClosingMethod");
    if (!iequals(method, "TaskKill")) {
        if (os::close_windows_of(names) > 0) os::wait_all_exit(names, 2500);
    }
    auto k = os::kill_processes(names, 8000);
    if (k.access_denied > 0 && os::any_running(names)) {
        r.needs_admin = true;
        r.message = name_ + TR(" запущен от имени администратора — перезапустите свитчер с правами администратора.",
                               " runs as administrator — restart the switcher as administrator.");
        return false;
    }
    if (!os::wait_all_exit(names, 3000)) {
        r.message = std::string(TR("Не удалось закрыть ", "Could not close ")) + name_;
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let file handles go
    return true;
}

OpResult BasicPlatform::close_client() {
    OpResult r;
    r.ok = close_now(nullptr, r);
    if (r.ok) r.message = name_ + TR(" закрыт", " closed");
    return r;
}

void BasicPlatform::launch() {
    std::string exe = exe_path();
    if (!fsx::exists(exe)) return;
    std::string args = jstr(def_, "ExeExtraArgs");
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        auto it = store_.settings.platforms.find(id_);
        if (it != store_.settings.platforms.end()) {
            if (!it->second.launch_after) return;
            if (!it->second.args.empty()) args += (args.empty() ? "" : " ") + it->second.args;
        }
    }
    os::launch(exe, args, fsx::parent(exe), true);
}

// ---------------------------------------------------------------- file ops
bool BasicPlatform::clear_login(std::string& err) {
    (void)err;
    if (epic_new()) { epic_clear(); return true; }
    json lf = jobj(def_, "LoginFiles");
    auto clear_spec = [&](const basic::Spec& s) {
        if (s.kind == basic::Spec::Reg) {
            // TcNo semantics: clearing always removes the value(s); ":*" empties the key but keeps it
            if (s.value_name.empty()) os::reg_delete_tree(s.path);
            else if (s.value_name == "*") {
                for (auto& sub : os::reg_subkeys(s.path)) os::reg_delete_tree(s.path + "\\" + sub);
                for (auto& v : os::reg_values(s.path)) os::reg_delete_value(s.path, v.name);
            } else if (is_value_glob(s.value_name)) {
                for (auto& v : reg_matching(s.path, s.value_name)) os::reg_delete_value(s.path, v.name);
            } else {
                os::reg_delete_value(s.path, s.value_name);
            }
        } else if (s.kind == basic::Spec::Json) {
            std::string path = expand(s.path), text;
            if (!fsx::read_file(path, text)) return;
            json j = json::parse(text, nullptr, false, true);
            if (j.is_discarded()) return;
            if (basic::json_get(j, s.json_path)) {
                basic::json_set(j, s.json_path, json(""));
                fsx::write_file(path, j.dump(4, ' ', false, json::error_handler_t::replace));
            }
        } else {
            std::string path = expand(s.path);
            if (has_wildcard(path)) {
                if (!safe_to_delete(fsx::parent(path))) { log::warn(id_ + ": refusing to clear " + path); return; }
                for (auto& m : fsx::glob(path)) safe_remove(m, id_);
            } else {
                safe_remove(path, id_);
            }
        }
    };
    for (auto& entry : str_list(jany(def_, "PathListToClear"))) {
        if (iequals(entry, "SAME_AS_LOGIN_FILES")) {
            for (auto& [src, dst] : lf.items()) clear_spec(basic::parse_spec(src));
        } else {
            clear_spec(basic::parse_spec(entry));
        }
    }
    return true;
}

static json reg_values_json(const std::string& key, const std::string& glob) {
    json vals = json::array();
    for (auto& v : reg_matching(key, glob)) vals.push_back({{"name", v.name}, {"type", v.type}, {"data", b64(v.data)}});
    return vals;
}

static bool reg_restore_values(const std::string& key, const json& vals) {
    bool ok = true;
    if (!vals.is_array()) return false;
    for (auto& v : vals) {
        if (!v.is_object()) continue;
        os::RegValue rv;
        rv.name = jstr(v, "name");
        if (rv.name.find_first_of("*?") != std::string::npos) continue;  // never create wildcard-named values
        rv.type = v.contains("type") && v["type"].is_number_unsigned() ? v["type"].get<uint32_t>() : 1u;
        rv.data = unb64(jstr(v, "data"));
        ok &= os::reg_set_raw(key, rv);
    }
    return ok;
}

bool BasicPlatform::backup_to(const std::string& dir, std::string& err) {
    if (epic_new()) return epic_save(dir, err);
    json lf = jobj(def_, "LoginFiles");
    bool all_required = jbool(def_, "AllFilesRequired");
    json reg = json::object(), jsv = json::object();
    int copied = 0, missing = 0;
    std::string uid_spec = iequals(jstr(def_, "UniqueIdMethod"), "REGKEY") ? jstr(def_, "UniqueIdFile") : std::string();
    for (auto& [src_raw, dst_j] : lf.items()) {
        if (!dst_j.is_string()) continue;
        std::string dst = dst_j.get<std::string>();
        basic::Spec s = basic::parse_spec(src_raw);
        if (s.kind == basic::Spec::Reg) {
            // keyed by the full source spec (several values may share one destination label)
            if (is_value_glob(s.value_name)) {
                json vals = reg_values_json(s.path, s.value_name);
                if (vals.empty()) { ++missing; continue; }
                reg[src_raw] = {{"key", s.path}, {"values", vals}};
            } else if (s.value_name.empty()) {
                std::vector<os::RegTree> tree;
                if (!os::reg_export_tree(s.path, tree)) { ++missing; continue; }
                json t = json::array();
                for (auto& node : tree) {
                    json vals = json::array();
                    for (auto& v : node.values) vals.push_back({{"name", v.name}, {"type", v.type}, {"data", b64(v.data)}});
                    t.push_back({{"path", node.path}, {"values", vals}});
                }
                reg[src_raw] = {{"key", s.path}, {"tree", t}};
            } else {
                os::RegValue v;
                if (!os::reg_get_raw(s.path, s.value_name, v)) { ++missing; continue; }
                reg[src_raw] = {{"key", s.path}, {"values", json::array({{{"name", v.name}, {"type", v.type}, {"data", b64(v.data)}}})}};
            }
            ++copied;
        } else if (s.kind == basic::Spec::Json) {
            std::string text;
            if (!fsx::read_file(expand(s.path), text)) { ++missing; continue; }
            json j = json::parse(text, nullptr, false, true);
            const json* v = j.is_discarded() ? nullptr : basic::json_get(j, s.json_path);
            if (!v || v->is_null()) { ++missing; continue; }
            json val = *v;
            if (s.json_mode == "JSON_SELECT_FIRST") {
                if (val.is_string()) val = basic::first_of(val.get<std::string>(), s.delimiter);
                else if (val.is_array() && !val.empty()) val = val[0];
            }
            if (val.is_string() && val.get<std::string>().empty()) { ++missing; continue; }
            jsv[src_raw] = val;
            ++copied;
        } else {
            std::string path = expand(s.path);
            std::string target = fsx::join(dir, dst);
            std::string e;
            if (has_wildcard(path)) {
                auto matches = fsx::glob(path);
                if (matches.empty()) { ++missing; continue; }
                fsx::mkdirs(target);
                for (auto& m : matches) {
                    if (!fsx::copy_any(m, fsx::join(target, fsx::filename(m)), &e)) { err = e; return false; }
                    ++copied;
                }
            } else {
                if (!fsx::exists(path)) {
                    ++missing;
                    if (all_required) { err = path; return false; }
                    continue;
                }
                bool dir_dst = ends_with(dst, "\\") || ends_with(dst, "/");
                std::string t2 = fsx::is_dir(path) || !dir_dst ? target : fsx::join(target, fsx::filename(path));
                if (!fsx::copy_any(path, t2, &e)) { err = e; return false; }
                ++copied;
            }
        }
    }
    // REGKEY unique id value that is not itself among LoginFiles
    if (!uid_spec.empty()) {
        std::string key = istarts_with(uid_spec, "REG:") ? uid_spec : "REG:" + uid_spec;
        bool listed = false;
        for (auto& [src_raw, dst_j] : lf.items())
            if (iequals(trim(src_raw), key)) listed = true;
        if (!listed) {
            basic::Spec s = basic::parse_spec(key);
            json vals = reg_values_json(s.path, is_value_glob(s.value_name) ? s.value_name : s.value_name);
            if (!vals.empty() && !is_value_glob(s.value_name)) {
                json one = json::array();
                for (auto& v : vals) if (iequals(jstr(v, "name"), s.value_name)) one.push_back(v);
                vals = one;
            }
            if (!vals.empty()) reg[key] = {{"key", s.path}, {"values", vals}};
        }
    }
    if (all_required && missing > 0) { err = TR("не все файлы входа найдены", "not all login files were found"); return false; }
    if (copied == 0) { err = TR("клиент не хранит вход — войдите в аккаунт", "nothing to save — sign in first"); return false; }
    if (!reg.empty() && !fsx::write_file(fsx::join(dir, "_vs_registry.json"), reg.dump(1, ' ', false, json::error_handler_t::replace))) { err = "registry"; return false; }
    if (!jsv.empty() && !fsx::write_file(fsx::join(dir, "_vs_json.json"), jsv.dump(1, ' ', false, json::error_handler_t::replace))) { err = "json"; return false; }
    return true;
}

bool BasicPlatform::can_restore(const std::string& dir, std::string& err) {
    if (!fsx::is_dir(dir)) { err = TR("сохранённые данные не найдены", "saved data not found"); return false; }
    if (epic_new()) {
        std::string prot;
        std::vector<uint8_t> out;
        if (!fsx::read_file(fsx::join(dir, "epic-session.dpapi"), prot) || !os::dpapi_unprotect(std::vector<uint8_t>(prot.begin(), prot.end()), out)) {
            err = TR("у этого аккаунта нет сессии нового Epic Launcher — войдите в него и сохраните заново",
                     "this account has no new Epic launcher session — sign in and save it again");
            return false;
        }
        std::fill(out.begin(), out.end(), 0);
        return true;
    }
    std::string t;
    for (const char* f : {"_vs_registry.json", "_vs_json.json"}) {
        if (!fsx::read_file(fsx::join(dir, f), t)) continue;
        json j = json::parse(t, nullptr, false);
        if (!j.is_object()) { err = std::string(f) + TR(" повреждён", " is damaged"); return false; }
    }
    return true;
}

bool BasicPlatform::restore_from(const std::string& dir, std::string& err) {
    if (epic_new()) return epic_restore(dir, err);
    json lf = jobj(def_, "LoginFiles");
    json reg = json::object(), jsv = json::object();
    std::string text;
    if (fsx::read_file(fsx::join(dir, "_vs_registry.json"), text)) { reg = json::parse(text, nullptr, false); if (!reg.is_object()) reg = json::object(); }
    if (fsx::read_file(fsx::join(dir, "_vs_json.json"), text)) { jsv = json::parse(text, nullptr, false); if (!jsv.is_object()) jsv = json::object(); }
    bool ok = true;
    std::string first_err;
    auto note = [&](const std::string& e) { if (!e.empty() && first_err.empty()) first_err = e; };
    // registry first (unique id values included)
    for (auto& [key, r] : reg.items()) {
        if (!r.is_object()) continue;
        basic::Spec s = basic::parse_spec(key);
        std::string rk = jstr(r, "key", s.path);
        if (r.contains("tree") && r["tree"].is_array()) {
            std::vector<os::RegTree> tree;
            for (auto& n : r["tree"]) {
                if (!n.is_object()) continue;
                os::RegTree t;
                t.path = jstr(n, "path");
                for (auto& v : n["values"]) {
                    if (!v.is_object()) continue;
                    os::RegValue rv;
                    rv.name = jstr(v, "name");
                    rv.type = v.contains("type") && v["type"].is_number_unsigned() ? v["type"].get<uint32_t>() : 1u;
                    rv.data = unb64(jstr(v, "data"));
                    t.values.push_back(rv);
                }
                tree.push_back(t);
            }
            if (!os::reg_import_tree(rk, tree, true)) { ok = false; note("registry " + rk); }
        } else if (r.contains("values")) {
            if (!reg_restore_values(rk, r["values"])) { ok = false; note("registry " + rk); }
        }
    }
    for (auto& [src_raw, dst_j] : lf.items()) {
        if (!dst_j.is_string()) continue;
        std::string dst = dst_j.get<std::string>();
        basic::Spec s = basic::parse_spec(src_raw);
        if (s.kind == basic::Spec::Reg) continue;  // done above
        if (s.kind == basic::Spec::Json) {
            if (!jsv.contains(src_raw)) continue;
            std::string path = expand(s.path), t2;
            json j = json::object();
            if (fsx::read_file(path, t2)) {
                j = json::parse(t2, nullptr, false, true);
                if (j.is_discarded()) j = json::object();
            }
            basic::json_set(j, s.json_path, jsv[src_raw]);
            if (!fsx::write_file(path, j.dump(4, ' ', false, json::error_handler_t::replace))) { ok = false; note(path); }
            continue;
        }
        std::string path = expand(s.path);
        std::string saved = fsx::join(dir, dst);
        if (!fsx::exists(saved)) continue;
        std::string e;
        if (has_wildcard(path)) {
            std::string parent = fsx::parent(path);
            fsx::mkdirs(parent);
            std::string pat = fsx::filename(path);
            for (auto& f : fsx::list_dir(saved))
                if (fsx::wildcard_match(pat, f) && !fsx::copy_any(fsx::join(saved, f), fsx::join(parent, f), &e)) { ok = false; note(e); }
        } else if (fsx::is_dir(saved)) {
            std::string inner = fsx::join(saved, fsx::filename(path));
            bool file_in_folder = (ends_with(dst, "\\") || ends_with(dst, "/")) && fsx::is_file(inner);
            if (file_in_folder) {
                if (!fsx::copy_any(inner, path, &e)) { ok = false; note(e); }
            } else {
                fsx::mkdirs(path);
                if (!fsx::copy_any(saved, path, &e)) { ok = false; note(e); }
            }
        } else {
            if (!fsx::copy_any(saved, path, &e)) { ok = false; note(e); }
        }
    }
    if (!ok) {
        err = TR("часть данных не восстановилась: ", "some data could not be restored: ") + first_err;
        log::warn(id_ + ": restore " + first_err);
    }
    return ok;
}

// ---------------------------------------------------------------- new Epic Games Launcher
static const char* kEpicCredTargets[] = {"credentials.com.epicgames.eglv2.prod", "LegacyGeneric:target=credentials.com.epicgames.eglv2.prod"};
static std::string epic_auth_dir() { return fsx::join(os::appdata_local(), "com.epicgames.eglv2\\auth"); }

bool BasicPlatform::epic_new() {
    if (id_ != "epic") return false;
    std::string exe = exe_path();
    if (exe.empty()) return false;
    return iequals(fsx::parent(exe), fsx::join(os::appdata_local(), "Epic Games\\Launcher"));
}

static bool epic_read_auth(std::map<std::string, std::string>& files) {
    std::string dir = epic_auth_dir();
    for (auto& f : fsx::list_dir(dir)) {
        std::string p = fsx::join(dir, f);
        if (!fsx::is_file(p) || fsx::file_size(p) > 4u * 1024 * 1024) continue;
        std::string d;
        if (fsx::read_file(p, d)) files[f] = d;
    }
    return files.count("access-token-prod.dpapi") && files.count("auth.json");
}

static std::string epic_account_from(const std::map<std::string, std::string>& files) {
    auto it = files.find("access-token-prod.dpapi");
    if (it == files.end()) return {};
    std::vector<uint8_t> in(it->second.begin(), it->second.end()), out;
    if (!os::dpapi_unprotect(in, out)) return {};
    json j = json::parse(std::string(out.begin(), out.end()), nullptr, false);
    std::fill(out.begin(), out.end(), 0);
    if (!j.is_object()) return {};
    return trim(j.value("account_id", std::string()));
}

std::string BasicPlatform::epic_uid() {
    std::map<std::string, std::string> files;
    if (!epic_read_auth(files)) return {};
    return epic_account_from(files);
}

bool BasicPlatform::epic_save(const std::string& dir, std::string& err) {
    os::Credential c;
    bool have = false;
    for (auto* t : kEpicCredTargets)
        if (os::cred_read(t, c) && !c.blob.empty()) { have = true; break; }
    if (!have) { err = TR("в Epic Games Launcher не выполнен вход", "Epic Games Launcher is not signed in"); return false; }
    std::map<std::string, std::string> files;
    if (!epic_read_auth(files)) { err = TR("не найдены файлы входа Epic", "Epic auth files not found"); return false; }
    std::string uid = epic_account_from(files);
    if (uid.empty()) { err = TR("не удалось прочитать аккаунт Epic", "could not read the Epic account"); return false; }
    json snap = {{"version", 1}, {"uniqueId", uid},
                 {"credential", {{"target", c.target}, {"user", c.username}, {"alias", c.alias}, {"comment", c.comment}, {"persist", c.persist}, {"blob", b64(c.blob)}}}};
    json fj = json::object();
    for (auto& [n, d] : files) fj[n] = b64(std::vector<uint8_t>(d.begin(), d.end()));
    snap["files"] = fj;
    std::string plain = snap.dump();
    std::vector<uint8_t> in(plain.begin(), plain.end()), prot;
    std::fill(plain.begin(), plain.end(), 0);
    if (!os::dpapi_protect(in, prot)) { err = "DPAPI"; return false; }
    std::fill(in.begin(), in.end(), 0);
    return fsx::write_file(fsx::join(dir, "epic-session.dpapi"), std::string(prot.begin(), prot.end()));
}

bool BasicPlatform::epic_restore(const std::string& dir, std::string& err) {
    std::string prot;
    if (!fsx::read_file(fsx::join(dir, "epic-session.dpapi"), prot)) {
        err = TR("для этого аккаунта нет сохранённой сессии нового Epic Launcher — войдите в него и сохраните заново",
                 "this account has no saved session for the new Epic launcher — sign in and save it again");
        return false;
    }
    std::vector<uint8_t> in(prot.begin(), prot.end()), out;
    if (!os::dpapi_unprotect(in, out)) { err = TR("не удалось расшифровать сохранение Epic", "could not decrypt the Epic save"); return false; }
    json snap = json::parse(std::string(out.begin(), out.end()), nullptr, false);
    std::fill(out.begin(), out.end(), 0);
    if (!snap.is_object() || !snap["credential"].is_object() || !snap["files"].is_object()) { err = "epic snapshot"; return false; }
    std::map<std::string, std::string> files;
    for (auto& [n, v] : snap["files"].items()) {
        if (!v.is_string() || n.find_first_of("\\\\/:") != std::string::npos || n == "." || n == "..") continue;
        auto d = unb64(v.get<std::string>());
        files[n] = std::string(d.begin(), d.end());
    }
    if (!files.count("access-token-prod.dpapi")) { err = "epic snapshot"; return false; }
    const json& cj = snap["credential"];
    os::Credential c;
    c.target = jstr(cj, "target", kEpicCredTargets[0]);
    c.username = jstr(cj, "user");
    c.alias = jstr(cj, "alias");
    c.comment = jstr(cj, "comment");
    c.persist = cj.contains("persist") && cj["persist"].is_number_unsigned() ? cj["persist"].get<uint32_t>() : 2u;
    if (c.persist < 1 || c.persist > 3) c.persist = 2;
    c.blob = unb64(jstr(cj, "blob"));
    if (c.blob.empty()) { err = "epic snapshot"; return false; }
    // stage new files, move current ones aside, swap; roll back on any failure
    std::string ad = epic_auth_dir();
    std::string parent = fsx::parent(ad);
    std::string stage = fsx::join(parent, ".vs-epic-stage"), backup = fsx::join(parent, ".vs-epic-backup");
    fsx::remove_all(stage);
    fsx::remove_all(backup);
    fsx::mkdirs(stage);
    bool ok = true;
    for (auto& [n, d] : files) ok &= fsx::write_file(fsx::join(stage, n), d);
    if (!ok) { fsx::remove_all(stage); err = TR("не удалось подготовить файлы Epic", "could not stage Epic files"); return false; }
    fsx::mkdirs(ad);
    std::error_code ec;
    fsx::fs::rename(fsx::P(ad), fsx::P(backup), ec);
    if (ec) { fsx::remove_all(stage); err = TR("файлы Epic заняты — закройте лаунчер", "Epic files are busy — close the launcher"); return false; }
    fsx::fs::rename(fsx::P(stage), fsx::P(ad), ec);
    if (ec || !os::cred_write(c)) {
        fsx::remove_all(ad);
        std::error_code e2;
        fsx::fs::rename(fsx::P(backup), fsx::P(ad), e2);
        fsx::remove_all(stage);
        err = TR("не удалось восстановить сессию Epic", "could not restore the Epic session");
        return false;
    }
    std::fill(c.blob.begin(), c.blob.end(), 0);
    fsx::remove_all(backup);
    return true;
}

void BasicPlatform::epic_clear() {
    std::string ad = epic_auth_dir();
    for (auto& f : fsx::list_dir(ad))
        if (fsx::is_file(fsx::join(ad, f))) fsx::remove_file(fsx::join(ad, f));
    for (auto* t : kEpicCredTargets) os::cred_delete(t);
}

// ---------------------------------------------------------------- operations
// Saves the live login into account folder `sid` atomically (old copy kept if anything fails).
bool BasicPlatform::snapshot_into(const std::string& sid, std::string& err) {
    std::string dir = account_dir(sid);
    std::string tmp = dir + ".saving";
    fsx::remove_all(tmp);
    fsx::mkdirs(tmp);
    if (!backup_to(tmp, err)) { fsx::remove_all(tmp); return false; }
    for (const char* av : {"avatar.png", "avatar.jpg"})
        if (fsx::exists(fsx::join(dir, av))) fsx::copy_any(fsx::join(dir, av), fsx::join(tmp, av));
    if (!fsx::replace_dir(tmp, dir)) {
        fsx::remove_all(tmp);
        err = TR("не удалось заменить сохранение (папка занята)", "could not replace the save (folder busy)");
        return false;
    }
    return true;
}

// Safety net for platforms whose live account can't be identified: keep the last few sessions.
void BasicPlatform::autosave_unknown() {
    std::string root = fsx::join(store_.dir, "autosave\\" + id_);
    std::string dir = fsx::join(root, std::to_string(unix_now()));
    fsx::mkdirs(dir);
    std::string err;
    if (!backup_to(dir, err)) { fsx::remove_all(dir); return; }
    auto all = fsx::list_dir(root, true);
    std::sort(all.begin(), all.end());
    while (all.size() > 3) { fsx::remove_all(fsx::join(root, all.front())); all.erase(all.begin()); }
    log::info(id_ + ": autosaved unidentified session to " + dir);
}

OpResult BasicPlatform::save_current(const std::string& display, const std::string& id_hint, std::string& out_id) {
    auto t0 = clk::now();
    OpResult r;
    bool was_running = os::any_running(process_names());
    bool must_close = jbool(def_, "ExitBeforeSave") || jbool(def_, "ExitBeforeInteract");
    if (must_close && !close_now(nullptr, r)) return r;

    std::string uid = unique_id_now();
    std::string method = jstr(def_, "UniqueIdMethod");
    if (uid.empty() && iequals(method, "CREATE_ID_FILE")) {
        std::string f = expand(jstr(def_, "UniqueIdFile"));
        if (!f.empty() && f.find('%') == std::string::npos) {
            uid = basic::random_id().substr(0, 16);
            fsx::write_file(f, uid);
        }
    }
    std::string sid = uid.empty() ? (id_hint.empty() ? basic::random_id() : id_hint) : sanitize_filename(uid);
    if (!id_hint.empty() && !uid.empty() && sid != id_hint) {
        r.message = TR("Сейчас выполнен вход в другой аккаунт — сохраните его как новый.", "Another account is signed in now — save it as a new one.");
        if (was_running && must_close) launch();
        return r;
    }
    std::string err;
    if (!snapshot_into(sid, err)) {
        r.message = std::string(TR("Не удалось сохранить: ", "Could not save: ")) + err;
        if (was_running && must_close) launch();
        return r;
    }
    std::string dir = account_dir(sid);
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        auto& m = store_.meta[id_][sid];
        if (!display.empty()) m.display = display;
        m.saved_at = unix_now();
        if (!uid.empty() && (uid.find('@') != std::string::npos || uid.size() <= 24)) m.login = uid;
    }
    store_.save_meta();
    // profile picture from a URL template
    std::string pic = extras_str(def_, "ProfilePicPath");
    if (istarts_with(pic, "http") && !uid.empty() && !fsx::exists(fsx::join(dir, "avatar.png"))) {
        std::string url = ireplace_all(pic, "%UniqueId%", uid);
        std::string body;
        if (os::http_get(url, body, nullptr, 8000) && body.size() > 200) fsx::write_file(fsx::join(dir, "avatar.png"), body);
    }
    if (was_running && must_close) launch();
    out_id = sid;
    r.ok = true;
    r.seconds = std::chrono::duration<double>(clk::now() - t0).count();
    r.message = std::string(TR("Сохранено: ", "Saved: ")) + (display.empty() ? sid : display);
    log::info(id_ + ": saved account " + sid);
    return r;
}

OpResult BasicPlatform::switch_to(const SwitchRequest& req, const Progress& p) {
    auto t0 = clk::now();
    OpResult r;
    std::string err;
    // 1. validate the target before touching anything
    if (!req.account_id.empty() && !can_restore(account_dir(req.account_id), err)) {
        r.message = err;
        return r;
    }
    std::string cur = current_account_id();
    if (!req.account_id.empty() && req.extra_args.empty() && cur == req.account_id) {
        if (!os::any_running(process_names())) launch();
        r.ok = true;
        r.message = TR("Этот аккаунт уже активен", "This account is already active");
        return r;
    }
    if (!close_now(p, r)) return r;
    // 2. keep the current session: refresh its save (tokens rotate) or autosave an unknown one
    cur = current_account_id();
    bool cur_saved = false;
    if (!cur.empty()) {
        if (p) p(TR("Обновляю сохранение текущего аккаунта…", "Refreshing the current account's save…"));
        std::string e2;
        cur_saved = snapshot_into(cur, e2);
        if (!cur_saved) log::warn(id_ + ": could not refresh save of " + cur + ": " + e2);
    } else {
        autosave_unknown();
    }
    // 3. clear and restore
    if (p) p(TR("Применяю аккаунт…", "Applying account…"));
    clear_login(err);
    if (!req.account_id.empty()) {
        if (!restore_from(account_dir(req.account_id), err)) {
            // put the previous account back so the user isn't left signed out
            std::string e3;
            clear_login(e3);
            if (!cur.empty()) restore_from(account_dir(cur), e3);
            launch();
            r.message = std::string(TR("Не удалось переключить: ", "Switch failed: ")) + err +
                        (cur.empty() ? std::string() : std::string(TR(". Возвращён прежний аккаунт.", ". The previous account was restored.")));
            log::error(id_ + ": " + r.message);
            return r;
        }
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        auto& m = store_.meta[id_][req.account_id];
        m.last_switched = unix_now();
        m.switch_count++;
    }
    if (p) p(std::string(TR("Запускаю ", "Starting ")) + name_ + "…");
    launch();
    store_.save_meta();
    r.ok = true;
    r.seconds = std::chrono::duration<double>(clk::now() - t0).count();
    log::info(id_ + ": switch -> " + (req.account_id.empty() ? std::string("<new>") : req.account_id) + " in " + std::to_string(r.seconds) + "s");
    return r;
}

OpResult BasicPlatform::forget(const std::string& id) {
    OpResult r;
    r.ok = fsx::remove_all(account_dir(id));
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        store_.meta[id_].erase(id);
    }
    store_.save_meta();
    r.message = r.ok ? TR("Аккаунт удалён из свитчера", "Account removed from the switcher") : TR("Не удалось удалить папку аккаунта", "Could not delete the account folder");
    return r;
}

// ---------------------------------------------------------------- catalogue
std::vector<std::unique_ptr<Platform>> make_basic_platforms(Store& st, const std::string& text) {
    std::vector<std::unique_ptr<Platform>> out;
    nlohmann::ordered_json j = nlohmann::ordered_json::parse(text, nullptr, false, true);
    if (j.is_discarded()) { log::error("platforms.json: parse error"); return out; }
    if (!j.is_object()) return out;
    const nlohmann::ordered_json& pl = j.contains("Platforms") ? j["Platforms"] : j;
    if (!pl.is_object()) return out;
    for (auto& [name, def] : pl.items()) {
        try {
            if (!def.is_object() || iequals(name, "Steam")) continue;
            if (!def.contains("LoginFiles") || !def["LoginFiles"].is_object()) continue;
            json d = json::parse(def.dump());
            d["_name"] = name;
            std::string id = jstr(d, "vsId", basic::slug(name));
            out.push_back(std::make_unique<BasicPlatform>(st, id, d));
        } catch (const std::exception& e) {
            log::error("platforms.json: skipping '" + name + "': " + e.what());
        }
    }
    return out;
}

}  // namespace vs
