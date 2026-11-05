#include "steam_platform.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "../core/fsx.h"
#include "../core/i18n.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../core/steam_core.h"
#include "../core/str.h"
#include "../os/win.h"

namespace vs {

using clk = std::chrono::steady_clock;

const char* steam_state_label(int st) {
    switch (st) {
        case kStateKeep: return TR("Как было", "As before");
        case steam::Online: return TR("В сети", "Online");
        case steam::Away: return TR("Нет на месте", "Away");
        case steam::Invisible: return TR("Невидимка", "Invisible");
        case steam::Offline: return TR("Не в сети", "Offline");
        case steam::Busy: return TR("Не беспокоить", "Busy");
        case kStateOfflineMode: return TR("Офлайн-режим", "Offline mode");
        default: return TR("По умолчанию", "Default");
    }
}

static const char* kSteamReg = "HKCU\\Software\\Valve\\Steam";

std::string SteamPlatform::root() {
    std::string custom;
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        custom = store_.settings.steam_path;
    }
    auto norm = [](std::string p) {
        p = replace_all(p, "/", "\\");
        while (!p.empty() && (p.back() == '\\')) p.pop_back();
        if (ends_with(to_lower(p), "\\steam.exe")) p = fsx::parent(p);
        return p;
    };
    if (!custom.empty() && fsx::is_dir(norm(custom))) return norm(custom);
    std::vector<std::string> cands;
    std::string v;
    if (os::reg_get_string(kSteamReg, "SteamPath", v) && !v.empty()) cands.push_back(norm(v));
    if (os::reg_get_string(kSteamReg, "SteamExe", v) && !v.empty()) cands.push_back(norm(v));
    if (os::reg_get_string("HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath", v) && !v.empty()) cands.push_back(norm(v));
    if (os::reg_get_string("HKLM\\SOFTWARE\\Valve\\Steam", "InstallPath", v, os::RegView::Wow64_32) && !v.empty()) cands.push_back(norm(v));
    std::string pf86 = os::program_files_x86();
    if (!pf86.empty()) cands.push_back(fsx::join(pf86, "Steam"));
    cands.push_back("C:\\Program Files (x86)\\Steam");
    for (auto& c : cands)
        if (fsx::exists(fsx::join(c, "config\\loginusers.vdf"))) return c;
    for (auto& c : cands)
        if (fsx::exists(fsx::join(c, "steam.exe"))) return c;
    return cands.empty() ? std::string() : cands.front();
}

std::string SteamPlatform::exe_path() { return fsx::join(root(), "steam.exe"); }
bool SteamPlatform::installed() { return fsx::exists(exe_path()) || fsx::exists(fsx::join(root(), "config\\loginusers.vdf")); }

std::vector<std::string> SteamPlatform::process_names() const {
    return {"steam.exe", "steamwebhelper.exe", "GameOverlayUI.exe", "steamerrorreporter.exe", "steamerrorreporter64.exe"};
}

std::string SteamPlatform::userdata_dir(const std::string& id64) {
    uint64_t v;
    if (!parse_u64(id64, v)) return {};
    return fsx::join(root(), "userdata\\" + std::to_string(steam::account_id(v)));
}

std::string SteamPlatform::avatar_for(const std::string& id64) {
    std::string ours = fsx::join(store_.dir, "avatars\\steam\\" + id64 + ".jpg");
    if (fsx::exists(ours)) return ours;
    std::string cache = fsx::join(root(), "config\\avatarcache\\" + id64 + ".png");
    if (fsx::exists(cache)) return cache;
    return {};
}

static bool read_loginusers(const std::string& root, std::string& text, std::vector<steam::LoginUser>& users) {
    std::string path = fsx::join(root, "config\\loginusers.vdf");
    std::string err;
    if (fsx::read_file(path, text) && steam::parse_loginusers(text, users, &err) && !users.empty()) return true;
    if (!err.empty()) log::warn("loginusers.vdf parse error: " + err);
    std::string bak;
    std::vector<steam::LoginUser> bu;
    if (fsx::read_file(path + ".vs-backup", bak) && steam::parse_loginusers(bak, bu) && !bu.empty()) {
        log::warn("using loginusers.vdf backup");
        // Only fall back when the main file is unreadable, not when it is legitimately empty
        if (!err.empty() || text.empty()) { text = bak; users = bu; return true; }
    }
    return err.empty();
}

