// Fake steamclient64.dll for the integration tests: the same exports and vtable slots the switcher uses
// (see src/os/steamclient.cpp), backed by a small state file next to the DLL so that changes made by one
// helper process are visible to the next one.
//
//   FAKE_STEAM_OFF=1      -> CreateSteamPipe fails ("Steam is not running")
//   subscribed apps       -> 10, 400, 620
//   state                 -> <dll dir>\fake_state.txt  ("A <app> <id> <0|1> <time>" / "S <app> <id> <value>")
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {
HMODULE g_self = nullptr;
std::mutex g_mtx;
const uint64_t kSteamId = 76561198000000001ULL;

struct Msg {
    int id;
    std::vector<uint8_t> data;
};
std::deque<Msg> g_queue;
std::vector<uint8_t> g_current;

struct CallbackMsg {
    int32_t user;
    int32_t id;
    uint8_t* param;
    int32_t size;
};

uint32_t app_id() {
    char b[32] = {0};
    if (!GetEnvironmentVariableA("SteamAppId", b, sizeof(b))) return 0;
    return (uint32_t)std::strtoul(b, nullptr, 10);
}

std::string state_path() {
    char p[MAX_PATH] = {0};
    GetModuleFileNameA(g_self, p, MAX_PATH);
    std::string s = p;
    return s.substr(0, s.find_last_of("\\/") + 1) + "fake_state.txt";
}

struct State {
    std::map<std::string, std::pair<int, uint32_t>> ach;  // "<app>/<id>" -> achieved, time
    std::map<std::string, double> stat;
};
State g_state;

void load_state() {
    g_state = State{};
    std::ifstream f(state_path());
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string kind, app, id;
        is >> kind >> app >> id;
        if (kind == "A") {
            int on = 0;
            uint32_t t = 0;
            is >> on >> t;
            g_state.ach[app + "/" + id] = {on, t};
        } else if (kind == "S") {
            double v = 0;
            is >> v;
            g_state.stat[app + "/" + id] = v;
        }
    }
}

void save_state() {
    std::ofstream f(state_path(), std::ios::trunc);
    for (auto& [k, v] : g_state.ach) {
        auto s = k.find('/');
        f << "A " << k.substr(0, s) << " " << k.substr(s + 1) << " " << v.first << " " << v.second << "\n";
    }
    for (auto& [k, v] : g_state.stat) {
        auto s = k.find('/');
        f << "S " << k.substr(0, s) << " " << k.substr(s + 1) << " " << v << "\n";
    }
}

std::string key(const char* id) { return std::to_string(app_id()) + "/" + id; }

void push(int id, uint64_t game, int32_t result) {
    Msg m;
    m.id = id;
    m.data.resize(24);
    std::memcpy(m.data.data(), &game, 8);
    std::memcpy(m.data.data() + 8, &result, 4);
    std::memcpy(m.data.data() + 16, &kSteamId, 8);
    g_queue.push_back(m);
}

bool known_ach(const char* id) { return std::strncmp(id, "MISSING", 7) != 0; }
bool float_stat(const char* id) {
    size_t n = std::strlen(id);
    return !std::strcmp(id, "distance") || (n > 2 && !std::strcmp(id + n - 2, "_f"));
}

// optional "<dll dir>\fake_apps.txt": "lang <language>" and "<appid> <name>" lines (otherwise apps 10, 400, 620)
std::map<uint32_t, std::string> g_apps;
std::string g_lang = "english";
void load_apps() {
    std::string p = state_path();
    p = p.substr(0, p.find_last_of("\\/") + 1) + "fake_apps.txt";
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("lang ", 0) == 0) { g_lang = line.substr(5); continue; }
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        g_apps[(uint32_t)std::strtoul(line.c_str(), nullptr, 10)] = line.substr(sp + 1);
    }
    if (g_apps.empty()) g_apps = {{10, "Game 10"}, {400, "Game 400"}, {620, "Game 620"}};
}

// ---- ISteamClient018
int c_create_pipe(void*) { return GetEnvironmentVariableA("FAKE_STEAM_OFF", nullptr, 0) ? 0 : 1; }
bool c_release_pipe(void*, int) { return true; }
int c_connect(void*, int) { return 1; }
void c_release_user(void*, int, int) {}
void* c_get_user(void*, int, int, const char*);
void* c_get_utils(void*, int, const char*);
void* c_get_stats(void*, int, int, const char*);
void* c_get_apps(void*, int, int, const char* ver);
void* c_unused(void*) { return nullptr; }

// ---- ISteamUser012
int u_hsteamuser(void*) { return 1; }
bool u_logged_on(void*) { return true; }
void* u_steam_id(void*, uint64_t* out) { *out = kSteamId; return out; }

// ---- ISteamUtils005
uint32_t ut_app_id(void*) { return app_id(); }

// ---- ISteamApps008 / 001
bool a_subscribed(void*, uint32_t app) { return g_apps.count(app) > 0; }
const char* a_language(void*) { return g_lang.c_str(); }
int a_app_data(void*, uint32_t app, const char* k, char* buf, int n) {
    if (std::strcmp(k, "name") || !a_subscribed(nullptr, app)) return 0;
    return std::snprintf(buf, n, "%s", g_apps[app].c_str()) + 1;
}

