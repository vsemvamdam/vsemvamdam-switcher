// Unit tests for portable core (run on Linux: make test)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/core/basic_core.h"
#include "../src/core/fsx.h"
#include "../src/core/image.h"
#include "../src/core/steam_core.h"
#include "../src/core/str.h"
#include "../src/core/vdf.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        auto _a = (a); auto _b = (b);                                                                      \
        if (_a == _b) ++g_pass;                                                                            \
        else { ++g_fail; std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); }               \
    } while (0)

using namespace vs;

#include "test_guard.inc"
#include "test_tools.inc"

static const char* kLoginUsers =
    "\xEF\xBB\xBF\"users\"\n{\n"
    "\t\"76561198000000001\"\n\t{\n"
    "\t\t\"AccountName\"\t\t\"main_acc\"\n"
    "\t\t\"PersonaName\"\t\t\"Шеф \\\"Main\\\"\"\n"
    "\t\t\"RememberPassword\"\t\t\"1\"\n"
    "\t\t\"WantsOfflineMode\"\t\t\"0\"\n"
    "\t\t\"SkipOfflineModeWarning\"\t\t\"0\"\n"
    "\t\t\"AllowAutoLogin\"\t\t\"1\"\n"
    "\t\t\"MostRecent\"\t\t\"1\"\n"
    "\t\t\"Timestamp\"\t\t\"1730000000\"\n"
    "\t}\n"
    "\t\"76561198000000002\"\n\t{\n"
    "\t\t\"AccountName\"\t\t\"smurf\"\n"
    "\t\t\"PersonaName\"\t\t\"back\\\\slash\"\n"
    "\t\t\"RememberPassword\"\t\t\"1\"\n"
    "\t\t\"WantsOfflineMode\"\t\t\"0\"\n"
    "\t\t\"SkipOfflineModeWarning\"\t\t\"0\"\n"
    "\t\t\"AllowAutoLogin\"\t\t\"0\"\n"
    "\t\t\"MostRecent\"\t\t\"0\"\n"
    "\t\t\"Timestamp\"\t\t\"1720000000\"\n"
    "\t}\n"
    "}\n";

static void test_vdf() {
    vdf::Node root;
    std::string err;
    CHECK(vdf::parse(kLoginUsers, root, &err));
    const vdf::Node* u = root.find("USERS");
    CHECK(u && u->is_object && u->children.size() == 2);
    CHECK_EQ(u->children[0].get("PersonaName"), std::string("Шеф \"Main\""));
    CHECK_EQ(u->children[1].get("personaname"), std::string("back\\slash"));
    // round trip keeps escapes stable
    std::string out = vdf::write(root);
    vdf::Node r2;
    CHECK(vdf::parse(out, r2));
    CHECK_EQ(vdf::write(r2), out);
    CHECK(out.find("back\\\\slash") != std::string::npos);
    // comments, unquoted tokens, conditionals
    vdf::Node r3;
    CHECK(vdf::parse("// c\nroot { key value [$WIN32] \"q\" \"v\" sub { a 1 } }", r3, &err));
    CHECK_EQ(r3.find_path({"root", "key"})->value, std::string("value"));
    CHECK_EQ(r3.find_path({"root", "sub", "a"})->value, std::string("1"));
    // broken files fail cleanly
    vdf::Node r4;
    CHECK(!vdf::parse("\"users\" { \"a\" { \"b\" \"c\" ", r4, &err));
}

static void test_ids() {
    uint64_t id = 76561198000000001ULL;
    CHECK_EQ(steam::account_id(id), 39734273u);
    CHECK_EQ(steam::steam3(id), std::string("[U:1:39734273]"));
    CHECK_EQ(steam::steam2(id), std::string("STEAM_0:1:19867136"));
    CHECK(steam::is_valid_id64("76561198000000001"));
    CHECK(!steam::is_valid_id64("123"));
    CHECK(!steam::is_valid_id64("abc"));
}