std::vector<Account> SteamPlatform::scan() {
    std::vector<Account> out;
    std::string r = root();
    std::string text;
    std::vector<steam::LoginUser> users;
    read_loginusers(r, text, users);
    std::string active = steam::active_id64(users);
    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
    auto& metas = store_.meta["steam"];
    for (auto& u : users) {
        auto it = metas.find(u.id64);
        if (it != metas.end() && it->second.forgotten_at > 0 && u.timestamp <= it->second.forgotten_at) continue;
        Account a;
        a.id = u.id64;
        a.login = u.account;
        a.display = u.persona.empty() ? u.account : u.persona;
        a.last_login = u.timestamp;
        a.current = u.id64 == active;
        a.offline_mode = u.offline;
        a.avatar_path = avatar_for(u.id64);
        AccountMeta& m = metas[u.id64];
        m.login = u.account;
        if (!u.persona.empty()) m.display = u.persona;
        if (m.forgotten_at && u.timestamp > m.forgotten_at) m.forgotten_at = 0;
        a.vac = m.vac;
        a.limited = m.limited;
        a.trade_ban = m.trade_ban;
        out.push_back(std::move(a));
    }
    return out;
}

std::string SteamPlatform::current_account_id() {
    std::string text;
    std::vector<steam::LoginUser> users;
    read_loginusers(root(), text, users);
    return steam::active_id64(users);
}

OpResult SteamPlatform::close_steam(const Progress& p) {
    OpResult r;
    r.ok = true;
    auto names = process_names();
    if (!os::any_running(names)) return r;
    SteamClose method;
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        method = store_.settings.steam_close;
    }
    if (p) p(TR("Закрываю Steam…", "Closing Steam…"));
    if (method != SteamClose::Instant && os::any_running({"steam.exe"})) {
        std::string exe = os::exe_path_of_running("steam.exe");
        if (exe.empty()) exe = exe_path();
        if (fsx::exists(exe)) {
            os::launch(exe, "-shutdown", fsx::parent(exe), true);
            uint32_t grace = method == SteamClose::Gentle ? 15000 : 2500;
            os::wait_all_exit({"steam.exe"}, grace);
        }
    }
    auto kr = os::kill_processes(names, 6000);
    if (kr.access_denied > 0 && os::any_running({"steam.exe"})) {
        r.ok = false;
        r.needs_admin = true;
        r.message = TR("Steam запущен от имени администратора — перезапустите свитчер с правами администратора.",
                       "Steam runs as administrator — restart the switcher as administrator.");
        return r;
    }
    if (!os::wait_all_exit({"steam.exe"}, 4000)) {
        r.ok = false;
        r.message = TR("Не удалось закрыть Steam.", "Could not close Steam.");
        return r;
    }
    return r;
}

OpResult SteamPlatform::close_client() {
    OpResult r = close_steam(nullptr);
    if (r.ok) r.message = TR("Steam закрыт", "Steam closed");
    return r;
}

