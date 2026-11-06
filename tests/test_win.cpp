// Windows integration test — runs the real switching code against a fake Steam / fake platform.
// Build: make wintest   Run: wine build/wintest.exe   (or on Windows)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

#include "../src/app/basic_platform.h"
#include "../src/app/steam_platform.h"
#include "../src/core/fsx.h"
#include "../src/core/settings.h"
#include "../src/core/steam_core.h"
#include "../src/core/str.h"
#include "../src/os/win.h"

using namespace vs;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fflush(stdout);} } while (0)

static std::string g_root;

static const char* kLU =
    "\"users\"\n{\n"
    "\t\"76561198000000001\"\n\t{\n\t\t\"AccountName\"\t\t\"main_acc\"\n\t\t\"PersonaName\"\t\t\"Main\"\n\t\t\"RememberPassword\"\t\t\"1\"\n"
    "\t\t\"WantsOfflineMode\"\t\t\"0\"\n\t\t\"SkipOfflineModeWarning\"\t\t\"0\"\n\t\t\"AllowAutoLogin\"\t\t\"1\"\n\t\t\"MostRecent\"\t\t\"1\"\n\t\t\"Timestamp\"\t\t\"1730000000\"\n\t}\n"
    "\t\"76561198000000002\"\n\t{\n\t\t\"AccountName\"\t\t\"smurf\"\n\t\t\"PersonaName\"\t\t\"Смурф\"\n\t\t\"RememberPassword\"\t\t\"1\"\n"
    "\t\t\"WantsOfflineMode\"\t\t\"0\"\n\t\t\"SkipOfflineModeWarning\"\t\t\"0\"\n\t\t\"AllowAutoLogin\"\t\t\"0\"\n\t\t\"MostRecent\"\t\t\"0\"\n\t\t\"Timestamp\"\t\t\"1720000000\"\n\t}\n"
    "}\n";

static std::string read(const std::string& p) { std::string s; fsx::read_file(p, s); return s; }

static void start_fake(const std::string& exe) {
    os::launch(exe, "--fake-running", fsx::parent(exe), false);
    for (int i = 0; i < 50 && !os::any_running({fsx::filename(exe)}); ++i) Sleep(50);
}

