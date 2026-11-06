// vsemvamdam-switcher — Steam tools service (library, achievements, farm)
#include "steam_tools.h"

#include <algorithm>
#include <chrono>

#include "../core/crypto.h"
#include "../core/fsx.h"
#include "../core/i18n.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../core/str.h"
#include "../core/steam_core.h"
#include "guard.h"
#include "host.h"
#include "nlohmann/json.hpp"

using nlohmann::json;

namespace vs {

namespace {
const int kHelperTimeoutMs = 45000;
const int kCardsCheckSingle = 5 * 60;     // one game: check the badge every 5 minutes (as Idle Master)
const int kCardsCheckMulti = 10 * 60;     // several games under 2 h together: every 10 minutes
const int kMaxBadgePages = 30;

std::string quote_arg(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\\' || t.back() == '/')) t.pop_back();
    return "\"" + t + "\"";
}
}  // namespace

std::string SteamTools::helper_error_text(const std::string& code) {
    if (code == "no_steam") return TR("Steam не найден на этом компьютере", "Steam was not found on this PC");
    if (code == "not_running") return TR("Steam не запущен — запустите Steam и войдите в аккаунт", "Steam is not running — start it and sign in");
    if (code == "not_logged_in") return TR("В Steam не выполнен вход", "Nobody is signed in to Steam");
    if (code == "app_mismatch") return TR("Steam не принял игру (AppID)", "Steam rejected the game (AppID)");
    if (code == "no_exports" || code == "no_client" || code == "no_interfaces")
        return TR("Эта версия Steam не поддерживается — обновите Steam", "This Steam version is not supported — update Steam");
    if (code == "stats_timeout")
        return TR("Steam не отдал статистику — игра должна быть в библиотеке этого аккаунта", "Steam returned no stats — the game must be in this account's library");
    if (starts_with(code, "stats_result_")) return std::string(TR("Steam вернул ошибку статистики ", "Steam stats error ")) + code.substr(13);
    if (code == "no_schema") return TR("У игры нет достижений или статистики", "The game has no achievements or stats");
    if (code == "store_failed" || starts_with(code, "store_result_")) return TR("Steam не сохранил изменения", "Steam did not save the changes");
    if (code == "reset_failed") return TR("Steam не сбросил статистику", "Steam did not reset the stats");
    if (code == "timeout") return TR("Steam не ответил вовремя", "Steam timed out");
    if (code == "spawn") return TR("Не удалось запустить вспомогательный процесс", "Could not start the helper process");
    return std::string(TR("Ошибка: ", "Error: ")) + code;
}

SteamTools::SteamTools(Store& st, Guard& g, std::function<std::string()> root) : store_(st), guard_(g), root_fn_(std::move(root)) {
    fsx::mkdirs(dir());
    worker_ = std::thread([this] { worker_main(); });
}
SteamTools::~SteamTools() { stop(); }

std::string SteamTools::dir() const { return fsx::join(store_.dir, "tools"); }

void SteamTools::stop() {
    if (stop_.exchange(true)) return;
    farm_stop_ = true;
    fcv_.notify_all();
    qcv_.notify_all();
    if (farm_thread_.joinable()) farm_thread_.join();
    if (worker_.joinable()) worker_.join();
    kill_all_helpers();
}

void SteamTools::emit(ToolEvent e) {
    if (!on_event_) return;
    auto cb = on_event_;
    ui_post([cb, e] { cb(e); });
}

void SteamTools::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(qmtx_);
        queue_.push_back(std::move(job));
    }
    qcv_.notify_one();
}

void SteamTools::worker_main() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(qmtx_);
            qcv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            log::error(std::string("tools job: ") + e.what());
        }
    }
}

