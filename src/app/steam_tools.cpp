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
const int kRetrySeconds = 30;             // a helper that could not start (Steam closed, updating) is retried this often
const char* kCdn = "https://cdn.cloudflare.steamstatic.com/steamcommunity/public/images/apps/";

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
    if (code == "wrong_account") return TR("В Steam сейчас другой аккаунт", "Another account is signed in to Steam");
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
    icon_thread_ = std::thread([this] { icon_main(); });
}
SteamTools::~SteamTools() { stop(); }

std::string SteamTools::dir() const { return fsx::join(store_.dir, "tools"); }

void SteamTools::stop() {
    if (stop_.exchange(true)) return;
    std::shared_ptr<FarmRun> r;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        r = run_;
        run_.reset();
        fv_.active = false;
    }
    if (r) {
        r->stop = true;
        r->cv.notify_all();
        retired_.push_back(r);
    }
    qcv_.notify_all();
    icv_.notify_all();
    reap_retired(true);  // each run kills its own helpers on the way out
    if (worker_.joinable()) worker_.join();
    if (icon_thread_.joinable()) icon_thread_.join();
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

// ============================================================ icons
void SteamTools::queue_icon(IconJob j, bool urgent) {
    {
        std::lock_guard<std::mutex> lk(imtx_);
        if (!icon_queued_.insert(j.file).second) return;
        if (urgent) icon_q_.push_front(std::move(j));
        else icon_q_.push_back(std::move(j));
    }
    icv_.notify_one();
}

void SteamTools::icon_main() {
    auto last_emit = std::chrono::steady_clock::now();
    bool dirty = false;
    for (;;) {
        IconJob j;
        {
            std::unique_lock<std::mutex> lk(imtx_);
            if (icon_q_.empty() && dirty) {
                lk.unlock();
                emit({});
                dirty = false;
                lk.lock();
            }
            icv_.wait(lk, [this] { return stop_ || !icon_q_.empty(); });
            if (stop_) return;
            j = std::move(icon_q_.front());
            icon_q_.pop_front();
        }
        bool have = fsx::exists(j.file);
        if (!have) {
            std::string img;
            if (os::http_get(j.url, img, nullptr, 6000) && img.size() > 64) have = fsx::write_file(j.file, img);
        }
        {
            std::lock_guard<std::mutex> lk(imtx_);
            icon_queued_.erase(j.file);
        }
        if (have) {
            std::lock_guard<std::mutex> lk(cmtx_);
            if (j.game) game_icons_[j.appid] = j.file;
            else ach_files_[j.appid].insert(fsx::filename(j.file));
            dirty = true;
        }
        if (dirty && std::chrono::steady_clock::now() - last_emit > std::chrono::milliseconds(400)) {
            emit({});
            dirty = false;
            last_emit = std::chrono::steady_clock::now();
        }
    }
}

std::string SteamTools::icon_for(uint32_t appid) {
    {
        std::lock_guard<std::mutex> lk(cmtx_);
        auto it = game_icons_.find(appid);
        if (it != game_icons_.end()) return it->second;
    }
    // resolved once per game: our download, then Steam's own library cache
    std::string found;
    std::string f = fsx::join(fsx::join(dir(), "icons"), std::to_string(appid) + ".jpg");
    if (fsx::exists(f)) found = f;
    std::string root = found.empty() && root_fn_ ? root_fn_() : std::string();
    if (!root.empty()) {
        std::string lc = fsx::join(root, "appcache\\librarycache\\" + std::to_string(appid));
        uint64_t best_size = UINT64_MAX;
        for (auto& n : fsx::list_dir(lc)) {
            std::string l = to_lower(n);
            if (!ends_with(l, ".jpg") && !ends_with(l, ".png")) continue;
            std::string p = fsx::join(lc, n);
            uint64_t sz = fsx::file_size(p);
            if (sz < best_size) {
                best_size = sz;
                found = p;
            }
        }
        if (found.empty()) {
            std::string old = fsx::join(root, "appcache\\librarycache\\" + std::to_string(appid) + "_icon.jpg");
            if (fsx::exists(old)) found = old;
        }
    }
    std::lock_guard<std::mutex> lk(cmtx_);
    game_icons_[appid] = found;
    return found;
}

