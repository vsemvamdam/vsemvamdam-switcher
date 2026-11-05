// vsemvamdam-switcher — application state, actions and UI
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/settings.h"
#include "host.h"
#include "platform.h"

namespace vs {

class SteamPlatform;

struct Placement {
    int x = 0, y = 0, w = 0, h = 0;
    bool maximized = false;
};

class App {
public:
    static App& get();

    // host hooks
    bool handle_cli_early(const std::vector<std::string>& args, int& rc);
    bool handle_cli_headless(const std::vector<std::string>& args, int& rc);
    bool init(const std::vector<std::string>& args);
    void on_ready(float dpi_scale);
    void on_dpi_changed(float dpi_scale);
    void before_frame();
    void frame();
    bool animating() const;
    bool wants_slow_tick() const;
    void on_external_command(const std::string& cmdline);
    void on_activated();
    std::vector<TrayEntry> tray_menu();
    void on_tray_command(int id);
    bool close_to_tray() const { return store_.settings.close_to_tray; }
    void on_window_placement(int x, int y, int w, int h, bool maximized);
    Placement initial_placement() const;
    bool start_hidden() const { return start_hidden_; }
    void shutdown();

private:
    App() = default;
    // ---- data
    Store store_;
    std::vector<std::unique_ptr<Platform>> all_;       // every known platform (Steam first)
    SteamPlatform* steam_ = nullptr;
    std::map<std::string, std::vector<Account>> accounts_;
    std::string cur_;          // current platform id
    std::string selected_;     // selected account id
    bool start_hidden_ = false;
    bool ready_ = false;

    // ---- async ops
    std::atomic<bool> busy_{false};
    std::string busy_step_;
    std::string busy_platform_;
    std::string busy_account_;
    double busy_started_ = 0;
    std::mutex step_mtx_;
    std::thread worker_;

    // ---- UI state
    enum class Page { Accounts, Settings };
    Page page_ = Page::Accounts;
    char search_[128] = {};
    std::string ctx_account_;   // account for context menu
    bool open_ctx_ = false;
    int state_choice_ = 0;      // index in state list for bottom bar
    struct Toast {
        std::string text;
        bool error;
        double t0;
    };
    std::vector<Toast> toasts_;
    // dialogs
    enum class Dlg { None, Rename, Note, Forget, SaveCurrent, NewLogin, Admin, About, AddShortcut, GameRunning };
    Dlg dlg_ = Dlg::None;
    bool dlg_open_req_ = false;
    std::string dlg_platform_, dlg_account_;
    char dlg_buf_[512] = {};
    char dlg_buf2_[256] = {};
    std::string admin_msg_;
    SwitchRequest pending_req_;
    std::string pending_game_;
    double last_activate_ = 0;
    int settings_tab_ = 0;
    std::vector<std::pair<int, std::function<void()>>> tray_actions_;
    int drag_from_ = -1;
    double last_click_time_ = 0;
    std::string last_click_id_;
    bool theme_dark_applied_ = false;
    float dpi_ = 1.0f;
    char path_buf_[512] = {};
    char args_buf_[512] = {};

    // ---- helpers
    Platform* platform(const std::string& id);
    Platform* current();
    std::vector<Platform*> enabled_platforms();
    void rescan(const std::string& pid);
    void rescan_all();
    std::vector<Account> sorted_accounts(const std::string& pid);
    std::string display_name(const std::string& pid, const Account& a);
    void toast(const std::string& text, bool error = false);
    void run_switch(const std::string& pid, const SwitchRequest& req, bool from_tray = false);
    void request_switch(const std::string& pid, const SwitchRequest& req);  // UI entry: confirms if a game is running
    void after_switch_window();
    void forget(const std::string& pid, const std::string& id);
    void save_current(const std::string& pid, const std::string& name, const std::string& overwrite_id);
    void create_shortcut(const std::string& pid, const Account& a, int state);
    void set_order(const std::string& pid, const std::vector<std::string>& ids);
    void apply_language();
    bool want_dark() const;
    void open_dialog(Dlg d, const std::string& pid, const std::string& acc);
    bool execute_cli(const std::vector<std::string>& args, bool headless, std::string& err);
    void load_platform_defs();

    // ---- UI
    void draw_sidebar(float w, float h);
    void draw_header(float x, float w);
    void draw_accounts(float x, float y, float w, float h);
    void draw_grid(const std::vector<Account>& list, float w);
    void draw_list(const std::vector<Account>& list, float w);
    void draw_empty(float w, float h);
    void draw_bottom_bar(float x, float y, float w, float h);
    void draw_context_menu();
    void draw_dialogs();
    void draw_toasts();
    void draw_settings(float x, float y, float w, float h);
    void draw_settings_general();
    void draw_settings_steam();
    void draw_settings_platforms();
    void draw_settings_about();
    void account_menu_items(const std::string& pid, const Account& a);
    bool handle_card_input(const std::string& pid, const Account& a, bool hovered);
};

}  // namespace vs
