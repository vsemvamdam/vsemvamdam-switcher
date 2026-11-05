// vsemvamdam-switcher — generic platform engine driven by a TcNo-compatible Platforms.json
#include "basic_platform.h"

#include <algorithm>
#include <chrono>
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
    name_ = def_.value("vsName", def_.value("_name", id_));
    help_ = def_.value(g_lang_en ? "vsHelpEn" : "vsHelp", std::string());
}

std::string BasicPlatform::saves_dir() const { return fsx::join(store_.dir, "platforms\\" + id_); }
std::string BasicPlatform::account_dir(const std::string& id) const { return fsx::join(saves_dir(), sanitize_filename(id)); }

std::string BasicPlatform::platform_folder() {
    std::string exe = exe_path();
    return exe.empty() ? std::string() : fsx::parent(exe);
}

std::string BasicPlatform::expand(const std::string& s) {
    static std::map<std::string, std::string> base;
    if (base.empty()) {
        base["appdata"] = os::appdata_roaming();
        base["localappdata"] = os::appdata_local();
        base["programdata"] = os::programdata();
        base["programfiles"] = os::program_files();
        base["programfiles(x86)"] = os::program_files_x86();
        base["userprofile"] = os::user_profile();
        base["documents"] = os::documents_dir();
        base["desktop"] = os::desktop_dir();
        base["music"] = fsx::join(os::user_profile(), "Music");
        base["pictures"] = fsx::join(os::user_profile(), "Pictures");
        base["videos"] = fsx::join(os::user_profile(), "Videos");
        base["startmenuappdata"] = os::start_menu_programs();
        base["startmenuprogramdata"] = fsx::join(os::programdata(), "Microsoft\\Windows\\Start Menu\\Programs");
        base["temp"] = os::expand_env("%TEMP%");
        base["windir"] = os::expand_env("%WINDIR%");
        base["systemdrive"] = os::expand_env("%SYSTEMDRIVE%");
        base["public"] = os::expand_env("%PUBLIC%");
    }
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
                auto d = str_list(def_.value("ExeLocationDefault", json()));
                if (!d.empty()) p = fsx::join(p, fsx::filename(expand(d[0])));
            }
            return p;
        }
    }
    auto defs = str_list(def_.value("ExeLocationDefault", json()));
    // expand without %Platform_Folder% (would recurse)
    std::vector<std::string> cands;
    for (auto& d : defs)
        if (!icontains(d, "%Platform_Folder%")) cands.push_back(expand(d));
    for (auto& c : cands)
        if (fsx::exists(c)) return c;
    static std::map<std::string, std::string> shortcut_cache;
    std::string sc = def_.value("GetPathFromShortcutNamed", std::string());
    if (!sc.empty()) {
        auto it = shortcut_cache.find(id_);
        if (it == shortcut_cache.end()) it = shortcut_cache.emplace(id_, os::find_shortcut_target(sc)).first;
        if (!it->second.empty() && fsx::exists(it->second)) {
            // a game shortcut may point to the game; keep the client exe name from defaults when possible
            if (!cands.empty()) {
                std::string alt = fsx::join(fsx::parent(it->second), fsx::filename(cands[0]));
                if (fsx::exists(alt)) return alt;
            }
            return it->second;
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
    for (auto& n : str_list(def_.value("ExesToEnd", json())))
        if (!istarts_with(n, "SERVICE:")) out.push_back(n);
    return out;
}

std::string BasicPlatform::data_folder() { return platform_folder(); }

// ---------------------------------------------------------------- unique id
std::string BasicPlatform::unique_id_now() {
    if (epic_new()) return epic_uid();
    std::string file = def_.value("UniqueIdFile", std::string());
    std::string method = def_.value("UniqueIdMethod", std::string());
    std::string rx = def_.value("UniqueIdRegex", std::string());
    if (file.empty()) return {};
    if (iequals(method, "LEVELDB") || iequals(method, "STEAM")) return {};  // not supported: no reliable live id
    if (iequals(method, "REGKEY")) {
        basic::Spec s = basic::parse_spec(istarts_with(file, "REG:") ? file : "REG:" + file);
        std::string v;
        if (is_value_glob(s.value_name)) {
            auto m = reg_matching(s.path, s.value_name);
            std::sort(m.begin(), m.end(), [](auto& a, auto& b) { return a.name < b.name; });
            if (!m.empty()) os::reg_get_string(s.path, m[0].name, v);
        } else {
            os::reg_get_string(s.path, s.value_name, v);
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
    if (istarts_with(method, "JSON_SELECT") || istarts_with(file, "JSON_")) {
        std::string raw = istarts_with(file, "JSON_") ? file : method + "::" + file;
        basic::Spec s = basic::parse_spec(raw);
        std::string text;
        if (!fsx::read_file(expand(s.path), text)) return {};
        json j = json::parse(text, nullptr, false, true);
        if (j.is_discarded()) return {};
        const json* v = basic::json_get(j, s.json_path);
        if (!v) return {};
        std::string val = v->is_string() ? v->get<std::string>() : v->is_array() && !v->empty() && (*v)[0].is_string() ? (*v)[0].get<std::string>() : v->dump();
        if (s.json_mode == "JSON_SELECT_FIRST") val = basic::first_of(val, s.delimiter);
        return trim(val);
    }
    std::string path = expand(file);
    if (!fsx::exists(path)) {
        // relative name: look it up among LoginFiles destinations
        auto lf = def_.value("LoginFiles", json::object());
        for (auto& [src, dst] : lf.items())
            if (dst.is_string() && iequals(dst.get<std::string>(), file)) { path = expand(src); break; }
    }
    std::string text;
    if (!fsx::is_file(path) || !fsx::read_file(path, text)) return {};
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
std::vector<Account> BasicPlatform::scan() {
    std::vector<Account> out;
    std::string cur = current_account_id();
    for (auto& d : fsx::list_dir(saves_dir(), true)) {
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
    std::string method = def_.contains("Extras") ? def_["Extras"].value("ClosingMethod", std::string()) : std::string();
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
    std::string args = def_.value("ExeExtraArgs", std::string());
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        auto& pp = store_.settings.platforms[id_];
        if (!pp.launch_after) return;
        if (!pp.args.empty()) args += (args.empty() ? "" : " ") + pp.args;
    }
    os::launch(exe, args, fsx::parent(exe), true);
}

// ---------------------------------------------------------------- file ops
bool BasicPlatform::clear_login(std::string& err) {
    (void)err;
    if (epic_new()) { epic_clear(); return true; }
    auto lf = def_.value("LoginFiles", json::object());
    auto clear_spec = [&](const basic::Spec& s, bool empty_json) {
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
                (void)empty_json;
                fsx::write_file(path, j.dump(4));
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
    for (auto& entry : str_list(def_.value("PathListToClear", json()))) {
        if (iequals(entry, "SAME_AS_LOGIN_FILES")) {
            for (auto& [src, dst] : lf.items()) clear_spec(basic::parse_spec(src), true);
        } else {
            clear_spec(basic::parse_spec(entry), true);
        }
    }
    return true;
}

bool BasicPlatform::backup_to(const std::string& dir, std::string& err) {
    if (epic_new()) return epic_save(dir, err);
    auto lf = def_.value("LoginFiles", json::object());
    bool all_required = def_.value("AllFilesRequired", false);
    json reg = json::object(), jsv = json::object();
    int copied = 0, missing = 0;
    for (auto& [src_raw, dst_j] : lf.items()) {
        if (!dst_j.is_string()) continue;
        std::string dst = dst_j.get<std::string>();
        basic::Spec s = basic::parse_spec(src_raw);
        if (s.kind == basic::Spec::Reg) {
            if (iequals(dst, "UNIQUE_ID")) {
                os::RegValue v;
                if (os::reg_get_raw(s.path, s.value_name, v)) reg["UNIQUE_ID"] = {{"key", s.path}, {"name", s.value_name}, {"type", v.type}, {"data", b64(v.data)}};
                continue;
            }
            if (is_value_glob(s.value_name)) {
                json vals = json::array();
                for (auto& v : reg_matching(s.path, s.value_name)) vals.push_back({{"name", v.name}, {"type", v.type}, {"data", b64(v.data)}});
                if (vals.empty()) { ++missing; continue; }
                reg[dst] = {{"key", s.path}, {"values", vals}};
            } else if (s.value_name.empty()) {
                std::vector<os::RegTree> tree;
                if (!os::reg_export_tree(s.path, tree)) { ++missing; continue; }
                json t = json::array();
                for (auto& node : tree) {
                    json vals = json::array();
                    for (auto& v : node.values) vals.push_back({{"name", v.name}, {"type", v.type}, {"data", b64(v.data)}});
                    t.push_back({{"path", node.path}, {"values", vals}});
                }
                reg[dst] = {{"key", s.path}, {"tree", t}};
            } else {
                os::RegValue v;
                if (!os::reg_get_raw(s.path, s.value_name, v)) { ++missing; continue; }
                reg[dst] = {{"key", s.path}, {"name", s.value_name}, {"type", v.type}, {"data", b64(v.data)}};
            }
            ++copied;
        } else if (s.kind == basic::Spec::Json) {
            std::string text;
            if (!fsx::read_file(expand(s.path), text)) { ++missing; continue; }
            json j = json::parse(text, nullptr, false, true);
            const json* v = j.is_discarded() ? nullptr : basic::json_get(j, s.json_path);
            if (!v) { ++missing; continue; }
            json val = *v;
            if (s.json_mode == "JSON_SELECT_FIRST" && val.is_string()) val = basic::first_of(val.get<std::string>(), s.delimiter);
            if (val.is_string() && val.get<std::string>().empty()) { ++missing; continue; }
            jsv[dst] = val;
            ++copied;
        } else {
            std::string path = expand(s.path);
            std::string target = fsx::join(dir, dst);
            if (has_wildcard(path)) {
                auto matches = fsx::glob(path);
                if (matches.empty()) { ++missing; continue; }
                fsx::mkdirs(target);
                for (auto& m : matches) {
                    std::string e;
                    if (fsx::copy_any(m, fsx::join(target, fsx::filename(m)), &e)) ++copied;
                    else log::warn(id_ + ": copy failed " + e);
                }
            } else {
                if (!fsx::exists(path)) { ++missing; if (all_required) { err = path; return false; } continue; }
                std::string e;
                bool dir_dst = ends_with(dst, "\\") || ends_with(dst, "/");
                if (fsx::is_dir(path)) {
                    if (!fsx::copy_any(path, target, &e)) log::warn(id_ + ": copy failed " + e);
                } else {
                    std::string t2 = dir_dst ? fsx::join(target, fsx::filename(path)) : target;
                    if (!fsx::copy_any(path, t2, &e)) { log::warn(id_ + ": copy failed " + e); if (all_required) { err = e; return false; } }
                }
                ++copied;
            }
        }
    }
    if (all_required && missing > 0) { err = TR("не все файлы входа найдены", "not all login files were found"); return false; }
    if (copied == 0) { err = TR("клиент не хранит вход — войдите в аккаунт", "nothing to save — sign in first"); return false; }
    if (!reg.empty()) fsx::write_file(fsx::join(dir, "_vs_registry.json"), reg.dump(1));
    if (!jsv.empty()) fsx::write_file(fsx::join(dir, "_vs_json.json"), jsv.dump(1));
    return true;
}

bool BasicPlatform::restore_from(const std::string& dir, std::string& err) {
    if (epic_new()) return epic_restore(dir, err);
    auto lf = def_.value("LoginFiles", json::object());
    json reg, jsv;
    std::string text;
    if (fsx::read_file(fsx::join(dir, "_vs_registry.json"), text)) reg = json::parse(text, nullptr, false);
    if (fsx::read_file(fsx::join(dir, "_vs_json.json"), text)) jsv = json::parse(text, nullptr, false);
    std::string acc_id = fsx::filename(dir);
    bool ok = true;
    for (auto& [src_raw, dst_j] : lf.items()) {
        if (!dst_j.is_string()) continue;
        std::string dst = dst_j.get<std::string>();
        basic::Spec s = basic::parse_spec(src_raw);
        if (s.kind == basic::Spec::Reg) {
            if (iequals(dst, "UNIQUE_ID") && reg.is_object() && reg.contains("UNIQUE_ID")) {
                os::RegValue rv;
                rv.name = s.value_name;
                rv.type = reg["UNIQUE_ID"].value("type", 1u);
                rv.data = unb64(reg["UNIQUE_ID"].value("data", std::string()));
                ok &= os::reg_set_raw(s.path, rv);
                continue;
            }
            if (iequals(dst, "UNIQUE_ID")) {
                std::string real_id = acc_id;
                {
                    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
                    auto& m = store_.meta[id_][acc_id];
                    if (!m.login.empty() && sanitize_filename(m.login) == acc_id) real_id = m.login;
                }
                os::reg_set_string(s.path, s.value_name, real_id);
                continue;
            }
            if (!reg.is_object() || !reg.contains(dst)) continue;
            auto& r = reg[dst];
            if (r.contains("values")) {
                for (auto& v : r["values"]) {
                    os::RegValue rv;
                    rv.name = v.value("name", std::string());
                    rv.type = v.value("type", 1u);
                    rv.data = unb64(v.value("data", std::string()));
                    ok &= os::reg_set_raw(s.path, rv);
                }
            } else if (r.contains("tree")) {
                std::vector<os::RegTree> tree;
                for (auto& n : r["tree"]) {
                    os::RegTree t;
                    t.path = n.value("path", std::string());
                    for (auto& v : n["values"]) {
                        os::RegValue rv;
                        rv.name = v.value("name", std::string());
                        rv.type = v.value("type", 1u);
                        rv.data = unb64(v.value("data", std::string()));
                        t.values.push_back(rv);
                    }
                    tree.push_back(t);
                }
                ok &= os::reg_import_tree(s.path, tree, true);
            } else {
                os::RegValue rv;
                rv.name = s.value_name;
                rv.type = r.value("type", 1u);
                rv.data = unb64(r.value("data", std::string()));
                ok &= os::reg_set_raw(s.path, rv);
            }
        } else if (s.kind == basic::Spec::Json) {
            if (!jsv.is_object() || !jsv.contains(dst)) continue;
            std::string path = expand(s.path), t2;
            json j = json::object();
            if (fsx::read_file(path, t2)) {
                j = json::parse(t2, nullptr, false, true);
                if (j.is_discarded()) j = json::object();
            }
            basic::json_set(j, s.json_path, jsv[dst]);
            ok &= fsx::write_file(path, j.dump(4));
        } else {
            std::string path = expand(s.path);
            std::string saved = fsx::join(dir, dst);
            if (!fsx::exists(saved)) continue;
            std::string e;
            if (has_wildcard(path)) {
                std::string parent = fsx::parent(path);
                fsx::mkdirs(parent);
                std::string pat = fsx::filename(path);
                for (auto& f : fsx::list_dir(saved))
                    if (fsx::wildcard_match(pat, f)) ok &= fsx::copy_any(fsx::join(saved, f), fsx::join(parent, f), &e);
            } else if (fsx::is_dir(saved)) {
                std::string inner = fsx::join(saved, fsx::filename(path));
                bool file_in_folder = (ends_with(dst, "\\") || ends_with(dst, "/")) && fsx::is_file(inner);
                if (file_in_folder) {
                    ok &= fsx::copy_any(inner, path, &e);
                } else {
                    fsx::mkdirs(path);
                    ok &= fsx::copy_any(saved, path, &e);
                }
            } else {
                ok &= fsx::copy_any(saved, path, &e);
            }
            if (!e.empty()) log::warn(id_ + ": restore " + e);
        }
    }
    if (!ok) err = TR("часть файлов не восстановилась (см. журнал)", "some files could not be restored (see log)");
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
    if (!os::dpapi_unprotect(in, out)) { err = "DPAPI"; return false; }
    json snap = json::parse(std::string(out.begin(), out.end()), nullptr, false);
    std::fill(out.begin(), out.end(), 0);
    if (!snap.is_object() || !snap.contains("credential") || !snap.contains("files")) { err = "snapshot"; return false; }
    std::map<std::string, std::string> files;
    for (auto& [n, v] : snap["files"].items()) {
        if (n.find_first_of("\\/:") != std::string::npos || n == "." || n == "..") continue;
        auto d = unb64(v.get<std::string>());
        files[n] = std::string(d.begin(), d.end());
    }
    if (!files.count("access-token-prod.dpapi")) { err = "snapshot"; return false; }
    // replace regular files in the auth folder
    std::string ad = epic_auth_dir();
    fsx::mkdirs(ad);
    for (auto& f : fsx::list_dir(ad))
        if (fsx::is_file(fsx::join(ad, f))) fsx::remove_file(fsx::join(ad, f));
    bool ok = true;
    for (auto& [n, d] : files) ok &= fsx::write_file(fsx::join(ad, n), d);
    auto& cj = snap["credential"];
    os::Credential c;
    c.target = cj.value("target", std::string(kEpicCredTargets[0]));
    c.username = cj.value("user", std::string());
    c.alias = cj.value("alias", std::string());
    c.comment = cj.value("comment", std::string());
    c.persist = cj.value("persist", 2u);
    c.blob = unb64(cj.value("blob", std::string()));
    ok &= os::cred_write(c);
    std::fill(c.blob.begin(), c.blob.end(), 0);
    if (!ok) err = TR("не удалось восстановить сессию Epic", "could not restore the Epic session");
    return ok;
}

void BasicPlatform::epic_clear() {
    std::string ad = epic_auth_dir();
    for (auto& f : fsx::list_dir(ad))
        if (fsx::is_file(fsx::join(ad, f))) fsx::remove_file(fsx::join(ad, f));
    for (auto* t : kEpicCredTargets) os::cred_delete(t);
}

// ---------------------------------------------------------------- operations
OpResult BasicPlatform::save_current(const std::string& display, const std::string& id_hint, std::string& out_id) {
    auto t0 = clk::now();
    OpResult r;
    bool was_running = os::any_running(process_names());
    bool must_close = def_.value("ExitBeforeSave", false) || def_.value("ExitBeforeInteract", false);
    if (must_close && !close_now(nullptr, r)) return r;

    std::string uid = unique_id_now();
    std::string method = def_.value("UniqueIdMethod", std::string());
    if (uid.empty() && iequals(method, "CREATE_ID_FILE")) {
        std::string f = expand(def_.value("UniqueIdFile", std::string()));
        if (!f.empty()) {
            uid = basic::random_id();
            fsx::write_file(f, uid);
        }
    }
    std::string sid = uid.empty() ? (id_hint.empty() ? basic::random_id() : id_hint) : sanitize_filename(uid);
    if (!id_hint.empty() && !uid.empty() && sid != id_hint) {
        r.message = TR("Сейчас выполнен вход в другой аккаунт — сохраните его как новый.", "Another account is signed in now — save it as a new one.");
        if (was_running && must_close) launch();
        return r;
    }
    std::string dir = account_dir(sid);
    std::string tmp = dir + ".saving";
    fsx::remove_all(tmp);
    fsx::mkdirs(tmp);
    std::string err;
    if (!backup_to(tmp, err)) {
        fsx::remove_all(tmp);
        r.message = std::string(TR("Не удалось сохранить: ", "Could not save: ")) + err;
        if (was_running && must_close) launch();
        return r;
    }
    // keep a custom avatar from a previous save
    for (const char* av : {"avatar.png", "avatar.jpg"})
        if (fsx::exists(fsx::join(dir, av))) fsx::copy_any(fsx::join(dir, av), fsx::join(tmp, av));
    fsx::remove_all(dir);
    std::error_code ec;
    fsx::fs::rename(fsx::P(tmp), fsx::P(dir), ec);
    if (ec) { fsx::copy_any(tmp, dir); fsx::remove_all(tmp); }
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        auto& m = store_.meta[id_][sid];
        if (!display.empty()) m.display = display;
        m.saved_at = unix_now();
        if (!uid.empty() && (uid.find('@') != std::string::npos || uid.size() <= 24)) m.login = uid;
    }
    store_.save_meta();
    // profile picture from a URL template
    if (def_.contains("Extras")) {
        std::string pic = def_["Extras"].value("ProfilePicPath", std::string());
        if (istarts_with(pic, "http") && !uid.empty()) {
            std::string url = ireplace_all(pic, "%UniqueId%", uid);
            std::string body;
            if (os::http_get(url, body, nullptr, 8000) && body.size() > 200) fsx::write_file(fsx::join(dir, "avatar.png"), body);
        }
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
    if (!req.account_id.empty() && !fsx::is_dir(account_dir(req.account_id))) {
        r.message = TR("Сохранённые данные аккаунта не найдены.", "Saved account data not found.");
        return r;
    }
    if (!req.account_id.empty() && req.extra_args.empty() && current_account_id() == req.account_id) {
        if (!os::any_running(process_names())) launch();
        r.ok = true;
        r.message = TR("Этот аккаунт уже активен", "This account is already active");
        return r;
    }
    if (!close_now(p, r)) return r;
    // Before leaving the current account, refresh its saved copy (tokens rotate over time)
    std::string cur = current_account_id();
    if (!cur.empty() && cur != req.account_id) {
        if (p) p(TR("Обновляю сохранение текущего аккаунта…", "Refreshing the current account's save…"));
        std::string tmp = account_dir(cur) + ".saving", err;
        fsx::remove_all(tmp);
        fsx::mkdirs(tmp);
        if (backup_to(tmp, err)) {
            for (const char* av : {"avatar.png", "avatar.jpg"})
                if (fsx::exists(fsx::join(account_dir(cur), av))) fsx::copy_any(fsx::join(account_dir(cur), av), fsx::join(tmp, av));
            fsx::remove_all(account_dir(cur));
            std::error_code ec;
            fsx::fs::rename(fsx::P(tmp), fsx::P(account_dir(cur)), ec);
            if (ec) { fsx::copy_any(tmp, account_dir(cur)); fsx::remove_all(tmp); }
        } else {
            fsx::remove_all(tmp);
        }
    }
    if (p) p(TR("Применяю аккаунт…", "Applying account…"));
    std::string err;
    clear_login(err);
    if (!req.account_id.empty()) {
        if (!restore_from(account_dir(req.account_id), err)) log::warn(id_ + ": " + err);
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
    if (!err.empty()) r.message = err;
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
    const nlohmann::ordered_json& pl = j.contains("Platforms") ? j["Platforms"] : j;
    if (!pl.is_object()) return out;
    for (auto& [name, def] : pl.items()) {
        if (!def.is_object() || iequals(name, "Steam")) continue;
        if (!def.contains("LoginFiles")) continue;
        json d = json::parse(def.dump());
        d["_name"] = name;
        std::string id = d.value("vsId", basic::slug(name));
        out.push_back(std::make_unique<BasicPlatform>(st, id, d));
    }
    return out;
}

}  // namespace vs
