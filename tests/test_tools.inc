// Achievements (binary KeyValues schema) and farming (badges page, owned games, plan) unit tests
#include "../src/core/cards.h"
#include "../src/core/kv_binary.h"
#include "../src/core/steam_stats.h"

namespace kvb {
// tiny builder for binary KeyValues
struct B {
    std::string d;
    B& obj(const char* n) { d += (char)0; d += n; d += (char)0; return *this; }
    B& end() { d += (char)8; return *this; }
    B& str(const char* n, const std::string& v) { d += (char)1; d += n; d += (char)0; d += v; d += (char)0; return *this; }
    B& i32(const char* n, int32_t v) {
        d += (char)2; d += n; d += (char)0;
        for (int k = 0; k < 4; ++k) d += (char)((uint32_t)v >> (8 * k));
        return *this;
    }
    B& f32(const char* n, float v) {
        d += (char)3; d += n; d += (char)0;
        uint32_t u; std::memcpy(&u, &v, 4);
        for (int k = 0; k < 4; ++k) d += (char)(u >> (8 * k));
        return *this;
    }
    B& u64(const char* n, uint64_t v) {
        d += (char)7; d += n; d += (char)0;
        for (int k = 0; k < 8; ++k) d += (char)(v >> (8 * k));
        return *this;
    }
};
}  // namespace kvb

static void test_kv_and_schema() {
    kvb::B b;
    b.obj("440")
        .str("gamename", "Team Fortress 2")
        .i32("version", 7)
        .obj("stats")
            .obj("1").str("type", "ACHIEVEMENTS").i32("id", 1)
                .obj("bits")
                    .obj("0").str("name", "TF_PLAY_GAME_EVERYCLASS").i32("permission", 0)
                        .obj("display")
                            .obj("name").str("english", "Head of the Class").str("russian", "Лучший в классе").end()
                            .obj("desc").str("english", "Play a complete round with every class.").end()
                            .str("hidden", "0").str("icon", "aaa.jpg").str("icon_gray", "bbb.jpg")
                        .end()
                    .end()
                    .obj("1").str("name", "TF_SECRET").i32("permission", 2)
                        .obj("display").obj("name").str("english", "Secret").end().i32("hidden", 1).end()
                    .end()
                .end()
            .end()
            .obj("2").i32("type_int", 1).str("name", "kills").i32("min", 0).i32("max", 1000000).i32("incrementonly", 1)
                .obj("display").str("name", "Kills").end()
            .end()
            .obj("3").str("type", "FLOAT").str("name", "distance").f32("max", 5000.5f)
                .obj("display").obj("name").str("english", "Distance").end().end()
            .end()
            .obj("4").str("type", "INT").str("name", "server_only").i32("permission", 1).end()
        .end()
    .end()
    .end();
    kv::Node root;
    CHECK(kv::parse_binary(b.d, root));
    CHECK_EQ(root["440"]["gamename"].as_string(), std::string("Team Fortress 2"));
    CHECK_EQ(root["440"]["VERSION"].as_int(), (int64_t)7);  // case-insensitive
    CHECK(!root["nope"]["x"].valid);
    CHECK(!kv::parse_binary(b.d.substr(0, b.d.size() - 3), root));  // truncated
    kvb::B u;
    u.u64("big", 76561198000000001ULL).end();
    CHECK(kv::parse_binary(u.d, root) && root["big"].as_int() == (int64_t)76561198000000001LL);

    kv::parse_binary(b.d, root);
    stats::Schema sc;
    CHECK(stats::parse_schema(root, 440, "russian", sc));
    CHECK_EQ(sc.achievements.size(), (size_t)2);
    CHECK_EQ(sc.achievements[0].id, std::string("TF_PLAY_GAME_EVERYCLASS"));
    CHECK_EQ(sc.achievements[0].name, std::string("Лучший в классе"));
    CHECK_EQ(sc.achievements[0].desc, std::string("Play a complete round with every class."));  // english fallback
    CHECK(!sc.achievements[0].hidden && sc.achievements[1].hidden);
    CHECK_EQ(sc.achievements[0].icon, std::string("aaa.jpg"));
    CHECK(!stats::is_protected(sc.achievements[0].permission) && stats::is_protected(sc.achievements[1].permission));
    CHECK_EQ(sc.stats.size(), (size_t)3);
    CHECK(sc.stats[0].id == "kills" && sc.stats[0].type == stats::StatInt && sc.stats[0].incrementonly && sc.stats[0].max == 1000000);
    CHECK_EQ(sc.stats[0].name, std::string("Kills"));
    CHECK(sc.stats[1].id == "distance" && sc.stats[1].type == stats::StatFloat && sc.stats[1].max > 5000 && sc.stats[1].max < 5001);
    CHECK(stats::is_protected(sc.stats[2].permission));
    stats::Schema none;
    CHECK(!stats::parse_schema(root, 730, "english", none));

    // JSON round trips with the helper process
    stats::GameStats g;
    g.appid = 440;
    g.name = "TF2 «тест»";
    g.steamid = 76561198000000001ULL;
    g.achievements.push_back({sc.achievements[0], true, 1700000000});
    g.stats.push_back({sc.stats[1], 12.5});
    stats::GameStats g2;
    CHECK(stats::from_json(stats::to_json(g), g2));
    CHECK(g2.appid == 440 && g2.name == g.name && g2.steamid == g.steamid);
    CHECK(g2.achievements.size() == 1 && g2.achievements[0].achieved && g2.achievements[0].unlock_time == 1700000000 &&
          g2.achievements[0].def.name == "Лучший в классе");
    CHECK(g2.stats.size() == 1 && g2.stats[0].value == 12.5 && g2.stats[0].def.type == stats::StatFloat);
    stats::Changes ch;
    ch.achievements["A"] = true;
    ch.achievements["B"] = false;
    ch.stats["kills"] = 42;
    ch.reset_all = true;
    stats::Changes ch2;
    CHECK(stats::from_json(stats::to_json(ch), ch2) && ch2.achievements.size() == 2 && !ch2.achievements["B"] && ch2.stats["kills"] == 42 &&
          ch2.reset_all && !ch2.reset_with_achievements);
}

