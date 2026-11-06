// Steam protocol smoke test without any account (run in CI on Windows; network needed).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

#include "../src/app/guard.h"

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> lines;
    bool ok = vs::Guard::self_test(lines);
    for (auto& l : lines) std::printf("  %s\n", l.c_str());
    std::printf("steam protocol smoke test: %s\n", ok ? "OK" : "PROBLEM");
    return ok ? 0 : 1;
}
