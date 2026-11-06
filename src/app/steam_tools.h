// vsemvamdam-switcher — Steam tools: game library, achievement manager (SAM-like) and hours / trading-card farm
// (Idle Master-like). All Steam client work happens in "--steam-helper" child processes.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/cards.h"
#include "../core/steam_stats.h"
#include "../os/win.h"

namespace vs {

class Store;
class Guard;

struct ToolGame {
    uint32_t appid = 0;
    std::string name;
    int minutes = -1;        // playtime, -1 unknown
    bool has_stats = false;  // likely has achievements
    std::string icon;        // community icon hash (GetOwnedGames)
};
struct LibraryView {
    bool loading = false, loaded = false;
    uint64_t steamid = 0;
    bool from_web = false;   // full list from the account (otherwise: what this PC knows about)
    std::string error;
    std::vector<ToolGame> games;
};
struct StatsView {
    bool loading = false, applying = false;
    uint32_t appid = 0;      // the game `data` belongs to (and the one being loaded)
    stats::GameStats data;
    std::string error;
    std::string result;       // last apply result for this game (user-facing)
    bool result_error = false;
    int64_t loaded_at = 0;
};
enum class FarmKind { None, Hours, Cards };
struct FarmRunning {
    uint32_t appid = 0;
    std::string name;
    int64_t started = 0;
    bool ok = true;          // helper alive
};
struct FarmView {
    FarmKind kind = FarmKind::None;
    bool active = false;
    cards::Mode mode = cards::Mode::Auto;
    uint64_t steamid = 0;
    std::vector<FarmRunning> running;
    std::vector<cards::BadgeGame> badges;  // games with drops left
    int drops_left = 0;
    int dropped = 0;             // cards received since start
    int64_t started = 0, next_check = 0, stop_at = 0;
    std::string status, error;
    bool need_login = false;
};
enum class ToolEventKind { Changed, Notice, CardDropped, FarmFinished, FarmError };
struct ToolEvent {
    ToolEventKind kind = ToolEventKind::Changed;
    std::string text;
    bool error = false;
};

class SteamTools {
public:
    SteamTools(Store& st, Guard& guard, std::function<std::string()> steam_root);
    ~SteamTools();
    void stop();
    void on_event(std::function<void(const ToolEvent&)> cb) { on_event_ = std::move(cb); }
    std::string dir() const;

    // library of the account signed in to Steam
    void load_library(bool force);
    LibraryView library();
    std::string game_name(uint32_t appid);
    std::string icon_for(uint32_t appid);                                   // local file or "" (cached; cheap every frame)
    std::string achievement_icon(uint32_t appid, const std::string& file);  // "" until downloaded

    // achievements
    void load_stats(uint32_t appid);
    void apply(uint32_t appid, const stats::Changes& c);
    StatsView stats_view();

    // farm (stop_farm never waits for network: a stopped run winds down in the background)
    void start_hours(const std::vector<uint32_t>& apps, int minutes_limit);
    void start_cards(cards::Mode mode);
    void stop_farm(const std::string& why = {});
    void check_now();
    FarmView farm_view();
    bool farming();

    static std::string helper_error_text(const std::string& code);

private:
    Store& store_;
    Guard& guard_;
    std::function<std::string()> root_fn_;
    std::function<void(const ToolEvent&)> on_event_;
    std::atomic<bool> stop_{false};
    std::mutex mtx_;
    LibraryView lib_;
    bool lib_reload_ = false;   // a forced reload asked for while loading
    StatsView sv_;
    FarmView fv_;

    // jobs (library, stats, apply) — one at a time
    std::thread worker_;
    std::mutex qmtx_;
    std::condition_variable qcv_;
    std::deque<std::function<void()>> queue_;
    void post(std::function<void()> job);
    void worker_main();

    // icons — their own thread, so downloads never delay a save
    struct IconJob {
        std::string url, file;
        uint32_t appid = 0;
        bool game = false;
    };
    std::thread icon_thread_;
    std::mutex imtx_;
    std::condition_variable icv_;
    std::deque<IconJob> icon_q_;
    std::set<std::string> icon_queued_;
    void queue_icon(IconJob j, bool urgent);
    void icon_main();
    std::mutex cmtx_;                                     // caches below
    std::map<uint32_t, std::string> game_icons_;          // resolved file or "" (none)
    std::map<uint32_t, std::set<std::string>> ach_files_; // downloaded achievement icons per game

    // farm runs
    struct FarmRun {
        uint64_t id = 0;
        std::atomic<bool> stop{false};
        std::atomic<bool> check_now{false};
        std::atomic<bool> finished{false};
        std::mutex m;
        std::condition_variable cv;
        uint64_t steamid = 0;
        std::vector<uint32_t> want;           // games that should be running
        std::map<uint32_t, os::Child> procs;  // their helpers (this run's thread only)
        int64_t next_retry = 0;
        std::thread thread;
    };
    std::shared_ptr<FarmRun> run_;                      // the current run (mtx_)
    std::vector<std::shared_ptr<FarmRun>> retired_;     // stopped runs still winding down (UI thread)
    uint64_t next_run_id_ = 1;
    void start_run(FarmKind kind, std::vector<uint32_t> apps, int minutes_limit, cards::Mode mode);
    void reap_retired(bool wait);
    void farm_main(std::shared_ptr<FarmRun> r, FarmKind kind, std::vector<uint32_t> apps, cards::Mode mode);
    bool sleep_farm(FarmRun& r, int seconds);          // false when the run should end
    void reconcile(FarmRun& r, const std::vector<uint32_t>& want);
    std::string ensure_running(FarmRun& r);           // starts missing helpers; first error code or ""
    void publish_running(FarmRun& r, const std::string& err);
    bool start_idle(FarmRun& r, uint32_t app, std::string& err);
    static void kill_helpers(FarmRun& r);
    template <class F>
    bool update_fv(const FarmRun& r, F f);            // runs f(fv_) only while r is the current run
    void end_run(FarmRun& r, ToolEventKind kind, const std::string& status, const std::string& error, const std::string& event_text);

    // helpers
    bool run_helper(const std::string& mode, uint32_t app, const std::string& in_text, int timeout_ms, std::string& out_json, std::string& err,
                    const std::atomic<bool>* cancel = nullptr, uint64_t expect_steamid = 0);
    uint64_t whoami(std::string& err, const std::atomic<bool>* cancel = nullptr);
    std::vector<uint32_t> local_candidates(std::set<uint32_t>& with_schema);
    void fetch_icons_for(const stats::GameStats& g);
    void emit(ToolEvent e);
};

}  // namespace vs
