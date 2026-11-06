// vsemvamdam-switcher — direct use of the installed Steam client's steamclient64.dll (as Steam Achievement Manager does):
// pretend to be a game (SteamAppId), read/write achievements and stats, check ownership. Windows only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vs::steamc {

enum class InitError { None, NoSteam, NoExports, NoClient, NotRunning, NotLoggedIn, AppMismatch, NoInterfaces };
const char* init_error_code(InitError e);  // stable code word for the helper protocol

constexpr int kUserStatsReceived = 1101;
constexpr int kUserStatsStored = 1102;

class Client {
public:
    Client() = default;
    ~Client() { shutdown(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // appid 0: no game (ownership checks only). steam_root: folder with steamclient64.dll ("" = from the registry)
    InitError init(const std::string& steam_root, uint32_t appid);
    void shutdown();

    uint64_t steam_id();
    bool logged_on();
    uint32_t app_id();
    std::string current_game_language();
    bool is_subscribed_app(uint32_t appid);
    std::string app_data(uint32_t appid, const char* key);

    uint64_t request_user_stats(uint64_t steamid);
    bool get_achievement(const std::string& id, bool& achieved, uint32_t& unlock_time);
    bool set_achievement(const std::string& id, bool achieved);
    bool get_stat(const std::string& id, int32_t& v);
    bool get_stat(const std::string& id, float& v);
    bool set_stat(const std::string& id, int32_t v);
    bool set_stat(const std::string& id, float v);
    bool store_stats();
    bool reset_all_stats(bool achievements_too);

    // Pumps callbacks until `id` arrives (payload copied) or the timeout expires.
    bool wait_callback(int id, int timeout_ms, std::vector<uint8_t>* payload = nullptr);
    void drain_callbacks();

    static std::string default_steam_root();

private:
    void* dll_ = nullptr;
    void* (*create_interface_)(const char*, int*) = nullptr;
    bool (*get_callback_)(int, void*, int*) = nullptr;
    bool (*free_callback_)(int) = nullptr;
    void* client_ = nullptr;
    void* user_iface_ = nullptr;
    void* stats_ = nullptr;
    void* utils_ = nullptr;
    void* apps1_ = nullptr;
    void* apps8_ = nullptr;
    int pipe_ = 0, user_ = 0;
};

}  // namespace vs::steamc
