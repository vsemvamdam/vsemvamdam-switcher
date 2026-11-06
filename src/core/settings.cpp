#include "settings.h"

#include "fsx.h"
#include "log.h"
#include "nlohmann/json.hpp"
#include "str.h"
#ifdef _WIN32
#include "../os/win.h"
#endif

using nlohmann::json;

namespace vs {

template <class T>
static void rd(const json& j, const char* k, T& v) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return;
    try { v = it->get<T>(); } catch (...) {}
}

static const char* theme_s(Theme t) { return t == Theme::Dark ? "dark" : t == Theme::System ? "system" : "light"; }
static Theme theme_p(const std::string& s) { return s == "light" ? Theme::Light : s == "system" ? Theme::System : Theme::Dark; }
static const char* after_s(AfterSwitch a) {
    switch (a) { case AfterSwitch::Nothing: return "nothing"; case AfterSwitch::Tray: return "tray"; case AfterSwitch::Quit: return "quit"; default: return "minimize"; }
}
static AfterSwitch after_p(const std::string& s) {
    if (s == "nothing") return AfterSwitch::Nothing;
    if (s == "tray") return AfterSwitch::Tray;
    if (s == "quit") return AfterSwitch::Quit;
    return AfterSwitch::Minimize;
}
static const char* close_s(SteamClose c) { return c == SteamClose::Instant ? "instant" : c == SteamClose::Gentle ? "gentle" : "fast"; }
static SteamClose close_p(const std::string& s) { return s == "instant" ? SteamClose::Instant : s == "gentle" ? SteamClose::Gentle : SteamClose::Fast; }

std::string data_dir_default() {
#ifdef _WIN32
    std::string exe_dir = fsx::parent(os::self_exe_path());
    if (fsx::exists(fsx::join(exe_dir, "portable")) || fsx::exists(fsx::join(exe_dir, "portable.txt"))) return fsx::join(exe_dir, "data");
    return fsx::join(os::appdata_roaming(), "vsemvamdam-switcher");
#else
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.vsemvamdam-switcher";
#endif
}

bool Store::load(const std::string& d) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    dir = d;
    fsx::mkdirs(dir);
    std::string text;
    if (fsx::read_file(fsx::join(dir, "settings.json"), text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) {
            auto& s = settings;
            rd(j, "language", s.language);
            std::string t; rd(j, "theme", t); if (!t.empty()) s.theme = theme_p(t);
            rd(j, "close_to_tray", s.close_to_tray);
            rd(j, "start_minimized", s.start_minimized);
            rd(j, "run_at_startup", s.run_at_startup);
            std::string a; rd(j, "after_switch", a); if (!a.empty()) s.after_switch = after_p(a);
            rd(j, "streamer_mode", s.streamer_mode);
            rd(j, "grid_view", s.grid_view);
            rd(j, "card_scale", s.card_scale);
            rd(j, "confirm_forget", s.confirm_forget);
            rd(j, "tray_accounts", s.tray_accounts);
            if (auto w = j.find("window"); w != j.end() && w->is_object()) {
                rd(*w, "x", s.win_x); rd(*w, "y", s.win_y); rd(*w, "w", s.win_w); rd(*w, "h", s.win_h); rd(*w, "max", s.win_max);
            }
            rd(j, "last_platform", s.last_platform);
            rd(j, "platform_order", s.platform_order);
            if (auto st = j.find("steam"); st != j.end() && st->is_object()) {
                rd(*st, "path", s.steam_path);
                rd(*st, "args", s.steam_args);
                rd(*st, "default_state", s.steam_default_state);
                rd(*st, "fetch_profiles", s.steam_fetch_profiles);
                rd(*st, "show_vac", s.steam_show_vac);
                rd(*st, "show_limited", s.steam_show_limited);
                std::string c; rd(*st, "close", c); if (!c.empty()) s.steam_close = close_p(c);
                rd(*st, "launch", s.steam_launch);
            }
            if (auto g = j.find("guard"); g != j.end() && g->is_object()) {
                rd(*g, "poll", s.guard_poll);
                rd(*g, "poll_sec", s.guard_poll_sec);
                rd(*g, "auto_market", s.guard_auto_market);
                rd(*g, "auto_trades", s.guard_auto_trades);
                rd(*g, "notify", s.guard_notify);
                rd(*g, "codes_on_cards", s.guard_codes_on_cards);
                rd(*g, "last", s.guard_last);
                if (s.guard_poll_sec < 10) s.guard_poll_sec = 10;
                if (s.guard_poll_sec > 3600) s.guard_poll_sec = 3600;
            }
            if (auto ps = j.find("platforms"); ps != j.end() && ps->is_object()) {
                for (auto& [k, v] : ps->items()) {
                    if (!v.is_object()) continue;
                    PlatformPrefs p;
                    rd(v, "enabled", p.enabled);
                    rd(v, "exe_path", p.exe_path);
                    rd(v, "args", p.args);
                    rd(v, "launch_after", p.launch_after);
                    s.platforms[k] = p;
                }
            }
            if (s.card_scale < 0.7f || s.card_scale > 1.6f) s.card_scale = 1.0f;
        }
    }
    if (fsx::read_file(fsx::join(dir, "accounts.json"), text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) {
            for (auto& [plat, accs] : j.items()) {
                if (!accs.is_object()) continue;
                for (auto& [id, v] : accs.items()) {
                    AccountMeta m;
                    rd(v, "custom_name", m.custom_name);
                    rd(v, "note", m.note);
                    rd(v, "order", m.order);
                    rd(v, "default_state", m.default_state);
                    rd(v, "favorite_app", m.favorite_app);
                    rd(v, "last_switched", m.last_switched);
                    rd(v, "switch_count", m.switch_count);
                    rd(v, "avatar_url", m.avatar_url);
                    rd(v, "profile_checked", m.profile_checked);
                    rd(v, "vac", m.vac);
                    rd(v, "limited", m.limited);
                    rd(v, "trade_ban", m.trade_ban);
                    rd(v, "is_private", m.is_private);
                    rd(v, "community_name", m.community_name);
                    rd(v, "login", m.login);
                    rd(v, "display", m.display);
                    rd(v, "saved_at", m.saved_at);
                    rd(v, "forgotten_at", m.forgotten_at);
                    meta[plat][id] = m;
                }
            }
        }
    }
    return true;
}

