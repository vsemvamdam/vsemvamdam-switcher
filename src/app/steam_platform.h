// vsemvamdam-switcher — Steam account switching
#pragma once
#include <mutex>

#include "platform.h"

namespace vs {

class SteamPlatform : public Platform {
public:
    explicit SteamPlatform(Store& st) : store_(st) {}
    std::string id() const override { return "steam"; }
    std::string name() const override { return "Steam"; }
    bool is_steam() const override { return true; }
    std::string exe_path() override;
    bool installed() override;
    std::vector<Account> scan() override;
    OpResult switch_to(const SwitchRequest& r, const Progress& p) override;
    OpResult forget(const std::string& account_id) override;
    OpResult close_client() override;
    std::vector<std::string> process_names() const override;
    std::string data_folder() override { return root(); }
    std::string current_account_id() override;

    std::string root();  // Steam install folder
    std::vector<Game> games();  // installed games (cached)
    void invalidate_games() { std::lock_guard<std::mutex> lk(gm_); games_loaded_ = false; }
    // Background profile refresh (avatars / VAC). Calls done() on completion (worker thread).
    void refresh_profiles(bool force, const std::function<void(bool changed)>& done);
    std::string avatar_for(const std::string& id64);
    std::string userdata_dir(const std::string& id64);

private:
    Store& store_;
    std::mutex gm_;
    std::vector<Game> games_;
    bool games_loaded_ = false;
    std::atomic<bool> refreshing_{false};
    OpResult close_steam(const Progress& p);
};

// Display label for a persona state value
const char* steam_state_label(int st);

}  // namespace vs