std::string SteamTools::achievement_icon(uint32_t appid, const std::string& file) {
    if (file.empty()) return {};
    std::string name = sanitize_filename(file);
    std::lock_guard<std::mutex> lk(cmtx_);
    auto it = ach_files_.find(appid);
    if (it == ach_files_.end()) {
        auto& set = ach_files_[appid];
        for (auto& n : fsx::list_dir(fsx::join(dir(), "ach\\" + std::to_string(appid)))) set.insert(n);
        it = ach_files_.find(appid);
    }
    if (!it->second.count(name)) return {};
    return fsx::join(fsx::join(dir(), "ach\\" + std::to_string(appid)), name);
}

void SteamTools::fetch_icons_for(const stats::GameStats& g) {
    std::string d = fsx::join(dir(), "ach\\" + std::to_string(g.appid));
    fsx::mkdirs(d);
    int n = 0;
    for (auto& a : g.achievements)
        for (const std::string* file : {&a.def.icon, &a.def.icon_gray}) {
            if (file->empty() || ++n > 800) continue;
            std::string f = fsx::join(d, sanitize_filename(*file));
            if (fsx::exists(f)) continue;
            queue_icon({kCdn + std::to_string(g.appid) + "/" + *file, f, g.appid, false}, true);
        }
}

// ============================================================ helper processes
bool SteamTools::run_helper(const std::string& mode, uint32_t app, const std::string& in_text, int timeout_ms, std::string& out, std::string& err,
                            const std::atomic<bool>* cancel, uint64_t expect_steamid) {
    std::string run = fsx::join(dir(), "run");
    fsx::mkdirs(run);
    std::string tag = mode + "_" + std::to_string(app) + "_" + crypto::hex_encode(crypto::random_bytes(4));
    std::string fout = fsx::join(run, tag + ".out.json");
    std::string fin = fsx::join(run, tag + ".in");
    std::string args = "--steam-helper " + mode;
    if (app) args += " --app " + std::to_string(app);
    if (expect_steamid) args += " --steamid " + std::to_string(expect_steamid);
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
        fsx::remove_file(fin);
        return false;
    }
    bool done = false;
    for (int waited = 0; waited < timeout_ms && !done; waited += 100) {
        done = os::child_wait(c, 100);
        if (!done && (stop_ || (cancel && *cancel))) break;
    }
    os::child_kill(c);  // closes the handle (and ends a helper that hung)
    if (!done) err = "timeout";
    fsx::remove_file(fin);
    std::string text;
    bool got = fsx::read_file(fout, text);
    fsx::remove_file(fout);
    if (!done) return false;
    if (!got) {
        err = "no_output";
        return false;
    }
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        err = "bad_output";
        return false;
    }
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
            size_t us = d.find_first_not_of("0123456789");
            add_num(d.substr(0, us), ids);
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
        if (lib_.loading) {
            if (force) lib_reload_ = true;  // e.g. the account changed while loading: load again right after
            return;
        }
        if (lib_.loaded && !force) return;
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
        bool again;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            lib_ = v;
            again = lib_reload_;
            lib_reload_ = false;
        }
        {
            std::lock_guard<std::mutex> lk(cmtx_);
            for (auto it = game_icons_.begin(); it != game_icons_.end();) it = it->second.empty() ? game_icons_.erase(it) : std::next(it);
        }
        emit({});
        if (again) {
            load_library(true);
            return;
        }
        // small icons for the list, in the background
        if (v.from_web) {
            std::string idir = fsx::join(dir(), "icons");
            fsx::mkdirs(idir);
            int n = 0;
            for (auto& g : v.games) {
                if (g.icon.empty() || ++n > 1500) continue;
                std::string f = fsx::join(idir, std::to_string(g.appid) + ".jpg");
                if (!fsx::exists(f)) queue_icon({kCdn + std::to_string(g.appid) + "/" + g.icon + ".jpg", f, g.appid, true}, false);
            }
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
    if (sv_.data.appid == appid) return sv_.data.name;
    return {};
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
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (sv_.appid != appid) return;  // another game was picked meanwhile
        }
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
    uint64_t expect = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (sv_.applying || sv_.appid != appid || sv_.data.appid != appid) return;  // only onto the game on screen
        sv_.applying = true;
        sv_.result.clear();
        expect = sv_.data.steamid;  // ...and only for the account whose values were shown
    }
    emit({});
    std::string in = stats::to_json(c);
    post([this, appid, in, expect] {
        std::string out, err;
        bool ok = run_helper("apply", appid, in, kHelperTimeoutMs, out, err, nullptr, expect);
        int applied = 0, skipped = 0;
        if (ok) {
            json j = json::parse(out, nullptr, false);
            applied = j.value("applied", 0);
            skipped = j.value("skipped", 0);
        }
        std::string msg;
        if (ok) {
            msg = std::string(TR("Сохранено в Steam: ", "Saved to Steam: ")) + std::to_string(applied);
            if (skipped) msg += std::string(TR(", пропущено защищённых: ", ", protected skipped: ")) + std::to_string(skipped);
        } else {
            msg = helper_error_text(err);
        }
        bool same;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            sv_.applying = false;
            same = sv_.appid == appid;
            if (same) {
                sv_.result = msg;
                sv_.result_error = !ok;
            }
        }
        log::info("tools: apply app " + std::to_string(appid) + (ok ? " ok" : " failed: " + err));
        emit({});
        if (!same) {
            std::string name = game_name(appid);
            emit({ToolEventKind::Notice, (name.empty() ? "AppID " + std::to_string(appid) : name) + ": " + msg, !ok});
        } else if (ok) {
            load_stats(appid);
        }
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

template <class F>
bool SteamTools::update_fv(const FarmRun& r, F f) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (run_.get() != &r) return false;
    f(fv_);
    return true;
}

