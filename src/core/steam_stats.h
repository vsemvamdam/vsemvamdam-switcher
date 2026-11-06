// vsemvamdam-switcher — achievements & stats (Steam Achievement Manager logic), portable:
// schema from UserGameStatsSchema_<appid>.bin, JSON exchanged with the --steam-helper process.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "kv_binary.h"

namespace vs::stats {

enum StatType { StatInvalid = 0, StatInt = 1, StatFloat = 2, StatAvgRate = 3, StatAchievements = 4, StatGroupAchievements = 5 };

struct AchievementDef {
    std::string id, name, desc, icon, icon_gray;
    bool hidden = false;
    int permission = 0;
};
struct StatDef {
    std::string id, name;
    int type = StatInt;
    double min = 0, max = 0, maxchange = 0, def = 0;
    bool incrementonly = false, set_by_gs = false;
    int permission = 0;
};
struct Schema {
    std::vector<AchievementDef> achievements;
    std::vector<StatDef> stats;
};
// Game-protected values (set by the game's servers) cannot be changed from the client.
inline bool is_protected(int permission) { return (permission & 3) != 0; }

// language: Steam's name, e.g. "russian", "english"; falls back to english, then to the raw value.
bool parse_schema(const kv::Node& root, uint32_t appid, const std::string& language, Schema& out);

struct Achievement {
    AchievementDef def;
    bool achieved = false;
    uint32_t unlock_time = 0;
};
struct Stat {
    StatDef def;
    double value = 0;
};
struct GameStats {
    uint32_t appid = 0;
    std::string name;
    uint64_t steamid = 0;
    std::vector<Achievement> achievements;
    std::vector<Stat> stats;
    std::string error;  // filled by the helper on failure (code word, see steam_helper.cpp)
};
std::string to_json(const GameStats& g);
bool from_json(const std::string& text, GameStats& out);

struct Changes {
    std::map<std::string, bool> achievements;  // id -> unlocked
    std::map<std::string, double> stats;       // id -> new value
    bool reset_all = false;                    // ResetAllStats
    bool reset_with_achievements = false;
};
std::string to_json(const Changes& c);
bool from_json(const std::string& text, Changes& out);

// Steam language name for the UI language / Windows locale ("russian", "english", ...)
std::string steam_language(bool english_ui);

}  // namespace vs::stats