OpResult SteamPlatform::switch_to(const SwitchRequest& req, const Progress& p) {
    auto t0 = clk::now();
    OpResult res;
    std::string r = root();
    if (r.empty() || !fsx::exists(fsx::join(r, "steam.exe"))) {
        res.message = TR("Steam не найден. Укажите папку Steam в настройках.", "Steam not found. Set the Steam folder in settings.");
        return res;
    }
    std::string lu_path = fsx::join(r, "config\\loginusers.vdf");

    // Resolve target & state before touching anything
    std::string text;
    std::vector<steam::LoginUser> users;
    bool readable = read_loginusers(r, text, users);
    if (!readable && fsx::exists(lu_path)) {
        // try anyway after closing Steam; the file may be mid-write
    }
    steam::SwitchEdit edit;
    edit.target_id64 = req.account_id;
    int state = req.state;
    std::string fav_app;
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        if (!req.account_id.empty()) {
            AccountMeta& m = store_.meta["steam"][req.account_id];
            if (state == kStateDefault) state = m.default_state;
            fav_app = m.favorite_app;
            edit.fallback.id64 = req.account_id;
            edit.fallback.account = m.login;
            edit.fallback.persona = m.display;
        }
        if (state == kStateDefault) state = store_.settings.steam_default_state;
    }
    if (!req.account_id.empty()) {
        bool known = std::any_of(users.begin(), users.end(), [&](auto& u) { return u.id64 == req.account_id; });
        if (!known && edit.fallback.account.empty()) {
            res.message = TR("Этот аккаунт неизвестен Steam — войдите в него через Steam один раз.",
                             "Steam doesn't know this account — sign in to it through Steam once.");
            return res;
        }
        // Already active and Steam running with no extra work: just bring Steam up
        if (steam::active_id64(users) == req.account_id && os::any_running({"steam.exe"}) && state == kStateKeep && req.app_id.empty() &&
            req.extra_args.empty()) {
            os::launch(fsx::join(r, "steam.exe"), "", r, true);
            res.ok = true;
            res.message = TR("Этот аккаунт уже активен", "This account is already active");
            return res;
        }
    }
    edit.offline = state == kStateOfflineMode;

    OpResult cr = close_steam(p);
    if (!cr.ok) return cr;

    if (p) p(TR("Применяю аккаунт…", "Applying account…"));
    // Re-read after Steam has exited: it may have rewritten the file on shutdown
    text.clear();
    users.clear();
    readable = read_loginusers(r, text, users);
    if (!readable && fsx::exists(lu_path) && edit.fallback.account.empty()) {
        res.message = TR("Файл loginusers.vdf повреждён — Steam не тронут.", "loginusers.vdf is damaged — nothing changed.");
        return res;
    }
    std::string out, account, err;
    if (!steam::rewrite_loginusers(readable ? text : std::string(), edit, out, account, &err)) {
        res.message = std::string(TR("Не удалось подготовить вход: ", "Could not prepare login: ")) + err;
        return res;
    }
    if (!text.empty()) fsx::write_file(lu_path + ".vs-backup", text);
    if (!fsx::write_file(lu_path, out)) {
        res.message = TR("Нет доступа к loginusers.vdf.", "Cannot write loginusers.vdf.");
        return res;
    }
    if (account.empty()) os::reg_delete_value(kSteamReg, "AutoLoginUser");
    else os::reg_set_string(kSteamReg, "AutoLoginUser", account);
    os::reg_set_dword(kSteamReg, "RememberPassword", 1);

    // Persona state (friends status) on next login
    if (!req.account_id.empty() && state >= 0 && state != kStateOfflineMode) {
        std::string lc = fsx::join(userdata_dir(req.account_id), "config\\localconfig.vdf");
        std::string lct;
        if (fsx::read_file(lc, lct)) {
            uint64_t v;
            parse_u64(req.account_id, v);
            if (steam::patch_persona_state(lct, steam::account_id(v), state)) fsx::write_file(lc, lct);
        }
    }

    bool launch;
    std::string args;
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        launch = store_.settings.steam_launch;
        args = store_.settings.steam_args;
        if (!req.account_id.empty()) {
            AccountMeta& m = store_.meta["steam"][req.account_id];
            m.last_switched = unix_now();
            m.switch_count++;
        }
    }
    std::string app = !req.app_id.empty() ? req.app_id : std::string();
    if (!app.empty()) args += (args.empty() ? "" : " ") + std::string("-applaunch ") + app;
    if (!req.extra_args.empty()) args += (args.empty() ? "" : " ") + req.extra_args;
    if (launch || !app.empty()) {
        if (p) p(TR("Запускаю Steam…", "Starting Steam…"));
        if (!os::launch(fsx::join(r, "steam.exe"), trim(args), r, true)) {
            res.message = TR("Аккаунт применён, но Steam не запустился.", "Account applied, but Steam failed to start.");
            return res;
        }
    }
    store_.save_meta();
    res.ok = true;
    res.seconds = std::chrono::duration<double>(clk::now() - t0).count();
    log::info("steam switch -> " + (req.account_id.empty() ? std::string("<new>") : req.account_id) + " state=" + std::to_string(state) +
              " in " + std::to_string(res.seconds) + "s");
    return res;
}

OpResult SteamPlatform::forget(const std::string& id64) {
    OpResult res;
    std::string r = root();
    std::string lu_path = fsx::join(r, "config\\loginusers.vdf");
    std::string text, out;
    std::string account;
    if (fsx::read_file(lu_path, text)) {
        std::vector<steam::LoginUser> users;
        steam::parse_loginusers(text, users);
        for (auto& u : users)
            if (u.id64 == id64) account = u.account;
        if (steam::remove_loginuser(text, id64, out)) {
            fsx::write_file(lu_path + ".vs-backup", text);
            fsx::write_file(lu_path, out);
        }
    }
    std::string cur;
    if (!account.empty() && os::reg_get_string(kSteamReg, "AutoLoginUser", cur) && iequals(cur, account))
        os::reg_delete_value(kSteamReg, "AutoLoginUser");
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        AccountMeta& m = store_.meta["steam"][id64];
        m.forgotten_at = unix_now();
        m.order = -1;
    }
    fsx::remove_file(fsx::join(store_.dir, "avatars\\steam\\" + id64 + ".jpg"));
    store_.save_meta();
    res.ok = true;
    res.message = TR("Аккаунт убран из списка", "Account removed from the list");
    return res;
}