void SteamTools::reap_retired(bool wait) {
    for (auto it = retired_.begin(); it != retired_.end();) {
        auto& r = *it;
        if (wait || r->finished) {
            if (r->thread.joinable()) r->thread.join();
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

void SteamTools::start_run(FarmKind kind, std::vector<uint32_t> apps, int minutes_limit, cards::Mode mode) {
    stop_farm();
    auto r = std::make_shared<FarmRun>();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        r->id = next_run_id_++;
        run_ = r;
        fv_ = FarmView{};
        fv_.kind = kind;
        fv_.mode = mode;
        fv_.active = true;
        fv_.started = unix_now();
        fv_.stop_at = minutes_limit > 0 ? fv_.started + minutes_limit * 60 : 0;
        fv_.status = kind == FarmKind::Hours ? TR("Запускаю игры…", "Starting the games…") : TR("Смотрю, где остались карточки…", "Looking for remaining card drops…");
    }
    emit({});
    r->thread = std::thread([this, r, kind, apps, mode] { farm_main(r, kind, apps, mode); });
}

void SteamTools::start_hours(const std::vector<uint32_t>& apps_in, int minutes_limit) {
    std::vector<uint32_t> apps;
    for (uint32_t a : apps_in)
        if (a && std::find(apps.begin(), apps.end(), a) == apps.end() && (int)apps.size() < cards::kMaxParallel) apps.push_back(a);
    if (apps.empty()) return;
    start_run(FarmKind::Hours, apps, minutes_limit, cards::Mode::Simultaneous);
}

void SteamTools::start_cards(cards::Mode mode) { start_run(FarmKind::Cards, {}, 0, mode); }

void SteamTools::stop_farm(const std::string& why) {
    std::shared_ptr<FarmRun> r;
    bool was;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        r = run_;
        run_.reset();  // from here on the old run can't touch the view
        was = fv_.active;
        fv_.active = false;
        fv_.running.clear();
        fv_.next_check = 0;
        if (was) fv_.status = why.empty() ? std::string(TR("Остановлено", "Stopped")) : why;
    }
    if (r) {
        r->stop = true;
        r->cv.notify_all();
        retired_.push_back(r);  // winds down by itself (may be in the middle of a request) and kills its helpers
    }
    reap_retired(false);
    if (was) {
        log::info("tools: farm stopped" + (why.empty() ? std::string() : ": " + why));
        emit({});
    }
}

void SteamTools::check_now() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!run_) return;
    run_->check_now = true;
    run_->cv.notify_all();
}