// ============================================================ helper processes
bool SteamTools::run_helper(const std::string& mode, uint32_t app, const std::string& in_text, int timeout_ms, std::string& out, std::string& err,
                            const std::atomic<bool>* cancel) {
    std::string run = fsx::join(dir(), "run");
    fsx::mkdirs(run);
    std::string tag = mode + "_" + std::to_string(app) + "_" + crypto::hex_encode(crypto::random_bytes(4));
    std::string fout = fsx::join(run, tag + ".out.json");
    std::string fin = fsx::join(run, tag + ".in");
    std::string args = "--steam-helper " + mode;
    if (app) args += " --app " + std::to_string(app);
    if (!in_text.empty()) {
        fsx::write_file(fin, in_text);
        args += " --in " + quote_arg(fin);
    }
    args += " --out " + quote_arg(fout);
    std::string root = root_fn_ ? root_fn_() : std::string();
    if (!root.empty()) args += " --root " + quote_arg(root);
    os::Child c;
    if (!os::spawn_child(os::self_exe_path(), args, c)) {
        err = "spawn";
        return false;
    }
    bool done = false;
    for (int waited = 0; waited < timeout_ms && !done; waited += 100) {
        done = os::child_wait(c, 100);
        if (!done && (stop_ || (cancel && *cancel))) break;
    }
    if (!done) {
        os::child_kill(c);
        err = "timeout";
    } else {
        os::child_kill(c);  // closes the handle
    }
    fsx::remove_file(fin);
    std::string text;
    bool got = fsx::read_file(fout, text);
    fsx::remove_file(fout);
    if (!done) return false;
    if (!got) { err = "no_output"; return false; }
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) { err = "bad_output"; return false; }
    if (!j.value("ok", false)) {
        err = j.value("error", std::string("error"));
        return false;
    }
    out = text;
    return true;
}

uint64_t SteamTools::whoami(std::string& err, const std::atomic<bool>* cancel) {
    std::string out;
    if (!run_helper("list", 0, "0\n", 20000, out, err, cancel)) return 0;
    json j = json::parse(out, nullptr, false);
    uint64_t id = 0;
    parse_u64(j.value("steamid", std::string()), id);
    return id;
}

bool SteamTools::start_idle(uint32_t app, std::string& err) {
    std::string run = fsx::join(dir(), "run");
    fsx::mkdirs(run);
    std::string fout = fsx::join(run, "idle_" + std::to_string(app) + ".json");
    fsx::remove_file(fout);
    std::string args = "--steam-helper idle --app " + std::to_string(app) + " --parent " + std::to_string(os::current_pid()) + " --out " + quote_arg(fout);
    std::string root = root_fn_ ? root_fn_() : std::string();
    if (!root.empty()) args += " --root " + quote_arg(root);
    os::Child c;
    if (!os::spawn_child(os::self_exe_path(), args, c)) { err = "spawn"; return false; }
    // wait for the helper to report that Steam accepted it
    for (int i = 0; i < 100; ++i) {
        std::string text;
        if (fsx::read_file(fout, text) && !text.empty()) {
            json j = json::parse(text, nullptr, false);
            if (j.is_object() && j.value("ok", false)) {
                procs_[app] = c;
                return true;
            }
            if (j.is_object()) err = j.value("error", std::string("error"));
            break;
        }
        uint32_t code = 0;
        if (os::child_wait(c, 100, &code)) { err = err.empty() ? "exit_" + std::to_string(code) : err; break; }
        if (farm_stop_ || stop_) { err = "cancelled"; break; }
    }
    if (err.empty()) err = "timeout";
    os::child_kill(c);
    return false;
}

void SteamTools::kill_all_helpers() {
    for (auto& [app, c] : procs_) os::child_kill(c);
    procs_.clear();
}

