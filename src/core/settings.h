// vsemvamdam-switcher — persisted settings and per-account metadata
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vs {

enum class Theme { System, Light, Dark };
enum class AfterSwitch { Nothing, Minimize, Tray, Quit };
enum class SteamClose { Fast, Instant, Gentle };  // Fast: -shutdown + short grace, Instant: kill, Gentle: -shutdown, long wait

struct PlatformPrefs {
    bool enabled = false;
    std::string exe_path;      // override
    std::string args;          // extra launch args
    bool launch_after = true;  // start the client after switching
};

struct Settings {
    std::string language = "ru";
    Theme theme = Theme::Dark;
    bool close_to_tray = true;
    bool start_minimized = false;
    bool run_at_startup = false;
    AfterSwitch after_switch = AfterSwitch::Minimize;
    bool streamer_mode = false;
    bool grid_view = true;
    float card_scale = 1.0f;
    bool confirm_forget = true;
    int tray_accounts = 5;
    // window
    int win_x = 0, win_y = 0, win_w = 0, win_h = 0;
    bool win_max = false;
    std::string last_platform = "steam";
    std::vector<std::string> platform_order;
    // steam
    std::string steam_path;
    std::string steam_args;
    int steam_default_state = -1;   // -1 keep, 0 offline(mode), 1 online, 7 invisible, 3 away...
    bool steam_fetch_profiles = true;
    bool steam_show_vac = true;
    bool steam_show_limited = true;
    SteamClose steam_close = SteamClose::Fast;
    bool steam_launch = true;
    // other platforms
    std::map<std::string, PlatformPrefs> platforms;
};

struct AccountMeta {
    std::string custom_name;
    std::string note;
    int order = -1;
    int default_state = -2;  // -2 = use platform default
    std::string favorite_app;  // steam appid to launch after login
    int64_t last_switched = 0;
    int switch_count = 0;
    // steam profile cache
    std::string avatar_url;
    int64_t profile_checked = 0;
    bool vac = false, limited = false, trade_ban = false, is_private = false;
    std::string community_name;
    // basic platforms: known display name & account login
    std::string login;
    std::string display;
    int64_t saved_at = 0;
    int64_t forgotten_at = 0;  // steam: hide rows not used since this moment
};

class Store {
public:
    bool load(const std::string& dir);
    bool save_settings();
    bool save_meta();

    Settings settings;
    // platform -> id -> meta
    std::map<std::string, std::map<std::string, AccountMeta>> meta;
    std::string dir;

    AccountMeta& m(const std::string& platform, const std::string& id) { return meta[platform][id]; }
    std::recursive_mutex mtx;
};

std::string data_dir_default();  // %APPDATA%\vsemvamdam-switcher, or .\data if portable flag exists

}  // namespace vs