void SteamTools::kill_helpers(FarmRun& r) {
    for (auto& [app, c] : r.procs) os::child_kill(c);
    r.procs.clear();
}

bool SteamTools::start_idle(FarmRun& r, uint32_t app, std::string& err) {
    std::string run = fsx::join(dir(), "run");
    fsx::mkdirs(run);
    std::string fout = fsx::join(run, "idle_" + std::to_string(r.id) + "_" + std::to_string(app) + ".json");
    fsx::remove_file(fout);
    std::string args = "--steam-helper idle --app " + std::to_string(app) + " --parent " + std::to_string(os::current_pid()) + " --out " + quote_arg(fout);
    if (r.steamid) args += " --steamid " + std::to_string(r.steamid);
    std::string root = root_fn_ ? root_fn_() : std::string();
    if (!root.empty()) args += " --root " + quote_arg(root);
    os::Child c;
    if (!os::spawn_child(os::self_exe_path(), args, c)) {
        err = "spawn";
        return false;
    }
    // wait for the helper to report that Steam accepted it
    bool ok = false;
    for (int i = 0; i < 100 && !ok; ++i) {
        std::string text;
        if (fsx::read_file(fout, text) && !text.empty()) {
            json j = json::parse(text, nullptr, false);
            if (j.is_object() && j.value("ok", false)) {
                ok = true;
                break;
            }
            if (j.is_object()) {
                err = j.value("error", std::string("error"));
                break;
            }
        }
        uint32_t code = 0;
        if (os::child_wait(c, 100, &code)) {
            // it may have written its error just before exiting
            if (fsx::read_file(fout, text)) {
                json j = json::parse(text, nullptr, false);
                if (j.is_object() && !j.value("ok", false)) err = j.value("error", std::string());
            }
            if (err.empty()) err = "exit_" + std::to_string(code);
            break;
        }
        if (r.stop || stop_) {
            err = "cancelled";
            break;
        }
    }
    fsx::remove_file(fout);
    if (ok) {
        r.procs[app] = c;
        return true;
    }
    if (err.empty()) err = "timeout";
    os::child_kill(c);
    return false;
}

std::string SteamTools::ensure_running(FarmRun& r) {
    std::string first_err;
    for (uint32_t app : r.want) {
        if (r.stop || stop_) break;
        auto it = r.procs.find(app);
        if (it != r.procs.end()) {
            if (os::child_running(it->second)) continue;
            os::child_kill(it->second);  // exited (Steam closed or signed out): start again
            r.procs.erase(it);
        }
        std::string err;
        if (!start_idle(r, app, err) && first_err.empty()) first_err = err;
        if (err == "wrong_account" || err == "not_running" || err == "not_logged_in" || err == "no_steam") break;  // same for every game
    }
    return first_err;
}

void SteamTools::publish_running(FarmRun& r, const std::string& err) {
    update_fv(r, [&](FarmView& fv) {
        std::vector<FarmRunning> keep;
        for (uint32_t app : r.want) {
            FarmRunning x;
            for (auto& old : fv.running)
                if (old.appid == app) x = old;
            x.appid = app;
            if (!x.started) x.started = unix_now();
            auto it = r.procs.find(app);
            x.ok = it != r.procs.end() && os::child_running(it->second);
            if (x.name.empty()) {
                for (auto& b : fv.badges)
                    if (b.appid == app) x.name = b.name;
                for (auto& g : lib_.games)
                    if (x.name.empty() && g.appid == app) x.name = g.name;
                if (x.name.empty()) x.name = "AppID " + std::to_string(app);
            }
            keep.push_back(x);
        }
        fv.running = keep;
        fv.error = err.empty() ? std::string() : helper_error_text(err) + (err == "cancelled" ? "" : std::string(TR(" — повторю через 30 с", " — retrying in 30 s")));
    });
}