void SteamTools::reconcile(const std::vector<uint32_t>& want) {
    for (auto it = procs_.begin(); it != procs_.end();) {
        if (std::find(want.begin(), want.end(), it->first) == want.end()) {
            os::child_kill(it->second);
            it = procs_.erase(it);
        } else {
            ++it;
        }
    }
    std::string first_err;
    for (uint32_t app : want) {
        if (procs_.count(app) && os::child_running(procs_[app])) continue;
        if (procs_.count(app)) { os::child_kill(procs_[app]); procs_.erase(app); }
        std::string err;
        if (!start_idle(app, err) && first_err.empty()) first_err = err;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<FarmRunning> keep;
    for (uint32_t app : want) {
        FarmRunning r;
        r.appid = app;
        for (auto& old : fv_.running)
            if (old.appid == app) r = old;
        r.appid = app;
        if (!r.started) r.started = unix_now();
        r.ok = procs_.count(app) > 0;
        if (r.name.empty()) {
            for (auto& b : fv_.badges)
                if (b.appid == app) r.name = b.name;
            for (auto& g : lib_.games)
                if (r.name.empty() && g.appid == app) r.name = g.name;
            if (r.name.empty()) r.name = "AppID " + std::to_string(app);
        }
        keep.push_back(r);
    }
    fv_.running = keep;
    fv_.error = first_err.empty() ? std::string() : helper_error_text(first_err);
}

// ============================================================ library
std::vector<uint32_t> SteamTools::local_candidates(std::set<uint32_t>& with_schema) {
    std::set<uint32_t> ids;
    std::string root = root_fn_ ? root_fn_() : std::string();
    auto add_num = [&](const std::string& s, std::set<uint32_t>& to) {
        uint64_t v = 0;
        if (parse_u64(s, v) && v && v < 0xFFFFFFFFu) to.insert((uint32_t)v);
    };
    if (!root.empty()) {
        for (auto& d : fsx::list_dir(fsx::join(root, "appcache\\librarycache"))) {
            std::string n = d;
            size_t us = n.find_first_not_of("0123456789");
            add_num(n.substr(0, us), ids);
        }
        for (auto& f : fsx::list_dir(fsx::join(root, "appcache\\stats"))) {
            if (!starts_with(f, "UserGameStatsSchema_")) continue;
            std::string n = f.substr(20);
            add_num(n.substr(0, n.find('.')), with_schema);
        }
        // installed games in every library
        std::string lf;
        std::vector<std::string> libs = {root};
        if (fsx::read_file(fsx::join(root, "steamapps\\libraryfolders.vdf"), lf))
            for (auto& p : steam::library_paths(lf)) libs.push_back(p);
        for (auto& lib : libs)
            for (auto& f : fsx::list_dir(fsx::join(lib, "steamapps")))
                if (starts_with(f, "appmanifest_")) add_num(f.substr(12, f.find('.') - 12), ids);
    }
    // Steam Achievement Manager's list of apps with stats (cached for a week)
    std::string cache = fsx::join(dir(), "sam_games.xml");
    std::string xml;
    if (!fsx::read_file(cache, xml) || unix_now() - fsx::mtime_unix(cache) > 7 * 86400) {
        std::string fresh;
        if (os::http_get("https://gib.me/sam/games.xml", fresh, nullptr, 15000) && fresh.size() > 100) {
            xml = fresh;
            fsx::write_file(cache, xml);
        }
    }
    for (uint32_t id : cards::parse_sam_games_xml(xml)) {
        ids.insert(id);
        with_schema.insert(id);
    }
    for (uint32_t id : with_schema) ids.insert(id);
    return std::vector<uint32_t>(ids.begin(), ids.end());
}

void SteamTools::load_library(bool force) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (lib_.loading || (lib_.loaded && !force)) return;
        lib_.loading = true;
        lib_.error.clear();
    }
    emit({});
    post([this] {
        LibraryView v;
        std::string err;
        uint64_t sid = whoami(err);
        if (!sid) {
            v.error = helper_error_text(err);
        } else {
            v.steamid = sid;
            std::set<uint32_t> with_schema;
            std::vector<cards::OwnedGame> owned;
            std::string body;
            int status = 0;
            if (guard_.has_web_session(sid) &&
                guard_.webapi_get(sid, "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?steamid=" + std::to_string(sid) +
                                           "&include_appinfo=1&include_played_free_games=1&skip_unvetted_apps=0",
                                  body, status) &&
                cards::parse_owned_games(body, owned) && !owned.empty()) {
                v.from_web = true;
                for (auto& o : owned) v.games.push_back({o.appid, o.name, o.minutes, o.has_stats, o.icon});
            } else {
                auto cands = local_candidates(with_schema);
                std::string in;
                for (uint32_t id : cands) in += std::to_string(id) + "\n";
                std::string out;
                if (!run_helper("list", 0, in, 120000, out, err)) {
                    v.error = helper_error_text(err);
                } else {
                    json j = json::parse(out, nullptr, false);
                    for (auto& g : j.value("games", json::array())) {
                        ToolGame t;
                        t.appid = g.value("appid", 0u);
                        t.name = g.value("name", std::string());
                        t.has_stats = with_schema.count(t.appid) > 0;
                        if (t.appid) v.games.push_back(t);
                    }
                }
            }
            std::sort(v.games.begin(), v.games.end(), [](const ToolGame& a, const ToolGame& b) {
                return fold_case_utf8(a.name.empty() ? std::to_string(a.appid) : a.name) < fold_case_utf8(b.name.empty() ? std::to_string(b.appid) : b.name);
            });
        }
        v.loaded = v.error.empty();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            lib_ = v;
        }
        emit({});
        // small icons for the list, in the background
        if (v.from_web) {
            std::string idir = fsx::join(dir(), "icons");
            fsx::mkdirs(idir);
            int n = 0;
            for (auto& g : v.games) {
                if (stop_ || g.icon.empty() || ++n > 400) continue;
                std::string f = fsx::join(idir, std::to_string(g.appid) + ".jpg");
                if (fsx::exists(f)) continue;
                std::string img;
                if (os::http_get("https://cdn.cloudflare.steamstatic.com/steamcommunity/public/images/apps/" + std::to_string(g.appid) + "/" + g.icon + ".jpg",
                                 img, nullptr, 6000) &&
                    img.size() > 64)
                    fsx::write_file(f, img);
                if (n % 25 == 0) emit({});
            }
            emit({});
        }
    });
}