bool Store::save_settings() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    auto& s = settings;
    json j;
    j["language"] = s.language;
    j["theme"] = theme_s(s.theme);
    j["close_to_tray"] = s.close_to_tray;
    j["start_minimized"] = s.start_minimized;
    j["run_at_startup"] = s.run_at_startup;
    j["after_switch"] = after_s(s.after_switch);
    j["streamer_mode"] = s.streamer_mode;
    j["grid_view"] = s.grid_view;
    j["card_scale"] = s.card_scale;
    j["confirm_forget"] = s.confirm_forget;
    j["tray_accounts"] = s.tray_accounts;
    j["window"] = {{"x", s.win_x}, {"y", s.win_y}, {"w", s.win_w}, {"h", s.win_h}, {"max", s.win_max}};
    j["last_platform"] = s.last_platform;
    j["platform_order"] = s.platform_order;
    j["steam"] = {{"path", s.steam_path},
                  {"args", s.steam_args},
                  {"default_state", s.steam_default_state},
                  {"fetch_profiles", s.steam_fetch_profiles},
                  {"show_vac", s.steam_show_vac},
                  {"show_limited", s.steam_show_limited},
                  {"close", close_s(s.steam_close)},
                  {"launch", s.steam_launch}};
    j["guard"] = {{"poll", s.guard_poll},
                  {"poll_sec", s.guard_poll_sec},
                  {"auto_market", s.guard_auto_market},
                  {"auto_trades", s.guard_auto_trades},
                  {"notify", s.guard_notify},
                  {"codes_on_cards", s.guard_codes_on_cards},
                  {"last", s.guard_last}};
    json ps = json::object();
    for (auto& [k, p] : s.platforms)
        ps[k] = {{"enabled", p.enabled}, {"exe_path", p.exe_path}, {"args", p.args}, {"launch_after", p.launch_after}};
    j["platforms"] = ps;
    bool ok = fsx::write_file(fsx::join(dir, "settings.json"), j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
    if (!ok) log::error("cannot write settings.json");
    return ok;
}

bool Store::save_meta() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    json j = json::object();
    for (auto& [plat, accs] : meta) {
        json a = json::object();
        for (auto& [id, m] : accs) {
            json v = json::object();
            if (!m.custom_name.empty()) v["custom_name"] = m.custom_name;
            if (!m.note.empty()) v["note"] = m.note;
            if (m.order >= 0) v["order"] = m.order;
            if (m.default_state != -2) v["default_state"] = m.default_state;
            if (!m.favorite_app.empty()) v["favorite_app"] = m.favorite_app;
            if (m.last_switched) v["last_switched"] = m.last_switched;
            if (m.switch_count) v["switch_count"] = m.switch_count;
            if (!m.avatar_url.empty()) v["avatar_url"] = m.avatar_url;
            if (m.profile_checked) v["profile_checked"] = m.profile_checked;
            if (m.vac) v["vac"] = true;
            if (m.limited) v["limited"] = true;
            if (m.trade_ban) v["trade_ban"] = true;
            if (m.is_private) v["is_private"] = true;
            if (!m.community_name.empty()) v["community_name"] = m.community_name;
            if (!m.login.empty()) v["login"] = m.login;
            if (!m.display.empty()) v["display"] = m.display;
            if (m.saved_at) v["saved_at"] = m.saved_at;
            if (m.forgotten_at) v["forgotten_at"] = m.forgotten_at;
            a[id] = v;
        }
        j[plat] = a;
    }
    bool ok = fsx::write_file(fsx::join(dir, "accounts.json"), j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace));
    if (!ok) log::error("cannot write accounts.json");
    return ok;
}

}  // namespace vs
