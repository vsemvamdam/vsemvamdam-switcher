// vsemvamdam-switcher — "--steam-helper" child process: talks to the Steam client as a given game.
// Every Steam interaction that needs SteamAppId runs here, in its own short-lived process, so the main
// window never loads steamclient and a crash or a Steam restart can't take the switcher down.
//
//   --steam-helper list  --in <candidates.txt> --out <result.json> [--root <steam>]
//   --steam-helper stats --app <id> --out <result.json> [--root <steam>]
//   --steam-helper apply --app <id> --in <changes.json> --out <result.json> [--root <steam>]
//   --steam-helper idle  --app <id> --out <status.json> [--parent <pid>] [--root <steam>]
#include "steam_helper.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>

#include "../core/fsx.h"
#include "../core/kv_binary.h"
#include "../core/steam_stats.h"
#include "../core/str.h"
#include "../os/steamclient.h"
#include "nlohmann/json.hpp"

using nlohmann::json;

namespace vs {

namespace {
struct Opts {
    std::string mode, out, in, root;
    uint32_t app = 0;
    uint32_t parent = 0;
    uint64_t steamid = 0;  // refuse to work if Steam is signed in to another account
};

void write_json(const Opts& o, const json& j) {
    if (!o.out.empty()) fsx::write_file(o.out, j.dump(-1, ' ', false, json::error_handler_t::replace));
}
int fail(const Opts& o, const std::string& code, int rc = 2) {
    write_json(o, {{"ok", false}, {"error", code}});
    return rc;
}

// UserStatsReceived_t: uint64 game id, EResult at +8
bool wait_stats(steamc::Client& c, uint32_t app, int& result) {
    uint64_t sid = c.steam_id();
    if (!c.request_user_stats(sid)) return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<uint8_t> p;
        if (!c.wait_callback(steamc::kUserStatsReceived, 1000, &p)) continue;
        if (p.size() < 12) continue;
        uint64_t game = 0;
        int32_t r = 0;
        std::memcpy(&game, p.data(), 8);
        std::memcpy(&r, p.data() + 8, 4);
        if ((uint32_t)(game & 0xFFFFFF) != app) continue;
        result = r;
        return true;
    }
    return false;
}

bool load_schema(const std::string& root, uint32_t app, const std::string& lang, stats::Schema& sc) {
    std::string data;
    std::string path = fsx::join(root, "appcache\\stats\\UserGameStatsSchema_" + std::to_string(app) + ".bin");
    kv::Node node;
    return fsx::read_file(path, data) && kv::parse_binary(data, node) && stats::parse_schema(node, app, lang, sc);
}

int run_list(const Opts& o, steamc::Client& c) {
    std::string text;
    fsx::read_file(o.in, text);
    std::set<uint32_t> seen;
    json games = json::array();
    for (auto& line : split(text, '\n')) {
        uint64_t id = 0;
        if (!parse_u64(trim(line), id) || !id || id > 0xFFFFFFFFu || seen.count((uint32_t)id)) continue;
        seen.insert((uint32_t)id);
        if (!c.is_subscribed_app((uint32_t)id)) continue;
        games.push_back({{"appid", (uint32_t)id}, {"name", c.app_data((uint32_t)id, "name")}});
    }
    write_json(o, {{"ok", true}, {"steamid", std::to_string(c.steam_id())}, {"games", games}});
    return 0;
}

int run_stats(const Opts& o, steamc::Client& c, const std::string& root) {
    int r = 0;
    if (!wait_stats(c, o.app, r)) return fail(o, "stats_timeout");
    if (r != 1) return fail(o, "stats_result_" + std::to_string(r));
    std::string lang = c.current_game_language();
    if (lang.empty()) lang = "english";
    stats::Schema sc;
    if (!load_schema(root, o.app, lang, sc)) return fail(o, "no_schema");
    stats::GameStats g;
    g.appid = o.app;
    g.steamid = c.steam_id();
    g.name = c.app_data(o.app, "name");
    for (auto& d : sc.achievements) {
        stats::Achievement a;
        a.def = d;
        if (!c.get_achievement(d.id, a.achieved, a.unlock_time)) continue;
        g.achievements.push_back(a);
    }
    for (auto& d : sc.stats) {
        stats::Stat s;
        s.def = d;
        if (d.type == stats::StatInt) {
            int32_t v;
            if (!c.get_stat(d.id, v)) continue;
            s.value = v;
        } else {
            float v;
            if (!c.get_stat(d.id, v)) continue;
            s.value = v;
        }
        g.stats.push_back(s);
    }
    json j = json::parse(stats::to_json(g));
    j["ok"] = true;
    write_json(o, j);
    return 0;
}

int run_apply(const Opts& o, steamc::Client& c, const std::string& root) {
    std::string text;
    stats::Changes ch;
    if (!fsx::read_file(o.in, text) || !stats::from_json(text, ch)) return fail(o, "bad_input");
    int r = 0;
    if (!wait_stats(c, o.app, r)) return fail(o, "stats_timeout");
    if (r != 1) return fail(o, "stats_result_" + std::to_string(r));
    stats::Schema sc;
    load_schema(root, o.app, "english", sc);  // only for types and protection flags
    int applied = 0, skipped = 0;
    if (ch.reset_all) {
        if (!c.reset_all_stats(ch.reset_with_achievements)) return fail(o, "reset_failed");
        ++applied;
    }
    for (auto& [id, on] : ch.achievements) {
        auto it = std::find_if(sc.achievements.begin(), sc.achievements.end(), [&](const stats::AchievementDef& d) { return d.id == id; });
        if (it != sc.achievements.end() && stats::is_protected(it->permission)) { ++skipped; continue; }
        if (c.set_achievement(id, on)) ++applied;
        else ++skipped;
    }
    for (auto& [id, val] : ch.stats) {
        auto it = std::find_if(sc.stats.begin(), sc.stats.end(), [&](const stats::StatDef& d) { return d.id == id; });
        if (it == sc.stats.end() || stats::is_protected(it->permission)) { ++skipped; continue; }
        bool ok = it->type == stats::StatInt ? c.set_stat(id, (int32_t)val) : c.set_stat(id, (float)val);
        if (ok) ++applied;
        else ++skipped;
    }
    if (!c.store_stats()) return fail(o, "store_failed");
    int stored = -1;
    std::vector<uint8_t> p;
    if (c.wait_callback(steamc::kUserStatsStored, 8000, &p) && p.size() >= 12) std::memcpy(&stored, p.data() + 8, 4);
    write_json(o, {{"ok", stored == -1 || stored == 1}, {"error", stored == -1 || stored == 1 ? "" : "store_result_" + std::to_string(stored)},
                   {"applied", applied}, {"skipped", skipped}});
    return 0;
}

int run_idle(const Opts& o, steamc::Client& c) {
    write_json(o, {{"ok", true}, {"steamid", std::to_string(c.steam_id())}, {"appid", o.app}});
    HANDLE parent = o.parent ? OpenProcess(SYNCHRONIZE, FALSE, o.parent) : nullptr;
    int offline = 0;
    for (int tick = 0;; ++tick) {
        if (parent) {
            if (WaitForSingleObject(parent, 1000) != WAIT_TIMEOUT) break;  // the switcher is gone
        } else {
            Sleep(1000);
        }
        c.drain_callbacks();
        if (tick % 10 == 9) {
            offline = c.logged_on() ? 0 : offline + 1;
            if (offline >= 3) return 3;  // Steam closed or signed out: the switcher restarts us when it is back
            if (o.steamid && c.steam_id() != o.steamid) return 4;  // another account now: the switcher stops the farm
        }
    }
    if (parent) CloseHandle(parent);
    return 0;
}
}  // namespace