LibraryView SteamTools::library() {
    std::lock_guard<std::mutex> lk(mtx_);
    return lib_;
}

std::string SteamTools::game_name(uint32_t appid) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& g : lib_.games)
        if (g.appid == appid) return g.name;
    for (auto& b : fv_.badges)
        if (b.appid == appid) return b.name;
    return {};
}

std::string SteamTools::icon_for(uint32_t appid) {
    std::string f = fsx::join(fsx::join(dir(), "icons"), std::to_string(appid) + ".jpg");
    if (fsx::exists(f)) return f;
    std::string root = root_fn_ ? root_fn_() : std::string();
    if (root.empty()) return {};
    std::string lc = fsx::join(root, "appcache\\librarycache\\" + std::to_string(appid));
    std::string best;
    uint64_t best_size = UINT64_MAX;
    for (auto& n : fsx::list_dir(lc)) {
        std::string l = to_lower(n);
        if (!ends_with(l, ".jpg") && !ends_with(l, ".png")) continue;
        std::string p = fsx::join(lc, n);
        uint64_t sz = fsx::file_size(p);
        if (sz < best_size) { best_size = sz; best = p; }
    }
    if (!best.empty()) return best;
    std::string old = fsx::join(root, "appcache\\librarycache\\" + std::to_string(appid) + "_icon.jpg");
    return fsx::exists(old) ? old : std::string();
}

std::string SteamTools::achievement_icon(uint32_t appid, const std::string& file) {
    if (file.empty()) return {};
    std::string f = fsx::join(fsx::join(dir(), "ach\\" + std::to_string(appid)), sanitize_filename(file));
    return fsx::exists(f) ? f : std::string();
}