void SteamTools::reconcile(FarmRun& r, const std::vector<uint32_t>& want) {
    for (auto it = r.procs.begin(); it != r.procs.end();) {
        if (std::find(want.begin(), want.end(), it->first) == want.end()) {
            os::child_kill(it->second);
            it = r.procs.erase(it);
        } else {
            ++it;
        }
    }
    r.want = want;
    std::string err = ensure_running(r);
    r.next_retry = err.empty() ? 0 : unix_now() + kRetrySeconds;
    if (err == "wrong_account") {
        end_run(r, ToolEventKind::FarmError, {}, std::string(TR("В Steam открыт другой аккаунт — фарм остановлен", "Another account is open in Steam — the farm stopped")),
                TR("Фарм остановлен: в Steam другой аккаунт", "Farm stopped: another account in Steam"));
        return;
    }
    publish_running(r, err);
    emit({});
}

void SteamTools::end_run(FarmRun& r, ToolEventKind kind, const std::string& status, const std::string& error, const std::string& event_text) {
    r.stop = true;
    kill_helpers(r);
    bool current = update_fv(r, [&](FarmView& fv) {
        fv.active = false;
        fv.running.clear();
        fv.next_check = 0;
        fv.status = status;
        fv.error = error;
    });
    if (current) emit({kind, event_text, kind == ToolEventKind::FarmError});
}

bool SteamTools::sleep_farm(FarmRun& r, int seconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!r.stop && !stop_) {
        if (r.check_now.exchange(false)) return true;
        // helpers that died (Steam restarted, crash) or never started are started again every 30 s
        bool missing = false;
        for (uint32_t app : r.want) {
            auto it = r.procs.find(app);
            if (it == r.procs.end() || !os::child_running(it->second)) missing = true;
        }
        if (missing) {
            if (!r.next_retry) {
                // first notice: show it and give Steam a moment (it may be restarting)
                r.next_retry = unix_now() + 5;
                publish_running(r, {});
                emit({});
            } else if (unix_now() >= r.next_retry) {
                reconcile(r, r.want);
                if (r.stop) return false;
            }
        } else if (r.next_retry) {
            r.next_retry = 0;
            publish_running(r, {});
            emit({});
        }
        int64_t stop_at = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (run_.get() == &r) stop_at = fv_.stop_at;
        }
        if (stop_at && unix_now() >= stop_at) return false;
        if (std::chrono::steady_clock::now() >= deadline) return true;
        std::unique_lock<std::mutex> lk(r.m);
        r.cv.wait_for(lk, std::chrono::seconds(1), [&] { return r.stop.load() || stop_.load() || r.check_now.load(); });
    }
    return false;
}