static const char* kBadges = R"(<!DOCTYPE html><html><head><script>g_steamID = "76561198000000001";</script></head><body>
<div class="badges_sheet">
<div class="badge_row is_link">
  <a class="badge_row_overlay" href="https://steamcommunity.com/profiles/76561198000000001/gamecards/730/"></a>
  <div class="badge_row_inner"><div class="badge_title_row"><div class="badge_title_stats"><div class="badge_title_stats_content">
    <div class="badge_title_stats_playtime">&nbsp;1,234.5 hrs on record</div>
    <div class="badge_title_stats_drops"><span class="progress_info_bold">3 card drops remaining</span>
      <div class="card_drop_info_dialog" id="card_drop_info_gamebadge_730_1_0" style="display: none">
        <div class="card_drop_info_header">Card drops received: 2</div>
        <div class="card_drop_info_body">You can get 3 more trading cards by playing Counter-Strike 2.</div></div></div>
  </div></div>
  <div class="badge_title">
    Counter-Strike 2&nbsp;<span class="badge_view_details">View details</span>
  </div></div></div>
</div>
<div class="badge_row is_link">
  <a class="badge_row_overlay" href="https://steamcommunity.com/profiles/76561198000000001/gamecards/570/"></a>
  <div class="badge_title_stats_playtime">&nbsp;0.4 hrs on record</div>
  <span class="progress_info_bold">1 card drop remaining</span>
  <div class="badge_title">Dota&nbsp;2 &amp; Friends&nbsp;<span class="badge_view_details">View details</span></div>
</div>
<div class="badge_row is_link">
  <a class="badge_row_overlay" href="https://steamcommunity.com/profiles/76561198000000001/gamecards/10/"></a>
  <div class="badge_title_stats_playtime">&nbsp;25 hrs on record</div>
  <span class="progress_info_bold">No card drops remaining</span>
  <div class="badge_title">Counter-Strike&nbsp;<span class="badge_view_details">View details</span></div>
</div>
<div class="badge_row is_link">
  <a class="badge_row_overlay" href="https://steamcommunity.com/profiles/76561198000000001/badges/1/"></a>
  <div class="badge_title">Years of Service&nbsp;<span class="badge_view_details">View details</span></div>
</div>
<div class="badge_row is_link">
  <a class="badge_row_overlay" href="https://steamcommunity.com/profiles/76561198000000001/gamecards/730/?border=1"></a>
  <span class="progress_info_bold">No card drops remaining</span>
  <div class="badge_title">Counter-Strike 2 (foil)&nbsp;</div>