void SteamTools::fetch_icons_for(const stats::GameStats& g) {
    std::string d = fsx::join(dir(), "ach\\" + std::to_string(g.appid));
    fsx::mkdirs(d);
    int n = 0;
    for (auto& a : g.achievements) {
        for (const std::string* file : {&a.def.icon, &a.def.icon_gray}) {
            if (stop_ || file->empty() || ++n > 800) continue;
            std::string f = fsx::join(d, sanitize_filename(*file));
            if (fsx::exists(f)) continue;
            std::string img;
            if (os::http_get("https://cdn.cloudflare.steamstatic.com/steamcommunity/public/images/apps/" + std::to_string(g.appid) + "/" + *file, img,
                             nullptr, 6000) &&
                img.size() > 64)
                fsx::write_file(f, img);
            if (n % 20 == 0) emit({});
        }
    }
    emit({});
}

// ============================================================ achievements
void SteamTools::load_stats(uint32_t appid) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (sv_.loading && sv_.appid == appid) return;
        bool same = sv_.appid == appid;
        sv_.loading = true;
        sv_.appid = appid;
        sv_.error.clear();
        if (!same) {
            sv_.data = stats::GameStats{};
            sv_.result.clear();
        }
    }
    emit({});
    post([this, appid] {
        std::string out, err;
        stats::GameStats g;
        bool ok = run_helper("stats", appid, "", kHelperTimeoutMs, out, err) && stats::from_json(out, g);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (sv_.appid != appid) return;
            sv_.loading = false;
            if (ok) {
                sv_.data = g;
                sv_.loaded_at = unix_now();
            } else {
                sv_.error = helper_error_text(err.empty() ? "bad_output" : err);
            }
        }
        emit({});
        if (ok) fetch_icons_for(g);
    });
}

void SteamTools::apply(uint32_t appid, const stats::Changes& c) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (sv_.applying) return;
        sv_.applying = true;
        sv_.result.clear();
    }
    emit({});
    std::string in = stats::to_json(c);
    post([this, appid, in] {
        std::string out, err;
        bool ok = run_helper("apply", appid, in, kHelperTimeoutMs, out, err);
        int applied = 0, skipped = 0;
        if (ok) {
            json j = json::parse(out, nullptr, false);
            applied = j.value("applied", 0);
            skipped = j.value("skipped", 0);
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            sv_.applying = false;
            sv_.result_error = !ok;
            if (ok) {
                sv_.result = std::string(TR("Сохранено в Steam: ", "Saved to Steam: ")) + std::to_string(applied);
                if (skipped) sv_.result += std::string(TR(", пропущено защищённых: ", ", protected skipped: ")) + std::to_string(skipped);
            } else {
                sv_.result = helper_error_text(err);
            }
        }
        log::info("tools: apply app " + std::to_string(appid) + (ok ? " ok" : " failed: " + err));
        emit({});
        if (ok) load_stats(appid);
    });
}

StatsView SteamTools::stats_view() {
    std::lock_guard<std::mutex> lk(mtx_);
    return sv_;
}

// ============================================================ farm
FarmView SteamTools::farm_view() {
    std::lock_guard<std::mutex> lk(mtx_);
    return fv_;
}
bool SteamTools::farming() {
    std::lock_guard<std::mutex> lk(mtx_);
    return fv_.active;
}

void SteamTools::set_farm_status(const std::string& status, const std::string& error) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fv_.status = status;
        fv_.error = error;
    }
    emit({});
}

