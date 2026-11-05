// Fake platform client for integration tests: records its command line, then idles until killed.
#include <windows.h>
#include <cstdio>
#include <string>
int main(int argc, char** argv) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    dir = dir.substr(0, dir.find_last_of("\\/"));
    std::string args;
    for (int i = 1; i < argc; ++i) { if (i > 1) args += ' '; args += argv[i]; }
    FILE* f = fopen((dir + "\\launched.txt").c_str(), "a");
    if (f) { fprintf(f, "%s\n", args.c_str()); fclose(f); }
    if (args.find("-shutdown") != std::string::npos) return 0;  // pretend to forward shutdown
    for (;;) Sleep(1000);
}
