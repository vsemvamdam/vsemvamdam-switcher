// Achievements and farm end to end: SteamTools -> "--steam-helper" child processes (this exe) -> fake steamclient64.dll
#include "../src/app/steam_tools.h"

namespace kvw {
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
};
}  // namespace kvw

template <class F>
static bool wait_until(F f, int ms) {
    for (int i = 0; i < ms / 50; ++i) {
        if (f()) return true;
        Sleep(50);
    }
    return f();
}

static void test_tools(const std::string& root) {
    std::string dll = fsx::join(fsx::parent(os::self_exe_path()), "steamclient64.dll");
    if (!fsx::exists(dll)) {
        std::printf("steamclient64.dll (fake) missing\n");
        CHECK(false);
        return;
    }
    std::string base = fsx::join(root, "tools");
    fsx::remove_all(base);
    std::string steam = fsx::join(base, "Steam");
    fsx::mkdirs(fsx::join(steam, "appcache\\stats"));
    fsx::mkdirs(fsx::join(steam, "appcache\\librarycache\\400"));
    fsx::mkdirs(fsx::join(steam, "appcache\\librarycache\\10"));
    fsx::mkdirs(fsx::join(steam, "appcache\\librarycache\\777"));  // not owned
    CHECK(CopyFileW(widen(dll).c_str(), widen(fsx::join(steam, "steamclient64.dll")).c_str(), FALSE));
    kvw::B b;
    b.obj("400")
        .str("gamename", "Portal")
        .obj("stats")
            .obj("1").str("type", "ACHIEVEMENTS").i32("id", 1)
                .obj("bits")
                    .obj("0").str("name", "ACH_A").i32("permission", 0)
                        .obj("display").obj("name").str("english", "First").end().obj("desc").str("english", "Do the first thing").end().end()
                    .end()
                    .obj("1").str("name", "ACH_B").i32("permission", 0)
                        .obj("display").obj("name").str("english", "Second").end().end()
                    .end()
                    .obj("2").str("name", "ACH_PROT").i32("permission", 2)
                        .obj("display").obj("name").str("english", "Server only").end().end()
                    .end()
                .end()
            .end()
            .obj("2").str("type", "INT").str("name", "kills").i32("max", 100000).obj("display").str("name", "Kills").end().end()
            .obj("3").str("type", "FLOAT").str("name", "distance").obj("display").str("name", "Distance").end().end()
        .end()
    .end()
    .end();
    fsx::write_file(fsx::join(steam, "appcache\\stats\\UserGameStatsSchema_400.bin"), b.d);
    fsx::write_file(fsx::join(steam, "fake_state.txt"), "A 400 ACH_A 1 1690000000\nS 400 kills 7\n");

    Store st;
    st.load(fsx::join(base, "data"));
    Guard guard(st);
    {
        SteamTools t(st, guard, [steam] { return steam; });
        // no network in the test: a fresh cached copy of SAM's game list
        fsx::mkdirs(t.dir());
        fsx::write_file(fsx::join(t.dir(), "sam_games.xml"),
                        "<games><game type=\"normal\">400</game><game type=\"normal\">999</game><game type=\"junk\">10</game></games>");

        // ---- library from this PC
        t.load_library(false);
        CHECK(wait_until([&] { return !t.library().loading; }, 30000));
        LibraryView lib = t.library();
        CHECK(lib.loaded);
        CHECK(lib.error.empty());
        if (!lib.error.empty()) std::printf("  library error: %s\n", lib.error.c_str());
        CHECK(lib.steamid == 76561198000000001ULL);
        CHECK(!lib.from_web);
        CHECK(lib.games.size() == 2);  // 10 and 400 (777 and 999 are not owned)
        bool g400 = false, g10 = false;
        for (auto& g : lib.games) {
            if (g.appid == 400) g400 = g.name == "Game 400" && g.has_stats;
            if (g.appid == 10) g10 = g.name == "Game 10" && !g.has_stats;
        }
        CHECK(g400);
        CHECK(g10);
        std::printf("  library: %d game(s) from steamid %llu\n", (int)lib.games.size(), (unsigned long long)lib.steamid);

        // ---- achievements and stats
        t.load_stats(400);
        CHECK(wait_until([&] { return !t.stats_view().loading; }, 30000));
        StatsView sv = t.stats_view();
        if (!sv.error.empty()) std::printf("  stats error: %s\n", sv.error.c_str());
        CHECK(sv.error.empty());
        CHECK(sv.data.achievements.size() == 3);
        CHECK(sv.data.stats.size() == 2);
        if (sv.data.achievements.size() == 3) {
            CHECK(sv.data.achievements[0].def.id == "ACH_A" && sv.data.achievements[0].achieved && sv.data.achievements[0].unlock_time == 1690000000u);
            CHECK(sv.data.achievements[0].def.name == "First" && sv.data.achievements[0].def.desc == "Do the first thing");
            CHECK(!sv.data.achievements[1].achieved);
            CHECK(stats::is_protected(sv.data.achievements[2].def.permission));
        }
        if (sv.data.stats.size() == 2) {
            CHECK(sv.data.stats[0].def.id == "kills" && sv.data.stats[0].value == 7);
            CHECK(sv.data.stats[1].def.type == stats::StatFloat);
        }

        stats::Changes ch;
        ch.achievements["ACH_A"] = false;
        ch.achievements["ACH_B"] = true;
        ch.achievements["ACH_PROT"] = true;  // must be skipped
        ch.stats["kills"] = 42;
        ch.stats["distance"] = 12.5;
        t.apply(400, ch);
        CHECK(wait_until([&] { auto v = t.stats_view(); return !v.applying && !v.loading; }, 60000));
        sv = t.stats_view();
        CHECK(!sv.result_error);
        if (sv.result_error) std::printf("  apply error: %s\n", sv.result.c_str());
        std::string state = read(fsx::join(steam, "fake_state.txt"));
        std::printf("  apply: %s\n", sv.result.c_str());
        CHECK(state.find("A 400 ACH_A 0") != std::string::npos);
        CHECK(state.find("A 400 ACH_B 1") != std::string::npos);
        CHECK(state.find("ACH_PROT") == std::string::npos);
        CHECK(state.find("S 400 kills 42") != std::string::npos);
        CHECK(state.find("S 400 distance 12.5") != std::string::npos);
        // the view is re-read from Steam after saving
        if (sv.data.achievements.size() == 3) {
            CHECK(!sv.data.achievements[0].achieved);
            CHECK(sv.data.achievements[1].achieved);
            CHECK(!sv.data.achievements[2].achieved);
        }
        if (sv.data.stats.size() == 2) CHECK(sv.data.stats[0].value == 42);

        // the values on screen belong to one account: saving them while Steam has another one is refused
        SetEnvironmentVariableW(L"FAKE_STEAMID", L"76561198000000002");
        stats::Changes other;
        other.achievements["ACH_A"] = true;
        t.apply(400, other);
        CHECK(wait_until([&] { auto v = t.stats_view(); return !v.applying && !v.loading; }, 60000));
        CHECK(t.stats_view().result_error && t.stats_view().result == SteamTools::helper_error_text("wrong_account"));
        CHECK(read(fsx::join(steam, "fake_state.txt")).find("A 400 ACH_A 0") != std::string::npos);
        SetEnvironmentVariableW(L"FAKE_STEAMID", nullptr);
        // a save for a game that is not on screen is ignored
        t.apply(10, other);
        CHECK(!t.stats_view().applying);

        stats::Changes reset;
        reset.reset_all = true;
        reset.reset_with_achievements = true;
        t.apply(400, reset);
        CHECK(wait_until([&] { auto v = t.stats_view(); return !v.applying && !v.loading; }, 60000));
        state = read(fsx::join(steam, "fake_state.txt"));
        CHECK(state.find("S 400 kills 0") != std::string::npos);
        CHECK(state.find("A 400 ACH_B 0") != std::string::npos);

        // a game without a schema
        t.load_stats(10);
        CHECK(wait_until([&] { return !t.stats_view().loading; }, 30000));
        CHECK(t.stats_view().error == SteamTools::helper_error_text("no_schema"));

        // ---- hours farm: one helper process per game
        t.start_hours({400, 10}, 0);
        CHECK(t.farming());
        CHECK(wait_until([&] {
            auto f = t.farm_view();
            return f.running.size() == 2 && f.running[0].ok && f.running[1].ok;
        }, 30000));
        FarmView fv = t.farm_view();
        std::printf("  hours farm: %d helper(s) running\n", (int)fv.running.size());
        CHECK(fv.kind == FarmKind::Hours && fv.active);
        CHECK(fv.steamid == 76561198000000001ULL);
        CHECK(fv.error.empty());
        Sleep(1500);  // the helpers stay alive
        fv = t.farm_view();
        CHECK(fv.running.size() == 2 && fv.running[0].ok && fv.running[1].ok);
        auto t0 = GetTickCount64();
        t.stop_farm();
        CHECK(GetTickCount64() - t0 < 1000);  // never waits for the farm thread
        CHECK(!t.farming());
        CHECK(t.farm_view().running.empty());

        // a new farm replaces the running one
        t.start_hours({620}, 0);
        CHECK(wait_until([&] { auto f = t.farm_view(); return f.running.size() == 1 && f.running[0].ok; }, 30000));
        t.stop_farm();

        // ---- cards need a web session
        t.start_cards(cards::Mode::Auto);
        CHECK(wait_until([&] { return !t.farming(); }, 30000));
        CHECK(t.farm_view().need_login);

        // ---- Steam is not running
        SetEnvironmentVariableW(L"FAKE_STEAM_OFF", L"1");
        t.load_stats(400);
        CHECK(wait_until([&] { return !t.stats_view().loading; }, 30000));
        CHECK(t.stats_view().error == SteamTools::helper_error_text("not_running"));
        t.start_hours({400}, 0);
        CHECK(wait_until([&] { return !t.farming(); }, 30000));
        CHECK(t.farm_view().error == SteamTools::helper_error_text("not_running"));
        SetEnvironmentVariableW(L"FAKE_STEAM_OFF", nullptr);
        t.stop();
    }
    // helper scratch files are cleaned up
    int left = 0;
    for (auto& f : fsx::list_dir(fsx::join(fsx::join(base, "data"), "tools\\run")))
        if (f.find(".out.json") != std::string::npos || f.find(".in") != std::string::npos) ++left;
    CHECK(left == 0);
}
