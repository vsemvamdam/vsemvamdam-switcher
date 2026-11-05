// vsemvamdam-switcher — generic "snapshot & restore" platforms (Epic, Battle.net, Riot, Ubisoft, Discord, ...)
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "platform.h"

namespace vs {

class BasicPlatform : public Platform {
public:
    BasicPlatform(Store& st, std::string id, nlohmann::json def);
    std::string id() const override { return id_; }
    std::string name() const override { return name_; }
    std::string exe_path() override;
    bool installed() override;
    std::vector<Account> scan() override;
    OpResult switch_to(const SwitchRequest& r, const Progress& p) override;
    OpResult forget(const std::string& account_id) override;
    bool can_save_current() const override { return true; }
    OpResult save_current(const std::string& display, const std::string& id_hint, std::string& out_id) override;
    OpResult close_client() override;
    std::vector<std::string> process_names() const override;
    std::string data_folder() override;
    std::string current_account_id() override;
    std::string help() const override { return help_; }
    std::string saves_dir() const;
    std::string account_dir(const std::string& id) const;
    const nlohmann::json& def() const { return def_; }

private:
    Store& store_;
    std::string id_, name_, help_;
    nlohmann::json def_;
    std::string expand(const std::string& s);
    std::string platform_folder();
    std::string unique_id_now();
    bool close_now(const Progress& p, OpResult& r);
    bool clear_login(std::string& err);
    bool backup_to(const std::string& dir, std::string& err);
    bool restore_from(const std::string& dir, std::string& err);
    bool can_restore(const std::string& dir, std::string& err);
    bool snapshot_into(const std::string& sid, std::string& err);
    void autosave_unknown();
    void launch();
    // New per-user Epic Games Launcher (session in Credential Manager + DPAPI auth files)
    bool epic_new();
    std::string epic_uid();
    bool epic_save(const std::string& dir, std::string& err);
    bool epic_restore(const std::string& dir, std::string& err);
    void epic_clear();
};

std::vector<std::unique_ptr<Platform>> make_basic_platforms(Store& st, const std::string& json_text);

}  // namespace vs