void SteamTools::start_hours(const std::vector<uint32_t>& apps_in, int minutes_limit) {
    std::vector<uint32_t> apps;
    for (uint32_t a : apps_in)
        if (a && std::find(apps.begin(), apps.end(), a) == apps.end() && (int)apps.size() < cards::kMaxParallel) apps.push_back(a);
    if (apps.empty()) return;
    stop_farm();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fv_ = FarmView{};
        fv_.kind = FarmKind::Hours;
        fv_.active = true;
        fv_.started = unix_now();
        fv_.stop_at = minutes_limit > 0 ? fv_.started + minutes_limit * 60 : 0;
        fv_.status = TR("Запускаю игры…", "Starting the games…");
    }
    emit({});
    farm_stop_ = false;
    farm_thread_ = std::thread([this, apps, minutes_limit] { farm_main(FarmKind::Hours, apps, minutes_limit, cards::Mode::Simultaneous); });
}

void SteamTools::start_cards(cards::Mode mode) {
    stop_farm();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fv_ = FarmView{};
        fv_.kind = FarmKind::Cards;
        fv_.mode = mode;
        fv_.active = true;
        fv_.started = unix_now();
        fv_.status = TR("Смотрю, где остались карточки…", "Looking for remaining card drops…");
    }
    emit({});
    farm_stop_ = false;
    farm_thread_ = std::thread([this, mode] { farm_main(FarmKind::Cards, {}, 0, mode); });
}

void SteamTools::stop_farm(const std::string& why) {
    farm_stop_ = true;
    fcv_.notify_all();
    if (farm_thread_.joinable()) farm_thread_.join();
    kill_all_helpers();
    bool was;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        was = fv_.active;
        fv_.active = false;
        fv_.running.clear();
        if (was) fv_.status = why.empty() ? std::string(TR("Остановлено", "Stopped")) : why;
    }
    if (was) {
        log::info("tools: farm stopped" + (why.empty() ? std::string() : ": " + why));
        emit({});
    }
}

void SteamTools::check_now() {
    check_now_ = true;
    fcv_.notify_all();
}

bool SteamTools::sleep_farm(int seconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!farm_stop_ && !stop_) {
        if (check_now_.exchange(false)) return true;
        // keep the helpers alive: one that died (Steam restarted, crash) is restarted a few times
        std::vector<uint32_t> dead;
        for (auto& [app, c] : procs_)
            if (!os::child_running(c)) dead.push_back(app);
        if (!dead.empty()) {
            std::vector<uint32_t> want;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& r : fv_.running) want.push_back(r.appid);
            }
            for (uint32_t a : dead) {
                if (++restarts_[a] > 5) {
                    want.erase(std::remove(want.begin(), want.end(), a), want.end());
                    log::warn("tools: helper for " + std::to_string(a) + " keeps dying");
                }
            }
            reconcile(want);
            emit({});
        }
        int64_t stop_at;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_at = fv_.stop_at;
        }
        if (stop_at && unix_now() >= stop_at) return false;
        if (std::chrono::steady_clock::now() >= deadline) return true;
        std::unique_lock<std::mutex> lk(fmtx_);
        fcv_.wait_for(lk, std::chrono::seconds(1), [this] { return farm_stop_.load() || stop_.load() || check_now_.load(); });
    }
    return false;
}