static void test_steam(const std::string& fake) {
    std::string steam = fsx::join(g_root, "Steam");
    fsx::remove_all(steam);
    fsx::mkdirs(fsx::join(steam, "config"));
    fsx::mkdirs(fsx::join(steam, "userdata\\39734274\\config"));
    fsx::mkdirs(fsx::join(steam, "steamapps"));
    fsx::copy_any(fake, fsx::join(steam, "steam.exe"));
    fsx::write_file(fsx::join(steam, "config\\loginusers.vdf"), kLU);
    fsx::write_file(fsx::join(steam, "userdata\\39734274\\config\\localconfig.vdf"),
                    "\"UserLocalConfigStore\"\n{\n\t\"WebStorage\"\n\t{\n\t\t\"FriendStoreLocalPrefs_39734274\"\t\t\"{\\\"ePersonaState\\\":1,\\\"strNonFriendsAllowedToMsg\\\":\\\"\\\"}\"\n\t}\n}\n");
    fsx::write_file(fsx::join(steam, "steamapps\\appmanifest_730.acf"), "\"AppState\"\n{\n\t\"appid\"\t\t\"730\"\n\t\"name\"\t\t\"Counter-Strike 2\"\n}\n");

    Store st;
    st.load(fsx::join(g_root, "data"));
    st.settings.steam_path = steam;
    st.settings.steam_close = SteamClose::Fast;
    st.settings.steam_args = "-silent";
    SteamPlatform sp(st);
    CHECK(iequals(sp.root(), steam));
    CHECK(sp.installed());
    auto accs = sp.scan();
    CHECK(accs.size() == 2);
    CHECK(accs.size() == 2 && accs[0].current && !accs[1].current);
    CHECK(sp.games().size() == 1 && sp.games()[0].name == "Counter-Strike 2");

    // Steam "running"
    start_fake(fsx::join(steam, "steam.exe"));
    CHECK(os::any_running({"steam.exe"}));
    fsx::remove_file(fsx::join(steam, "launched.txt"));

    SwitchRequest r;
    r.account_id = "76561198000000002";
    r.state = steam::Invisible;
    r.app_id = "730";
    DWORD t0 = GetTickCount();
    OpResult res = sp.switch_to(r, [](const std::string& s) { std::printf("  step: %s\n", s.c_str()); });
    DWORD dt = GetTickCount() - t0;
    std::printf("  switch: ok=%d msg='%s' %lu ms\n", res.ok, res.message.c_str(), (unsigned long)dt);
    CHECK(res.ok);
    Sleep(400);
    std::vector<steam::LoginUser> u;
    CHECK(steam::parse_loginusers(read(fsx::join(steam, "config\\loginusers.vdf")), u));
    CHECK(u.size() == 2 && steam::active_id64(u) == "76561198000000002");
    CHECK(u.size() == 2 && u[1].remember && u[1].allow_autologin && !u[1].offline);
    std::string v;
    CHECK(os::reg_get_string("HKCU\\Software\\Valve\\Steam", "AutoLoginUser", v) && v == "smurf");
    uint32_t rp = 0;
    CHECK(os::reg_get_dword("HKCU\\Software\\Valve\\Steam", "RememberPassword", rp) && rp == 1);
    CHECK(steam::read_persona_state(read(fsx::join(steam, "userdata\\39734274\\config\\localconfig.vdf")), 39734274) == 7);
    std::string launched = read(fsx::join(steam, "launched.txt"));
    std::printf("  launched.txt: %s", launched.c_str());
    CHECK(launched.find("-shutdown") != std::string::npos);            // graceful attempt first
    CHECK(launched.find("-silent -applaunch 730") != std::string::npos); // relaunch with args
    CHECK(fsx::exists(fsx::join(steam, "config\\loginusers.vdf.vs-backup")));
    CHECK(sp.scan().size() == 2 && sp.current_account_id() == "76561198000000002");

    // instant method + offline mode + new login screen
    st.settings.steam_close = SteamClose::Instant;
    SwitchRequest r2;
    r2.account_id = "76561198000000001";
    r2.state = kStateOfflineMode;
    t0 = GetTickCount();
    res = sp.switch_to(r2, nullptr);
    std::printf("  instant switch: ok=%d %lu ms\n", res.ok, (unsigned long)(GetTickCount() - t0));
    CHECK(res.ok);
    CHECK(steam::parse_loginusers(read(fsx::join(steam, "config\\loginusers.vdf")), u));
    CHECK(u.size() == 2 && u[0].offline && u[0].most_recent);
    CHECK(os::reg_get_string("HKCU\\Software\\Valve\\Steam", "AutoLoginUser", v) && v == "main_acc");

    res = sp.switch_to(SwitchRequest{}, nullptr);
    CHECK(res.ok);
    CHECK(!os::reg_get_string("HKCU\\Software\\Valve\\Steam", "AutoLoginUser", v) || v.empty());
    CHECK(steam::parse_loginusers(read(fsx::join(steam, "config\\loginusers.vdf")), u));
    CHECK(steam::active_id64(u).empty());

    // forget
    res = sp.forget("76561198000000002");
    CHECK(res.ok);
    CHECK(sp.scan().size() == 1);
    // the row is gone from loginusers too
    CHECK(steam::parse_loginusers(read(fsx::join(steam, "config\\loginusers.vdf")), u) && u.size() == 1);

    // damaged loginusers.vdf (and no usable backup) must never be overwritten
    fsx::remove_file(fsx::join(steam, "config\\loginusers.vdf.vs-backup"));
    fsx::write_file(fsx::join(steam, "config\\loginusers.vdf"), "\"users\" { \"7656\" { \"AccountName\" ");
    SwitchRequest r3;
    r3.account_id = "76561198000000001";
    res = sp.switch_to(r3, nullptr);
    CHECK(!res.ok);
    CHECK(read(fsx::join(steam, "config\\loginusers.vdf")) == "\"users\" { \"7656\" { \"AccountName\" ");

    os::kill_processes({"steam.exe"}, 3000);
    CHECK(!os::any_running({"steam.exe"}));
}

#include "test_win_guard.inc"
#include "test_win_tools.inc"
#include "../src/app/steam_helper.h"

extern void test_basic(const std::string& root, const std::string& fake);  // defined below
extern void test_epic(const std::string& root, const std::string& fake);

int wmain(int argc, wchar_t** argv) {
    // child processes of the achievements / farm test
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow(argv[i]));
    int helper_rc = 0;
    if (steam_helper_main(args, helper_rc)) return helper_rc;
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = fsx::join(narrow(tmp), "vs-wintest");
    fsx::remove_all(g_root);
    fsx::mkdirs(g_root);
    std::string fake = fsx::join(fsx::parent(os::self_exe_path()), "fake_client.exe");
    if (!fsx::exists(fake)) { std::printf("fake_client.exe missing\n"); return 2; }
    std::printf("== steam\n");
    test_steam(fake);
    std::printf("== basic\n");
    test_basic(g_root, fake);
    std::printf("== epic (new launcher)\n");
    test_epic(g_root, fake);
    std::printf("== steam guard storage\n");
    test_guard_store(g_root);
    std::printf("== achievements and farm\n");
    test_tools(g_root);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// ------------------------------------------------------------------ basic platform
#include "test_win_basic.inc"