static void test_loginusers() {
    std::vector<steam::LoginUser> users;
    CHECK(steam::parse_loginusers(kLoginUsers, users));
    CHECK_EQ(users.size(), (size_t)2);
    CHECK_EQ(users[0].account, std::string("main_acc"));
    CHECK(users[0].most_recent && users[0].allow_autologin);
    CHECK_EQ(steam::active_id64(users), std::string("76561198000000001"));

    steam::SwitchEdit e;
    e.target_id64 = "76561198000000002";
    e.offline = true;
    std::string out, acc;
    CHECK(steam::rewrite_loginusers(kLoginUsers, e, out, acc));
    CHECK_EQ(acc, std::string("smurf"));
    std::vector<steam::LoginUser> after;
    CHECK(steam::parse_loginusers(out, after));
    CHECK_EQ(after.size(), (size_t)2);
    CHECK(!after[0].most_recent && !after[0].allow_autologin);
    CHECK(after[0].remember);  // others keep their saved logins
    CHECK(after[1].most_recent && after[1].allow_autologin && after[1].remember && after[1].offline);
    CHECK_EQ(after[1].persona, std::string("back\\slash"));
    CHECK_EQ(after[0].persona, std::string("Шеф \"Main\""));
    CHECK_EQ(after[0].timestamp, (int64_t)1730000000);

    // switch again (idempotent)
    std::string out2, acc2;
    CHECK(steam::rewrite_loginusers(out, e, out2, acc2));
    CHECK_EQ(out2, out);

    // new account / login screen
    steam::SwitchEdit none;
    CHECK(steam::rewrite_loginusers(kLoginUsers, none, out, acc));
    CHECK(acc.empty());
    CHECK(steam::parse_loginusers(out, after));
    CHECK(!after[0].most_recent && !after[1].most_recent);

    // missing row recreated from fallback
    steam::SwitchEdit miss;
    miss.target_id64 = "76561198000000077";
    miss.fallback.account = "ghost";
    miss.fallback.persona = "Ghost";
    CHECK(steam::rewrite_loginusers(kLoginUsers, miss, out, acc));
    CHECK_EQ(acc, std::string("ghost"));
    CHECK(steam::parse_loginusers(out, after));
    CHECK_EQ(after.size(), (size_t)3);
    CHECK_EQ(steam::active_id64(after), std::string("76561198000000077"));

    // unknown target without fallback fails and gives no output
    steam::SwitchEdit bad;
    bad.target_id64 = "76561198000000099";
    std::string err;
    CHECK(!steam::rewrite_loginusers(kLoginUsers, bad, out, acc, &err));

    // empty file + fallback
    CHECK(steam::rewrite_loginusers("", miss, out, acc));
    CHECK(steam::parse_loginusers(out, after));
    CHECK_EQ(after.size(), (size_t)1);

    // remove
    CHECK(steam::remove_loginuser(kLoginUsers, "76561198000000001", out));
    CHECK(steam::parse_loginusers(out, after));
    CHECK_EQ(after.size(), (size_t)1);
    CHECK_EQ(after[0].account, std::string("smurf"));
}

