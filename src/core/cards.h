// vsemvamdam-switcher — card / hours farming logic (Idle Master / ASF style), portable
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vs::cards {

// One row of steamcommunity.com/profiles/<id>/badges (only games that still have drops matter)
struct BadgeGame {
    uint32_t appid = 0;
    std::string name;
    int drops = 0;       // card drops remaining
    double hours = 0;    // hours on record
};
struct BadgePage {
    std::vector<BadgeGame> games;  // rows with a game badge (drops may be 0)
    int last_page = 1;
    bool signed_in = true;         // false: the page is the public view / a login page
};
bool parse_badges(const std::string& html, BadgePage& out);

// IPlayerService/GetOwnedGames
struct OwnedGame {
    uint32_t appid = 0;
    std::string name;
    int minutes = 0;          // playtime_forever
    bool has_stats = false;   // has_community_visible_stats (achievements / stats)
    std::string icon;         // img_icon_url hash
};
bool parse_owned_games(const std::string& json, std::vector<OwnedGame>& out);

// gib.me/sam/games.xml (Steam Achievement Manager's list of apps with stats); junk entries are skipped
std::vector<uint32_t> parse_sam_games_xml(const std::string& xml);

// Which games to run right now.
enum class Mode {
    Auto,         // like Idle Master: games under 2 h run together (up to max), then one game at a time
    Sequential,   // always one game, most drops first
    Simultaneous  // everything with drops at once (up to max)
};
constexpr int kMaxParallel = 32;      // Steam counts up to 32 games played at the same time
constexpr double kHoursThreshold = 2.0;
std::vector<uint32_t> plan(const std::vector<BadgeGame>& games, Mode mode, int max_parallel = kMaxParallel,
                           double threshold_hours = kHoursThreshold);

std::string html_unescape(const std::string& s);

}  // namespace vs::cards