void SteamTools::farm_main(FarmKind kind, std::vector<uint32_t> apps, int minutes_limit, cards::Mode mode) {
    (void)minutes_limit;
    restarts_.clear();
    std::string err;
    uint64_t sid = whoami(err, &farm_stop_);
    if (farm_stop_) return;
    if (!sid) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fv_.active = false;
            fv_.error = helper_error_text(err);
            fv_.status.clear();
        }
        emit({ToolEventKind::FarmError, helper_error_text(err)});
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fv_.steamid = sid;
    }
    if (kind == FarmKind::Hours) {
        reconcile(apps);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fv_.status = TR("Набираю часы", "Gaining hours");
        }
        emit({});
        log::info("tools: hours farm for " + std::to_string(apps.size()) + " game(s)");
        if (!sleep_farm(365 * 86400)) {
            bool timed;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                timed = fv_.stop_at && unix_now() >= fv_.stop_at;
            }
            if (timed) {
                kill_all_helpers();
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    fv_.active = false;
                    fv_.running.clear();
                    fv_.status = TR("Готово — время вышло", "Done — time is up");
                }
                emit({ToolEventKind::FarmFinished, TR("Фарм часов завершён", "Hours farm finished")});
            }
        }
        return;
    }
    // ---- cards
    if (!guard_.has_web_session(sid)) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fv_.active = false;
            fv_.need_login = true;
            fv_.status.clear();
            fv_.error = TR("Чтобы видеть, сколько карточек осталось, нужен вход в аккаунт", "Sign in to see how many card drops are left");
        }
        emit({});
        return;
    }
    std::map<uint32_t, int> prev_drops;
    while (!farm_stop_ && !stop_) {
        std::vector<cards::BadgeGame> all;
        bool ok = true, signed_in = true;
        std::string body;
        int status = 0;
        for (int page = 1, last = 1; page <= last && page <= kMaxBadgePages && !farm_stop_; ++page) {
            if (!guard_.community_get(sid, "/profiles/" + std::to_string(sid) + "/badges/?l=english&p=" + std::to_string(page), body, status)) {
                ok = false;
                break;
            }
            cards::BadgePage bp;
            cards::parse_badges(body, bp);
            if (!bp.signed_in) { signed_in = false; break; }
            last = bp.last_page;
            for (auto& g : bp.games)
                if (g.drops > 0) all.push_back(g);
        }
        if (farm_stop_) break;
        if (!signed_in) {
            kill_all_helpers();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                fv_.active = false;
                fv_.need_login = true;
                fv_.running.clear();
                fv_.error = TR("Steam завершил сессию — войдите снова", "Steam ended the session — sign in again");
            }
            emit({ToolEventKind::FarmError, TR("Фарм карточек остановлен: нужен вход", "Card farm stopped: sign-in needed")});
            return;
        }
        if (!ok) {
            set_farm_status(TR("Нет связи со Steam — повторю через минуту", "Can't reach Steam — retrying in a minute"));
            if (!sleep_farm(60)) break;
            continue;
        }
        int left = 0, dropped_now = 0;
        std::string dropped_name;
        for (auto& g : all) {
            left += g.drops;
            auto p = prev_drops.find(g.appid);
            if (p != prev_drops.end() && g.drops < p->second) {
                dropped_now += p->second - g.drops;
                dropped_name = g.name;
            }
        }
        for (auto& [app, d] : prev_drops)
            if (std::none_of(all.begin(), all.end(), [&](const cards::BadgeGame& g) { return g.appid == app; })) {
                dropped_now += d;  // the game ran out of drops
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& r : fv_.running)
                    if (r.appid == app) dropped_name = r.name;
            }
        prev_drops.clear();
        for (auto& g : all) prev_drops[g.appid] = g.drops;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fv_.badges = all;
            fv_.drops_left = left;
            fv_.dropped += dropped_now;
        }
        if (dropped_now > 0) emit({ToolEventKind::CardDropped, (dropped_name.empty() ? std::string() : dropped_name + ": ") +
                                                                   TR("выпало карточек: ", "cards dropped: ") + std::to_string(dropped_now)});
        if (all.empty()) {
            kill_all_helpers();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                fv_.active = false;
                fv_.running.clear();
                fv_.status = TR("Все карточки получены", "All card drops received");
            }
            emit({ToolEventKind::FarmFinished, TR("Все карточки получены", "All card drops received")});
            log::info("tools: card farm finished");
            return;
        }
        auto want = cards::plan(all, mode);
        reconcile(want);
        int wait = want.size() > 1 ? kCardsCheckMulti : kCardsCheckSingle;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fv_.next_check = unix_now() + wait;
            fv_.status = want.size() > 1 ? TR("Набираю 2 часа в играх, где карточки ещё не падают", "Reaching 2 hours in games that don't drop yet")
                                         : TR("Фарм карточек", "Farming cards");
        }
        emit({});
        if (!sleep_farm(wait)) break;
    }
}

}  // namespace vs
