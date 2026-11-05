// vsemvamdam-switcher — common platform interface
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vs {

class Store;

struct Account {
    std::string id;          // steam: SteamID64; basic: unique id
    std::string login;       // account / e-mail / username
    std::string display;     // shown name (persona, nickname)
    std::string avatar_path; // local image file (may be empty)
    int64_t last_login = 0;
    bool current = false;
    bool offline_mode = false;
    bool vac = false, limited = false, trade_ban = false;
};

struct Game {
    std::string appid;
    std::string name;
    std::string icon_path;
};

// Persona states used in switch requests
constexpr int kStateDefault = -2;   // use account / platform default
constexpr int kStateKeep = -1;      // don't touch
constexpr int kStateOfflineMode = 100;  // Steam offline mode (WantsOfflineMode)

struct SwitchRequest {
    std::string account_id;  // empty => new login (clear)
    int state = kStateDefault;
    std::string app_id;      // steam: launch this app after login
    std::string extra_args;
};

struct OpResult {
    bool ok = false;
    std::string message;      // localized, user-facing
    bool needs_admin = false;
    double seconds = 0;
};

using Progress = std::function<void(const std::string& step)>;

class Platform {
public:
    virtual ~Platform() = default;
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    virtual bool is_steam() const { return false; }
    virtual std::string exe_path() = 0;
    virtual bool installed() = 0;
    virtual std::vector<Account> scan() = 0;
    virtual OpResult switch_to(const SwitchRequest& r, const Progress& p) = 0;
    virtual OpResult forget(const std::string& account_id) = 0;
    virtual bool can_save_current() const { return false; }
    virtual OpResult save_current(const std::string& display, const std::string& id_hint, std::string& out_id) {
        (void)display; (void)id_hint; (void)out_id;
        return {};
    }
    virtual OpResult start_client();
    virtual OpResult close_client();
    virtual std::vector<std::string> process_names() const = 0;
    virtual std::string data_folder() { return {}; }   // where platform stores login data (for "open folder")
    virtual std::string current_account_id() { return {}; }
    virtual std::string help() const { return {}; }
    bool running() const;
};

}  // namespace vs