static void test_persona() {
    uint32_t acc = 39734273;
    std::string lc =
        "\"UserLocalConfigStore\"\n{\n\t\"friends\"\n\t{\n\t\t\"ePersonaState\"\t\t\"1\"\n\t}\n"
        "\t\"WebStorage\"\n\t{\n\t\t\"FriendStoreLocalPrefs_39734273\"\t\t\"{\\\"ePersonaState\\\":1,\\\"strNonFriendsAllowedToMsg\\\":\\\"\\\"}\"\n"
        "\t\t\"Other\"\t\t\"C:\\\\Games\\\\x\"\n\t}\n}\n";
    std::string s = lc;
    CHECK(steam::patch_persona_state(s, acc, 7));
    CHECK(s.find("\"ePersonaState\"\t\t\"7\"") != std::string::npos);
    CHECK(s.find("{\\\"ePersonaState\\\":7,") != std::string::npos);
    CHECK(s.find("C:\\\\Games\\\\x") != std::string::npos);  // untouched bytes
    CHECK_EQ(steam::read_persona_state(s, acc), 7);
    // no change if already set
    std::string s2 = s;
    CHECK(!steam::patch_persona_state(s2, acc, 7));
    CHECK_EQ(s2, s);
    // missing: insert into WebStorage
    std::string m = "\"UserLocalConfigStore\"\n{\n\t\"WebStorage\"\n\t{\n\t\t\"A\"\t\t\"b\"\n\t}\n}\n";
    CHECK(steam::patch_persona_state(m, acc, 0));
    CHECK(m.find("FriendStoreLocalPrefs_39734273") != std::string::npos);
    vdf::Node root;
    CHECK(vdf::parse(m, root));
    CHECK_EQ(steam::read_persona_state(m, acc), 0);
    // missing WebStorage: create it
    std::string m2 = "\"UserLocalConfigStore\"\n{\n\t\"x\"\t\t\"y\"\n}\n";
    CHECK(steam::patch_persona_state(m2, acc, 3));
    vdf::Node r2;
    CHECK(vdf::parse(m2, r2));
    CHECK(r2.find_path({"UserLocalConfigStore", "WebStorage", "FriendStoreLocalPrefs_39734273"}) != nullptr);
    CHECK_EQ(steam::read_persona_state(m2, acc), 3);
}

static void test_library() {
    std::string lf =
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"730\"\t\t\"1\"\n\t\t}\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n";
    auto p = steam::library_paths(lf);
    CHECK_EQ(p.size(), (size_t)2);
    CHECK_EQ(p[1], std::string("D:\\SteamLibrary"));
    steam::AppManifest am;
    CHECK(steam::parse_appmanifest("\"AppState\"\n{\n\t\"appid\"\t\t\"730\"\n\t\"name\"\t\t\"Counter-Strike 2\"\n\t\"installdir\"\t\t\"Counter-Strike Global Offensive\"\n}\n", am));
    CHECK_EQ(am.name, std::string("Counter-Strike 2"));
}

static void test_xml() {
    std::string x =
        "<?xml version=\"1.0\"?><profile><steamID64>76561198000000001</steamID64><steamID><![CDATA[Шеф]]></steamID>"
        "<privacyState>public</privacyState><vacBanned>1</vacBanned><tradeBanState>None</tradeBanState><isLimitedAccount>0</isLimitedAccount>"
        "<avatarFull><![CDATA[https://avatars.steamstatic.com/abc_full.jpg]]></avatarFull></profile>";
    steam::ProfileInfo pi;
    CHECK(steam::parse_profile_xml(x, pi));
    CHECK(pi.ok && pi.vac && !pi.limited && !pi.trade_ban && !pi.is_private);
    CHECK_EQ(pi.persona, std::string("Шеф"));
    CHECK_EQ(pi.avatar_full, std::string("https://avatars.steamstatic.com/abc_full.jpg"));
    CHECK(steam::parse_profile_xml("<response><error><![CDATA[The specified profile could not be found.]]></error></response>", pi) == false);
    CHECK(steam::parse_profile_xml("<profile><privacyMessage>This profile is private.</privacyMessage></profile>", pi));
    CHECK(pi.is_private);
}

static void test_str() {
    CHECK(icontains("Привет МИР", "мир"));
    CHECK(icontains("Ёжик", "еж"));
    CHECK(icontains("Counter-Strike", "strike"));
    CHECK_EQ(std::string(plural_ru(1, "a", "b", "c")), std::string("a"));
    CHECK_EQ(std::string(plural_ru(3, "a", "b", "c")), std::string("b"));
    CHECK_EQ(std::string(plural_ru(11, "a", "b", "c")), std::string("c"));
    CHECK_EQ(std::string(plural_ru(22, "a", "b", "c")), std::string("b"));
    CHECK_EQ(time_ago_ru(1000, 1000 + 7200), std::string("2 часа назад"));
    CHECK_EQ(sanitize_filename("a:b*c?"), std::string("a_b_c_"));
    CHECK(fsx::wildcard_match("*.vdf", "LoginUsers.VDF"));
    CHECK(!fsx::wildcard_match("*.vdf", "x.vdf2"));
}