bool steam_helper_main(const std::vector<std::string>& args, int& rc) {
    if (args.empty() || args[0] != "--steam-helper") return false;
    Opts o;
    if (args.size() > 1) o.mode = args[1];
    for (size_t i = 2; i + 1 < args.size(); i += 2) {
        const std::string& k = args[i];
        const std::string& v = args[i + 1];
        uint64_t n = 0;
        if (k == "--out") o.out = v;
        else if (k == "--in") o.in = v;
        else if (k == "--root") o.root = v;
        else if (k == "--app" && parse_u64(v, n)) o.app = (uint32_t)n;
        else if (k == "--parent" && parse_u64(v, n)) o.parent = (uint32_t)n;
        else if (k == "--steamid" && parse_u64(v, n)) o.steamid = n;
    }
    bool needs_app = o.mode == "stats" || o.mode == "apply" || o.mode == "idle";
    if ((needs_app && !o.app) || (o.mode != "list" && !needs_app)) {
        rc = fail(o, "bad_args", 1);
        return true;
    }
    std::string root = o.root.empty() ? steamc::Client::default_steam_root() : o.root;
    steamc::Client c;
    steamc::InitError e = c.init(root, needs_app ? o.app : 0);
    if (e != steamc::InitError::None) {
        rc = fail(o, steamc::init_error_code(e));
        return true;
    }
    if (o.steamid && c.steam_id() != o.steamid) {
        c.shutdown();
        rc = fail(o, "wrong_account");
        ExitProcess((UINT)rc);
    }
    if (o.mode == "list") rc = run_list(o, c);
    else if (o.mode == "stats") rc = run_stats(o, c, root);
    else if (o.mode == "apply") rc = run_apply(o, c, root);
    else rc = run_idle(o, c);
    c.shutdown();
    // steamclient keeps threads; leave without running global destructors in a half-torn-down module
    ExitProcess((UINT)rc);
    return true;
}

}  // namespace vs