std::vector<Game> SteamPlatform::games() {
    std::lock_guard<std::mutex> lk(gm_);
    if (games_loaded_) return games_;
    games_.clear();
    std::string r = root();
    std::vector<std::string> libs = {r};
    std::string lf;
    if (fsx::read_file(fsx::join(r, "steamapps\\libraryfolders.vdf"), lf))
        for (auto& p : steam::library_paths(lf)) {
            std::string n = replace_all(p, "/", "\\");
            if (std::none_of(libs.begin(), libs.end(), [&](auto& x) { return iequals(x, n); })) libs.push_back(n);
        }
    std::string libcache = fsx::join(r, "appcache\\librarycache");
    for (auto& lib : libs) {
        std::string sa = fsx::join(lib, "steamapps");
        for (auto& f : fsx::list_dir(sa)) {
            std::string lf2 = to_lower(f);
            if (!starts_with(lf2, "appmanifest_") || !ends_with(lf2, ".acf")) continue;
            std::string t;
            if (!fsx::read_file(fsx::join(sa, f), t)) continue;
            steam::AppManifest am;
            if (!steam::parse_appmanifest(t, am)) continue;
            if (am.appid == "228980" || icontains(am.name, "Steamworks Common") || icontains(am.name, "Redistributable") ||
                icontains(am.name, "Proton") || icontains(am.name, "Steam Linux Runtime"))
                continue;
            if (std::any_of(games_.begin(), games_.end(), [&](auto& g) { return g.appid == am.appid; })) continue;
            Game g;
            g.appid = am.appid;
            g.name = am.name.empty() ? "App " + am.appid : am.name;
            // icon: smallest jpg in librarycache\<appid>\ (new layout) or <appid>_icon.jpg (old)
            std::string dir = fsx::join(libcache, am.appid);
            uint64_t best = UINT64_MAX;
            for (auto& ic : fsx::list_dir(dir)) {
                std::string l = to_lower(ic);
                if (!ends_with(l, ".jpg") || l.find("header") != std::string::npos || l.find("library") != std::string::npos ||
                    l.find("logo") != std::string::npos || l.find("hero") != std::string::npos)
                    continue;
                std::string full = fsx::join(dir, ic);
                uint64_t sz = fsx::file_size(full);
                if (sz > 0 && sz < best) { best = sz; g.icon_path = full; }
            }
            if (g.icon_path.empty()) {
                std::string old = fsx::join(libcache, am.appid + "_icon.jpg");
                if (fsx::exists(old)) g.icon_path = old;
            }
            games_.push_back(std::move(g));
        }
    }
    std::sort(games_.begin(), games_.end(), [](const Game& a, const Game& b) { return fold_case_utf8(a.name) < fold_case_utf8(b.name); });
    games_loaded_ = true;
    return games_;
}

void SteamPlatform::refresh_profiles(bool force, const std::function<void(bool)>& done) {
    if (refreshing_.exchange(true)) return;
    std::thread([this, force, done] {
        bool changed = false;
        std::vector<std::string> ids;
        {
            std::string text;
            std::vector<steam::LoginUser> users;
            read_loginusers(root(), text, users);
            for (auto& u : users) ids.push_back(u.id64);
        }
        int64_t now = unix_now();
        std::string avdir = fsx::join(store_.dir, "avatars\\steam");
        fsx::mkdirs(avdir);
        int failures = 0;
        for (auto& id : ids) {
            int64_t checked;
            std::string old_url;
            {
                std::lock_guard<std::recursive_mutex> lk(store_.mtx);
                auto& m = store_.meta["steam"][id];
                checked = m.profile_checked;
                old_url = m.avatar_url;
            }
            std::string avatar_file = fsx::join(avdir, id + ".jpg");
            bool need = force || now - checked > 24 * 3600 || !fsx::exists(avatar_file);
            if (!need) continue;
            if (failures >= 3) break;  // network down, try later
            std::string body;
            int status = 0;
            if (!os::http_get("https://steamcommunity.com/profiles/" + id + "?xml=1", body, &status, 10000)) {
                ++failures;
                if (status == 429) break;
                continue;
            }
            steam::ProfileInfo pi;
            if (!steam::parse_profile_xml(body, pi)) continue;
            {
                std::lock_guard<std::recursive_mutex> lk(store_.mtx);
                auto& m = store_.meta["steam"][id];
                m.profile_checked = now;
                m.is_private = pi.is_private;
                if (!pi.is_private) {
                    m.vac = pi.vac;
                    m.limited = pi.limited;
                    m.trade_ban = pi.trade_ban;
                }
                if (!pi.persona.empty()) m.community_name = pi.persona;
                if (!pi.avatar_full.empty()) m.avatar_url = pi.avatar_full;
            }
            changed = true;
            if (!pi.avatar_full.empty() && (pi.avatar_full != old_url || !fsx::exists(avatar_file))) {
                std::string img;
                if (os::http_get(pi.avatar_full, img, nullptr, 15000) && img.size() > 100) {
                    fsx::write_file(avatar_file, img);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (changed) store_.save_meta();
        refreshing_ = false;
        if (done) done(changed);
    }).detach();
}

}  // namespace vs