static void test_image() {
    img::Image a = img::placeholder(3, 64);
    CHECK(a.ok());
    img::Image b = img::resize(a, 16, 16);
    CHECK(b.ok() && b.w == 16);
    auto ico = img::make_ico(a.w, a.h, a.rgba);
    CHECK(ico.size() > 100 && ico[2] == 1);
}

static void test_basic_core() {
    auto r = basic::parse_spec("REG:HKCU\\Software\\Epic Games\\Unreal Engine\\Identifiers:AccountId");
    CHECK(r.kind == basic::Spec::Reg);
    CHECK_EQ(r.path, std::string("HKCU\\Software\\Epic Games\\Unreal Engine\\Identifiers"));
    CHECK_EQ(r.value_name, std::string("AccountId"));
    auto j = basic::parse_spec("JSON_SELECT_FIRST,::%AppData%\\Battle.net\\Battle.net.config::Client.SavedAccountNames");
    CHECK(j.kind == basic::Spec::Json && j.json_mode == "JSON_SELECT_FIRST" && j.delimiter == ",");
    CHECK_EQ(j.path, std::string("%AppData%\\Battle.net\\Battle.net.config"));
    CHECK_EQ(j.json_path, std::string("Client.SavedAccountNames"));
    auto e = basic::parse_spec("JSON_EMPTY_VALUE::C:\\x.json::A.B");
    CHECK(e.kind == basic::Spec::Json && e.json_mode == "JSON_EMPTY_VALUE");
    auto f = basic::parse_spec("%LocalAppData%\\Riot Games\\Riot Client\\Data");
    CHECK(f.kind == basic::Spec::File);

    std::map<std::string, std::string> vars = {{"appdata", "C:\\Users\\g\\AppData\\Roaming"}, {"platform_folder", "D:\\Ubi"}};
    CHECK_EQ(basic::expand_vars("%AppData%\\x;%Platform_Folder%\\logs;%Unknown%", vars),
             std::string("C:\\Users\\g\\AppData\\Roaming\\x;D:\\Ubi\\logs;%Unknown%"));

    nlohmann::json cfg = nlohmann::json::parse(R"({"Client":{"SavedAccountNames":"a@b.c,d@e.f","X":1}})");
    auto* v = basic::json_get(cfg, "Client.SavedAccountNames");
    CHECK(v && v->is_string());
    CHECK_EQ(basic::first_of(v->get<std::string>(), ","), std::string("a@b.c"));
    CHECK(basic::json_set(cfg, "Client.SavedAccountNames", "z@z.z"));
    CHECK_EQ(cfg["Client"]["SavedAccountNames"].get<std::string>(), std::string("z@z.z"));
    CHECK_EQ(cfg["Client"]["X"].get<int>(), 1);
    nlohmann::json empty = nlohmann::json::object();
    CHECK(basic::json_set(empty, "A.B.C", 5) && empty["A"]["B"]["C"] == 5);

    std::string log = "2024 start\nUser: 11111111-2222-3333-4444-555555555555 logged\nnoise\nUser:   aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee again\n";
    CHECK_EQ(basic::regex_id(log, "(?s).*User:\\s+([0-9a-fA-F-]{36})"), std::string("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));
    CHECK_EQ(basic::regex_id("[ini]\nid=ABC123\r\nother=1", "=([^\\r\\n]*)"), std::string("ABC123"));
    CHECK_EQ(basic::regex_id("{\"email\":\"Gera@Mail.ru\"}", "EMAIL_REGEX"), std::string("Gera@Mail.ru"));
    CHECK_EQ(basic::regex_id("abc", "(unclosed"), std::string(""));
    CHECK_EQ(basic::slug("Epic Games (Legacy)"), std::string("epic-games-legacy"));
    CHECK_EQ(basic::random_id().size(), (size_t)32);
}

int main() {
    test_basic_core();
    test_vdf();
    test_ids();
    test_loginusers();
    test_persona();
    test_library();
    test_xml();
    test_str();
    test_image();
    test_crypto();
    test_guard();
    test_kv_and_schema();
    test_cards();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