// ---- ISteamUserStats013
bool s_get_float(void*, const char* id, float* v) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!float_stat(id)) return false;
    load_state();
    *v = (float)g_state.stat[key(id)];
    return true;
}
bool s_get_int(void*, const char* id, int32_t* v) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (float_stat(id)) return false;
    load_state();
    *v = (int32_t)g_state.stat[key(id)];
    return true;
}
bool s_set_float(void*, const char* id, float v) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!float_stat(id)) return false;
    g_state.stat[key(id)] = v;
    return true;
}
bool s_set_int(void*, const char* id, int32_t v) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (float_stat(id)) return false;
    g_state.stat[key(id)] = v;
    return true;
}
bool s_set_ach(void*, const char* id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!known_ach(id)) return false;
    g_state.ach[key(id)] = {1, 1700000000u};
    return true;
}
bool s_clear_ach(void*, const char* id) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!known_ach(id)) return false;
    g_state.ach[key(id)] = {0, 0};
    return true;
}
bool s_get_ach_time(void*, const char* id, bool* on, uint32_t* t) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!known_ach(id)) return false;
    load_state();
    auto it = g_state.ach.find(key(id));
    *on = it != g_state.ach.end() && it->second.first;
    *t = it != g_state.ach.end() ? it->second.second : 0;
    return true;
}
bool s_store(void*) {
    std::lock_guard<std::mutex> lk(g_mtx);
    save_state();
    push(1102, app_id(), 1);
    return true;
}
uint64_t s_request_user(void*, uint64_t sid) {
    std::lock_guard<std::mutex> lk(g_mtx);
    load_state();  // the base for the following Set* calls
    push(1101, app_id(), sid == kSteamId ? 1 : 2);
    return 1234;
}
bool s_reset(void*, bool with_ach) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string pre = std::to_string(app_id()) + "/";
    for (auto& [k, v] : g_state.stat)
        if (k.rfind(pre, 0) == 0) v = 0;
    if (with_ach)
        for (auto& [k, v] : g_state.ach)
            if (k.rfind(pre, 0) == 0) v = {0, 0};
    return true;
}
bool s_stub(void*) { return false; }

struct Obj {
    void** vtbl;
};
void* g_client_vt[40];
void* g_user_vt[10];
void* g_utils_vt[12];
void* g_apps8_vt[10];
void* g_apps1_vt[4];
void* g_stats_vt[30];
Obj g_client{g_client_vt}, g_user{g_user_vt}, g_utils{g_utils_vt}, g_apps8{g_apps8_vt}, g_apps1{g_apps1_vt}, g_stats{g_stats_vt};

void* c_get_user(void*, int, int, const char* v) { return std::strcmp(v, "SteamUser012") ? nullptr : &g_user; }
void* c_get_utils(void*, int, const char* v) { return std::strcmp(v, "SteamUtils005") ? nullptr : &g_utils; }
void* c_get_stats(void*, int, int, const char* v) { return std::strcmp(v, "STEAMUSERSTATS_INTERFACE_VERSION013") ? nullptr : &g_stats; }
void* c_get_apps(void*, int, int, const char* v) {
    if (!std::strcmp(v, "STEAMAPPS_INTERFACE_VERSION008")) return &g_apps8;
    if (!std::strcmp(v, "STEAMAPPS_INTERFACE_VERSION001")) return &g_apps1;
    return nullptr;
}

void build() {
    for (auto& p : g_client_vt) p = (void*)c_unused;
    for (auto& p : g_user_vt) p = (void*)s_stub;
    for (auto& p : g_utils_vt) p = (void*)s_stub;
    for (auto& p : g_apps8_vt) p = (void*)s_stub;
    for (auto& p : g_apps1_vt) p = (void*)s_stub;
    for (auto& p : g_stats_vt) p = (void*)s_stub;
    g_client_vt[0] = (void*)c_create_pipe;
    g_client_vt[1] = (void*)c_release_pipe;
    g_client_vt[2] = (void*)c_connect;
    g_client_vt[4] = (void*)c_release_user;
    g_client_vt[5] = (void*)c_get_user;
    g_client_vt[9] = (void*)c_get_utils;
    g_client_vt[13] = (void*)c_get_stats;
    g_client_vt[15] = (void*)c_get_apps;
    g_user_vt[0] = (void*)u_hsteamuser;
    g_user_vt[1] = (void*)u_logged_on;
    g_user_vt[2] = (void*)u_steam_id;
    g_utils_vt[9] = (void*)ut_app_id;
    g_apps8_vt[4] = (void*)a_language;
    g_apps8_vt[6] = (void*)a_subscribed;
    g_apps1_vt[0] = (void*)a_app_data;
    g_stats_vt[0] = (void*)s_get_float;
    g_stats_vt[1] = (void*)s_get_int;
    g_stats_vt[2] = (void*)s_set_float;
    g_stats_vt[3] = (void*)s_set_int;
    g_stats_vt[6] = (void*)s_set_ach;
    g_stats_vt[7] = (void*)s_clear_ach;
    g_stats_vt[8] = (void*)s_get_ach_time;
    g_stats_vt[9] = (void*)s_store;
    g_stats_vt[15] = (void*)s_request_user;
    g_stats_vt[20] = (void*)s_reset;
}
}  // namespace

extern "C" __declspec(dllexport) void* CreateInterface(const char* ver, int* rc) {
    if (rc) *rc = 0;
    if (!std::strcmp(ver, "SteamClient018")) return &g_client;
    if (rc) *rc = 1;
    return nullptr;
}

extern "C" __declspec(dllexport) bool Steam_BGetCallback(int pipe, CallbackMsg* msg, int* call) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (pipe != 1 || g_queue.empty()) return false;
    g_current = g_queue.front().data;
    msg->user = 1;
    msg->id = g_queue.front().id;
    msg->param = g_current.data();
    msg->size = (int32_t)g_current.size();
    if (call) *call = 0;
    return true;
}

extern "C" __declspec(dllexport) bool Steam_FreeLastCallback(int) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_queue.empty()) g_queue.pop_front();
    return true;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        build();
        load_apps();
    }
    return TRUE;
}
