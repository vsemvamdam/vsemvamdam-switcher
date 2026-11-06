// Steam protocol smoke test without any account (run in CI on Windows; network needed).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

#include "../src/app/guard.h"
#include "../src/core/cards.h"
#include "../src/os/win.h"

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> lines;
    bool ok = vs::Guard::self_test(lines);
    // farm: a public badges page seen without signing in, and Steam Achievement Manager's game list
    {
        std::string body;
        int status = 0;
        bool net = vs::os::http_get("https://steamcommunity.com/profiles/76561197960287930/badges/?l=english", body, &status, 20000);
        vs::cards::BadgePage bp;
        vs::cards::parse_badges(body, bp);
        lines.push_back("badges (no session): net=" + std::string(net ? "ok" : "FAIL") + " http=" + std::to_string(status) +
                        " body=" + std::to_string(body.size()) + "B signed_in=" + (bp.signed_in ? "yes" : "no") + " rows=" +
                        std::to_string(bp.games.size()) + " last_page=" + std::to_string(bp.last_page) + " (expected: signed_in=no)");
        if (!net || bp.signed_in) ok = false;
        body.clear();
        net = vs::os::http_get("https://gib.me/sam/games.xml", body, &status, 20000);
        size_t n = vs::cards::parse_sam_games_xml(body).size();
        lines.push_back("SAM games.xml: net=" + std::string(net ? "ok" : "FAIL") + " http=" + std::to_string(status) + " apps=" + std::to_string(n));
        if (n < 1000) ok = false;
    }
    for (auto& l : lines) std::printf("  %s\n", l.c_str());
    std::printf("steam protocol smoke test: %s\n", ok ? "OK" : "PROBLEM");
    return ok ? 0 : 1;
}
