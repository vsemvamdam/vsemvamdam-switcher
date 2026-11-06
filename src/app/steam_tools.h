// vsemvamdam-switcher — Steam tools: game library, achievement manager (SAM-like) and hours / trading-card farm
// (Idle Master-like). All Steam client work happens in "--steam-helper" child processes.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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
    uint32_t appid = 0;
    stats::GameStats data;
    std::string error;
    std::string result;       // last apply result (user-facing)
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
enum class ToolEventKind { Changed, CardDropped, FarmFinished, FarmError };
struct ToolEvent {
    ToolEventKind kind = ToolEventKind::Changed;
    std::string text;
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
    std::string icon_for(uint32_t appid);                       // local file or "" (downloads in the background)
    std::string achievement_icon(uint32_t appid, const std::string& file);  // "" until downloaded

    // achievements
    void load_stats(uint32_t appid);
    void apply(uint32_t appid, const stats::Changes& c);
    StatsView stats_view();

    // farm
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
    StatsView sv_;
    FarmView fv_;
    std::map<uint32_t, os::Child> procs_;  // idle helpers (farm thread only)
    std::map<uint32_t, int> restarts_;

    std::thread worker_;
    std::mutex qmtx_;
    std::condition_variable qcv_;
    std::deque<std::function<void()>> queue_;
    void post(std::function<void()> job);
    void worker_main();

    std::thread farm_thread_;
    std::condition_variable fcv_;
    std::mutex fmtx_;
    std::atomic<bool> farm_stop_{false};
    std::atomic<bool> check_now_{false};
    void farm_main(FarmKind kind, std::vector<uint32_t> apps, int minutes_limit, cards::Mode mode);
    bool sleep_farm(int seconds);  // false when the farm should stop
    void reconcile(const std::vector<uint32_t>& want);
    void kill_all_helpers();

    // helpers
    bool run_helper(const std::string& mode, uint32_t app, const std::string& in_text, int timeout_ms, std::string& out_json, std::string& err,
                    const std::atomic<bool>* cancel = nullptr);
    bool start_idle(uint32_t app, std::string& err);
    uint64_t whoami(std::string& err, const std::atomic<bool>* cancel = nullptr);
    std::vector<uint32_t> local_candidates(std::set<uint32_t>& with_schema);
    void fetch_icons_for(const stats::GameStats& g);
    void emit(ToolEvent e);
    void set_farm_status(const std::string& status, const std::string& error = {});
};

}  // namespace vs
