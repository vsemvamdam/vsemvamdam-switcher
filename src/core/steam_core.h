// vsemvamdam-switcher — Steam file formats & pure transformations (portable, unit-tested)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vs::steam {

constexpr uint64_t kId64Base = 76561197960265728ULL;

struct LoginUser {
    std::string id64;
    std::string account;   // AccountName (login)
    std::string persona;   // PersonaName
    int64_t timestamp = 0;
    bool most_recent = false;
    bool remember = false;
    bool offline = false;
    bool allow_autologin = false;
};

bool is_valid_id64(const std::string& s);
uint32_t account_id(uint64_t id64);
std::string steam3(uint64_t id64);   // [U:1:123]
std::string steam2(uint64_t id64);   // STEAM_0:1:61
std::string profile_url(const std::string& id64);

bool parse_loginusers(const std::string& text, std::vector<LoginUser>& out, std::string* err = nullptr);
// Unique MostRecent / AllowAutoLogin row, or "" if ambiguous.
std::string active_id64(const std::vector<LoginUser>& users);

struct SwitchEdit {
    std::string target_id64;     // empty => "new account" (login screen)
    bool offline = false;
    LoginUser fallback;          // used when the target row is missing from the file (id64+account required)
};
// Produces new loginusers.vdf text. target_account receives the AccountName to put in AutoLoginUser.
bool rewrite_loginusers(const std::string& text, const SwitchEdit& e, std::string& out, std::string& target_account,
                        std::string* err = nullptr);
bool remove_loginuser(const std::string& text, const std::string& id64, std::string& out);

// Byte-preserving patch of userdata/<acc>/config/localconfig.vdf. Returns true if the text changed.
bool patch_persona_state(std::string& text, uint32_t account_id, int state);
// Reads the current persona state recorded for the account (-1 if unknown).
int read_persona_state(const std::string& text, uint32_t account_id);

// libraryfolders.vdf -> list of library roots (without \steamapps)
std::vector<std::string> library_paths(const std::string& text);
struct AppManifest {
    std::string appid, name, installdir;
};
bool parse_appmanifest(const std::string& text, AppManifest& out);

struct ProfileInfo {
    bool ok = false;
    bool is_private = false;
    bool vac = false;
    bool limited = false;
    bool trade_ban = false;
    std::string persona;
    std::string avatar_full;
};
bool parse_profile_xml(const std::string& xml, ProfileInfo& out);

// Persona states as stored by Steam
enum PersonaState { Offline = 0, Online = 1, Busy = 2, Away = 3, Snooze = 4, LookingToTrade = 5, LookingToPlay = 6, Invisible = 7 };

}  // namespace vs::steam