</div>
</div>
<div class="pageLinks"><a class="pagelink" href="?p=2">2</a><a class="pagelink" href="?p=3">3</a></div>
</body></html>)";

static void test_cards() {
    cards::BadgePage bp;
    CHECK(cards::parse_badges(kBadges, bp));
    CHECK(bp.signed_in);
    CHECK(bp.valid);
    CHECK_EQ(bp.last_page, 3);
    CHECK_EQ(bp.games.size(), (size_t)3);
    CHECK(bp.games.size() == 3 && bp.games[0].appid == 730 && bp.games[0].drops == 3 && bp.games[0].hours > 1234.4 && bp.games[0].hours < 1234.6);
    CHECK(bp.games.size() == 3 && bp.games[0].name == "Counter-Strike 2");
    CHECK(bp.games.size() == 3 && bp.games[1].appid == 570 && bp.games[1].drops == 1 && bp.games[1].hours > 0.39 && bp.games[1].hours < 0.41);
    CHECK(bp.games.size() == 3 && bp.games[1].name == "Dota 2 & Friends");
    CHECK(bp.games.size() == 3 && bp.games[2].appid == 10 && bp.games[2].drops == 0 && bp.games[2].hours == 25);
    cards::BadgePage pub;
    CHECK(cards::parse_badges("<html><script>g_steamID = false;</script><a href=\"https://steamcommunity.com/login/home/\">Sign In</a></html>", pub) &&
          !pub.signed_in && pub.games.empty());
    // an error page with the signed-in header is not "no drops left"
    cards::BadgePage err;
    CHECK(cards::parse_badges("<html><script>g_steamID = \"76561198000000001\";</script><div class=\"error_ctn\">An error was encountered while processing your request</div></html>", err) &&
          !err.valid && err.games.empty());
    cards::BadgePage none;
    CHECK(cards::parse_badges("<html><script>g_steamID = \"76561198000000001\";</script><div class=\"profile_xp_block\">Level 12</div><div class=\"badges_sheet\"></div></html>", none) &&
          none.valid && none.signed_in && none.games.empty());

    std::vector<cards::OwnedGame> og;
    CHECK(cards::parse_owned_games(R"({"response":{"game_count":2,"games":[{"appid":730,"name":"Counter-Strike 2","playtime_forever":74070,
        "img_icon_url":"8dbc71957312bbd3baea65848b545be9eae2a355","has_community_visible_stats":true},{"appid":4000,"name":"Garry's Mod","playtime_forever":0}]}})", og));
    CHECK(og.size() == 2 && og[0].appid == 730 && og[0].minutes == 74070 && og[0].has_stats && !og[1].has_stats && og[1].name == "Garry's Mod");
    CHECK(cards::parse_owned_games(R"({"response":{}})", og) && og.empty());
    CHECK(!cards::parse_owned_games("nope", og));

    auto ids = cards::parse_sam_games_xml(R"(<?xml version="1.0"?><games><game>220</game><game type="demo">530</game><game type="junk">999</game><game>240</game></games>)");
    CHECK(ids.size() == 3 && ids[0] == 220 && ids[1] == 530 && ids[2] == 240);

    std::vector<cards::BadgeGame> games = {{1, "a", 2, 5.0}, {2, "b", 4, 0.5}, {3, "c", 1, 1.5}, {4, "d", 0, 0.1}, {5, "e", 6, 3.0}};
    auto p = cards::plan(games, cards::Mode::Auto);
    CHECK(p.size() == 2 && p[0] == 3 && p[1] == 2);  // under 2 h together, closest to 2 h first
    p = cards::plan(games, cards::Mode::Sequential);
    CHECK(p.size() == 1 && p[0] == 5);                // most drops
    p = cards::plan(games, cards::Mode::Simultaneous, 3);
    CHECK(p.size() == 3 && p[0] == 5 && p[1] == 2 && p[2] == 1);
    std::vector<cards::BadgeGame> old = {{7, "x", 1, 9}, {8, "y", 3, 2.5}};
    p = cards::plan(old, cards::Mode::Auto);
    CHECK(p.size() == 1 && p[0] == 8);                // all past 2 h -> one at a time
    CHECK(cards::plan({{9, "z", 0, 1}}, cards::Mode::Auto).empty());
    CHECK_EQ(cards::html_unescape("A&amp;B&#39;s &#x41;&nbsp;&lt;"), std::string("A&B's A <"));
}
