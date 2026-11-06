#include "steamclient.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "../core/fsx.h"
#include "../core/str.h"
#include "win.h"

namespace vs::steamc {

// vtable slots, in the order the Steam client exports them (MSVC layout, as mapped by Steam Achievement Manager)
namespace slot {
// ISteamClient018
constexpr int CreateSteamPipe = 0, ReleaseSteamPipe = 1, ConnectToGlobalUser = 2, ReleaseUser = 4, GetISteamUser = 5, GetISteamUtils = 9,
              GetISteamUserStats = 13, GetISteamApps = 15;
// ISteamUser012
constexpr int LoggedOn = 1, GetSteamID = 2;
// ISteamUserStats013 (overloads are reversed by MSVC: float before int)
constexpr int GetStatFloat = 0, GetStatInt = 1, SetStatFloat = 2, SetStatInt = 3, SetAchievement = 6, ClearAchievement = 7,
              GetAchievementAndUnlockTime = 8, StoreStats = 9, RequestUserStats = 15, ResetAllStats = 20;
// ISteamUtils005
constexpr int GetAppID = 9;
// ISteamApps008
constexpr int GetCurrentGameLanguage = 4, IsSubscribedApp = 6;
// ISteamApps001
constexpr int GetAppData = 0;
}  // namespace slot

template <class Fn>
static Fn vfn(void* obj, int index) {
    void** vtbl = *(void***)obj;
    return (Fn)vtbl[index];
}

struct CallbackMsg {  // CallbackMsg_t, 8-byte packing on Windows
    int32_t user;
    int32_t id;
    uint8_t* param;
    int32_t size;
};

const char* init_error_code(InitError e) {
    switch (e) {
        case InitError::None: return "ok";
        case InitError::NoSteam: return "no_steam";
        case InitError::NoExports: return "no_exports";
        case InitError::NoClient: return "no_client";
        case InitError::NotRunning: return "not_running";
        case InitError::NotLoggedIn: return "not_logged_in";
        case InitError::AppMismatch: return "app_mismatch";
        case InitError::NoInterfaces: return "no_interfaces";
    }
    return "error";
}

std::string Client::default_steam_root() {
    std::string p;
    if (os::reg_get_string("HKCU\\Software\\Valve\\Steam", "SteamPath", p) && !p.empty()) return replace_all(p, "/", "\\");
    if (os::reg_get_string("HKLM\\Software\\Valve\\Steam", "InstallPath", p, os::RegView::Wow64_32) && !p.empty()) return p;
    if (os::reg_get_string("HKLM\\Software\\Valve\\Steam", "InstallPath", p) && !p.empty()) return p;
    return {};
}

InitError Client::init(const std::string& root_in, uint32_t appid) {
    shutdown();
    std::string root = root_in.empty() ? default_steam_root() : root_in;
    std::string dll = fsx::join(root, "steamclient64.dll");
    if (root.empty() || !fsx::exists(dll)) return InitError::NoSteam;
    if (appid) SetEnvironmentVariableW(L"SteamAppId", widen(std::to_string(appid)).c_str());
    SetDllDirectoryW(widen(root).c_str());
    HMODULE m = LoadLibraryExW(widen(dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return InitError::NoSteam;
    dll_ = m;
    create_interface_ = (void* (*)(const char*, int*))(void*)GetProcAddress(m, "CreateInterface");
    get_callback_ = (bool (*)(int, void*, int*))(void*)GetProcAddress(m, "Steam_BGetCallback");
    free_callback_ = (bool (*)(int))(void*)GetProcAddress(m, "Steam_FreeLastCallback");
    if (!create_interface_ || !get_callback_ || !free_callback_) return InitError::NoExports;
    client_ = create_interface_("SteamClient018", nullptr);
    if (!client_) return InitError::NoClient;
    pipe_ = vfn<int (*)(void*)>(client_, slot::CreateSteamPipe)(client_);
    if (!pipe_) return InitError::NotRunning;
    user_ = vfn<int (*)(void*, int)>(client_, slot::ConnectToGlobalUser)(client_, pipe_);
    if (!user_) return InitError::NotLoggedIn;
    utils_ = vfn<void* (*)(void*, int, const char*)>(client_, slot::GetISteamUtils)(client_, pipe_, "SteamUtils005");
    user_iface_ = vfn<void* (*)(void*, int, int, const char*)>(client_, slot::GetISteamUser)(client_, user_, pipe_, "SteamUser012");
    stats_ = vfn<void* (*)(void*, int, int, const char*)>(client_, slot::GetISteamUserStats)(client_, user_, pipe_,
                                                                                            "STEAMUSERSTATS_INTERFACE_VERSION013");
    apps1_ = vfn<void* (*)(void*, int, int, const char*)>(client_, slot::GetISteamApps)(client_, user_, pipe_, "STEAMAPPS_INTERFACE_VERSION001");
    apps8_ = vfn<void* (*)(void*, int, int, const char*)>(client_, slot::GetISteamApps)(client_, user_, pipe_, "STEAMAPPS_INTERFACE_VERSION008");
    if (!utils_ || !user_iface_ || !stats_ || !apps1_ || !apps8_) return InitError::NoInterfaces;
    if (appid && app_id() != appid) return InitError::AppMismatch;
    return InitError::None;
}

void Client::shutdown() {
    if (client_ && pipe_) {
        if (user_) vfn<void (*)(void*, int, int)>(client_, slot::ReleaseUser)(client_, pipe_, user_);
        vfn<bool (*)(void*, int)>(client_, slot::ReleaseSteamPipe)(client_, pipe_);
    }
    pipe_ = user_ = 0;
    client_ = user_iface_ = stats_ = utils_ = apps1_ = apps8_ = nullptr;
    // the module stays loaded: steamclient does not like being unloaded while its threads run
}

uint64_t Client::steam_id() {
    if (!user_iface_) return 0;
    uint64_t id = 0;
    vfn<void* (*)(void*, uint64_t*)>(user_iface_, slot::GetSteamID)(user_iface_, &id);  // CSteamID returned via hidden pointer
    return id;
}
bool Client::logged_on() { return user_iface_ && vfn<bool (*)(void*)>(user_iface_, slot::LoggedOn)(user_iface_); }
uint32_t Client::app_id() { return utils_ ? vfn<uint32_t (*)(void*)>(utils_, slot::GetAppID)(utils_) : 0; }
std::string Client::current_game_language() {
    if (!apps8_) return {};
    const char* s = vfn<const char* (*)(void*)>(apps8_, slot::GetCurrentGameLanguage)(apps8_);
    return s ? std::string(s) : std::string();
}
bool Client::is_subscribed_app(uint32_t appid) { return apps8_ && vfn<bool (*)(void*, uint32_t)>(apps8_, slot::IsSubscribedApp)(apps8_, appid); }
std::string Client::app_data(uint32_t appid, const char* key) {
    if (!apps1_) return {};
    char buf[1024] = {0};
    int n = vfn<int (*)(void*, uint32_t, const char*, char*, int)>(apps1_, slot::GetAppData)(apps1_, appid, key, buf, (int)sizeof(buf));
    if (n <= 0) return {};
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

uint64_t Client::request_user_stats(uint64_t steamid) {
    return stats_ ? vfn<uint64_t (*)(void*, uint64_t)>(stats_, slot::RequestUserStats)(stats_, steamid) : 0;
}
bool Client::get_achievement(const std::string& id, bool& achieved, uint32_t& t) {
    achieved = false;
    t = 0;
    return stats_ && vfn<bool (*)(void*, const char*, bool*, uint32_t*)>(stats_, slot::GetAchievementAndUnlockTime)(stats_, id.c_str(), &achieved, &t);
}
bool Client::set_achievement(const std::string& id, bool on) {
    if (!stats_) return false;
    return vfn<bool (*)(void*, const char*)>(stats_, on ? slot::SetAchievement : slot::ClearAchievement)(stats_, id.c_str());
}
bool Client::get_stat(const std::string& id, int32_t& v) {
    v = 0;
    return stats_ && vfn<bool (*)(void*, const char*, int32_t*)>(stats_, slot::GetStatInt)(stats_, id.c_str(), &v);
}
bool Client::get_stat(const std::string& id, float& v) {
    v = 0;
    return stats_ && vfn<bool (*)(void*, const char*, float*)>(stats_, slot::GetStatFloat)(stats_, id.c_str(), &v);
}
bool Client::set_stat(const std::string& id, int32_t v) { return stats_ && vfn<bool (*)(void*, const char*, int32_t)>(stats_, slot::SetStatInt)(stats_, id.c_str(), v); }
bool Client::set_stat(const std::string& id, float v) { return stats_ && vfn<bool (*)(void*, const char*, float)>(stats_, slot::SetStatFloat)(stats_, id.c_str(), v); }
bool Client::store_stats() { return stats_ && vfn<bool (*)(void*)>(stats_, slot::StoreStats)(stats_); }
bool Client::reset_all_stats(bool a) { return stats_ && vfn<bool (*)(void*, bool)>(stats_, slot::ResetAllStats)(stats_, a); }

bool Client::wait_callback(int id, int timeout_ms, std::vector<uint8_t>* payload) {
    if (!pipe_ || !get_callback_) return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        CallbackMsg msg{};
        int call = 0;
        while (get_callback_(pipe_, &msg, &call)) {
            bool hit = msg.id == id;
            if (hit && payload) payload->assign(msg.param, msg.param + (msg.size > 0 ? msg.size : 0));
            free_callback_(pipe_);
            if (hit) return true;
        }
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void Client::drain_callbacks() {
    if (!pipe_ || !get_callback_) return;
    CallbackMsg msg{};
    int call = 0;
    while (get_callback_(pipe_, &msg, &call)) free_callback_(pipe_);
}

}  // namespace vs::steamc