void SteamTools::farm_main(std::shared_ptr<FarmRun> rp, FarmKind kind, std::vector<uint32_t> apps, cards::Mode mode) {
    FarmRun& r = *rp;
    struct Done {
        FarmRun& r;
        ~Done() {
            kill_helpers(r);
            r.finished = true;
        }
    } done{r};
    std::string err;
    uint64_t sid = whoami(err, &r.stop);
    if (r.stop) return;
    if (!sid) {
        end_run(r, ToolEventKind::FarmError, {}, helper_error_text(err), helper_error_text(err));
        return;
    }
    r.steamid = sid;
    update_fv(r, [&](FarmView& fv) { fv.steamid = sid; });
    if (kind == FarmKind::Hours) {
        reconcile(r, apps);
        if (r.stop) return;
        update_fv(r, [&](FarmView& fv) { fv.status = TR("Набираю часы", "Gaining hours"); });
        emit({});
        log::info("tools: hours farm for " + std::to_string(apps.size()) + " game(s)");
        while (sleep_farm(r, 3600)) {
        }
        bool timed = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            timed = run_.get() == &r && fv_.stop_at && unix_now() >= fv_.stop_at;
        }
        if (timed) end_run(r, ToolEventKind::FarmFinished, TR("Готово — время вышло", "Done — time is up"), {}, TR("Фарм часов завершён", "Hours farm finished"));
        return;
    }
    // ---- cards
    if (!guard_.has_web_session(sid)) {
        update_fv(r, [&](FarmView& fv) { fv.need_login = true; });
        end_run(r, ToolEventKind::Changed, {}, TR("Чтобы видеть, сколько карточек осталось, нужен вход в аккаунт", "Sign in to see how many card drops are left"), {});
        return;
    }
    std::map<uint32_t, int> prev_drops;
    int empty_streak = 0;
    while (!r.stop && !stop_) {
        std::vector<cards::BadgeGame> all;
        bool ok = true, signed_in = true, need_login = false;
        std::string body;
        int status = 0;
        for (int page = 1, last = 1; page <= last && page <= kMaxBadgePages && !r.stop; ++page) {
            if (!guard_.community_get(sid, "/profiles/" + std::to_string(sid) + "/badges/?l=english&p=" + std::to_string(page), body, status, &need_login)) {
                ok = false;
                break;
            }
            cards::BadgePage bp;
            cards::parse_badges(body, bp);
            if (!bp.signed_in) {
                signed_in = false;
                break;
            }
            if (!bp.valid) {  // an error / maintenance page: not "no drops left"
                ok = false;
                break;
            }
            last = bp.last_page;
            for (auto& g : bp.games)
                if (g.drops > 0) all.push_back(g);
        }
        if (r.stop) break;
        if (!signed_in || need_login) {
            update_fv(r, [&](FarmView& fv) { fv.need_login = true; });
            end_run(r, ToolEventKind::FarmError, {}, TR("Steam завершил сессию — войдите снова", "Steam ended the session — sign in again"),
                    TR("Фарм карточек остановлен: нужен вход", "Card farm stopped: sign-in needed"));
            return;
        }
        if (!ok) {
            update_fv(r, [&](FarmView& fv) { fv.status = TR("Нет связи со Steam — повторю через минуту", "Can't reach Steam — retrying in a minute"); });
            emit({});
            if (!sleep_farm(r, 60)) break;
            continue;
        }
        if (all.empty() && !prev_drops.empty() && ++empty_streak < 2) {
            // make sure before calling it done: look once more in a minute
            update_fv(r, [&](FarmView& fv) { fv.status = TR("Похоже, карточек больше нет — проверю ещё раз", "Looks like no drops are left — checking once more"); });
            emit({});
            if (!sleep_farm(r, 60)) break;
            continue;
        }
        if (!all.empty()) empty_streak = 0;
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
                for (auto& x : fv_.badges)
                    if (x.appid == app) dropped_name = x.name;
            }
        prev_drops.clear();
        for (auto& g : all) prev_drops[g.appid] = g.drops;
        bool current = update_fv(r, [&](FarmView& fv) {
            fv.badges = all;
            fv.drops_left = left;
            fv.dropped += dropped_now;
        });
        if (!current) break;
        if (dropped_now > 0)
            emit({ToolEventKind::CardDropped,
                  (dropped_name.empty() ? std::string() : dropped_name + ": ") + TR("выпало карточек: ", "cards dropped: ") + std::to_string(dropped_now)});
        if (all.empty()) {
            end_run(r, ToolEventKind::FarmFinished, TR("Все карточки получены", "All card drops received"), {}, TR("Все карточки получены", "All card drops received"));
            log::info("tools: card farm finished");
            return;
        }
        auto want = cards::plan(all, mode);
        reconcile(r, want);
        if (r.stop) return;
        int wait = want.size() > 1 ? kCardsCheckMulti : kCardsCheckSingle;
        update_fv(r, [&](FarmView& fv) {
            fv.next_check = unix_now() + wait;
            fv.status = want.size() > 1 ? TR("Набираю 2 часа в играх, где карточки ещё не падают", "Reaching 2 hours in games that don't drop yet")
                                        : TR("Фарм карточек", "Farming cards");
        });
        emit({});
        if (!sleep_farm(r, wait)) break;
    }
}

}  // namespace vs
