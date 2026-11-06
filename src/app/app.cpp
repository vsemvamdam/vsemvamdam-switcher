// vsemvamdam-switcher — application logic (actions, CLI, tray, persistence)
#include "app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <exception>

#include "../core/fsx.h"
#include "../core/i18n.h"
#include "../core/image.h"
#include "../core/log.h"
#include "../core/steam_core.h"
#include "../core/str.h"
#include "../os/win.h"
#include "../ui/textures.h"
#include "../ui/theme.h"
#include "basic_platform.h"
#include "imgui.h"
#include "steam_platform.h"

namespace vs {

App& App::get() {
    static App a;
    return a;
}

// ---------------------------------------------------------------- init
static std::string embedded_platforms_json() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(110), MAKEINTRESOURCEW(10));
    if (!r) return {};
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return {};
    const char* d = static_cast<const char*>(LockResource(g));
    DWORD sz = SizeofResource(nullptr, r);
    return d ? std::string(d, sz) : std::string();
}

void App::load_platform_defs() {
    all_.clear();
    auto st = std::make_unique<SteamPlatform>(store_);
    steam_ = st.get();
    all_.push_back(std::move(st));
    // user override file wins over the embedded catalogue
    std::string text;
    std::string user_file = fsx::join(store_.dir, "platforms.json");
    if (!fsx::read_file(user_file, text)) text = embedded_platforms_json();
    for (auto& p : make_basic_platforms(store_, text)) all_.push_back(std::move(p));
}

bool App::init(const std::vector<std::string>& args) {
    store_.load(data_dir_default());
    log::init(fsx::join(store_.dir, "log.txt"));
    log::info("start, data=" + store_.dir);
    apply_language();
    load_platform_defs();
    guard_init();
    {
        auto& s = store_.settings;
        // Steam is always on; basic platforms are on when enabled in settings
        if (!s.platforms.count("steam")) s.platforms["steam"].enabled = true;
        s.platforms["steam"].enabled = true;
        cur_ = s.last_platform;
        if (!platform(cur_) || !s.platforms[cur_].enabled) cur_ = "steam";
    }
    for (auto& a : args) {
        if (a == "--tray" || a == "--minimized" || a == "/tray") start_hidden_ = true;
        if (a == "--guard") page_ = Page::Guard;
    }
    if (store_.settings.start_minimized) start_hidden_ = true;
    // a switch command passed to the first instance is executed once the UI is ready
    std::string err;
    bool has_cmd = std::any_of(args.begin(), args.end(), [](const std::string& a) {
        return a == "--switch" || a == "--new" || starts_with(a, "+s:") || starts_with(a, "vsemvamdam://");
    });
    if (has_cmd) {
        auto copy = args;
        ui_post([this, copy] {
            std::string e;
            execute_cli(copy, false, e);
            if (!e.empty()) toast(e, true);
        });
    }
    rescan_all();
    return true;
}

void App::on_ready(float dpi) {
    dpi_ = dpi;
    theme_dark_applied_ = want_dark();
    ui::apply_theme(theme_dark_applied_, dpi);
    host_set_theme_dark(theme_dark_applied_);
    ready_ = true;
    if (store_.settings.steam_fetch_profiles && steam_ && steam_->installed()) {
        steam_->refresh_profiles(false, [this](bool changed) {
            if (changed) ui_post([this] { ui::tex_clear(); rescan("steam"); });
        });
    }
}

void App::on_dpi_changed(float dpi) {
    dpi_ = dpi;
    ui::apply_theme(theme_dark_applied_, dpi);
}

bool App::want_dark() const {
    switch (store_.settings.theme) {
        case Theme::Dark: return true;
        case Theme::Light: return false;
        default: {
            uint32_t v = 1;
            os::reg_get_dword("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", "AppsUseLightTheme", v);
            return v == 0;
        }
    }
}

void App::apply_language() { g_lang_en = store_.settings.language == "en"; }

void App::before_frame() {
    ui::tex_pump();
    bool dark = want_dark();
    if (dark != theme_dark_applied_) {
        theme_dark_applied_ = dark;
        ui::apply_theme(dark, dpi_);
        host_set_theme_dark(dark);
    }
}

bool App::animating() const {
    if (busy_) return true;
    if (!toasts_.empty()) return true;
    if (guard_anim_) return true;  // login flow / confirmations loading spinners
    return false;
}

int App::tick_ms() const {
    if (!guard_) return 0;
    if (page_ == Page::Guard) return guard_->empty() ? 0 : 250;
    if (page_ == Page::Accounts && cur_ == "steam" && store_.settings.guard_codes_on_cards && !guard_ids_.empty()) return 500;
    return 0;
}

Placement App::initial_placement() const {
    Placement p;
    auto& s = store_.settings;
    p.x = s.win_x; p.y = s.win_y; p.w = s.win_w; p.h = s.win_h; p.maximized = s.win_max;
    return p;
}

void App::on_window_placement(int x, int y, int w, int h, bool maximized) {
    auto& s = store_.settings;
    if (s.win_x == x && s.win_y == y && s.win_w == w && s.win_h == h && s.win_max == maximized) return;
    s.win_x = x; s.win_y = y; s.win_w = w; s.win_h = h; s.win_max = maximized;
    store_.save_settings();
}

void App::shutdown() {
    if (worker_.joinable()) worker_.join();  // never exit in the middle of clearing/restoring a login
    if (steam_) steam_->stop_refresh();
    if (guard_) guard_->stop();
    store_.settings.last_platform = cur_;
    store_.save_settings();
    store_.save_meta();
    ui::tex_shutdown();
}

// ---------------------------------------------------------------- platforms / accounts
Platform* App::platform(const std::string& id) {
    for (auto& p : all_)
        if (p->id() == id) return p.get();
    return nullptr;
}
Platform* App::current() { return platform(cur_); }

std::vector<Platform*> App::enabled_platforms() {
    std::vector<Platform*> v;
    for (auto& p : all_) {
        auto it = store_.settings.platforms.find(p->id());
        if (p->is_steam() || (it != store_.settings.platforms.end() && it->second.enabled)) v.push_back(p.get());
    }
    return v;
}

void App::rescan(const std::string& pid) {
    Platform* p = platform(pid);
    if (!p) return;
    accounts_[pid] = p->scan();
    if (pid == cur_ && !selected_.empty()) {
        auto& l = accounts_[pid];
        if (std::none_of(l.begin(), l.end(), [&](auto& a) { return a.id == selected_; })) selected_.clear();
    }
}

void App::rescan_all() {
    for (auto* p : enabled_platforms()) rescan(p->id());
}

std::string App::display_name(const std::string& pid, const Account& a) {
    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
    auto& m = store_.meta[pid];
    auto it = m.find(a.id);
    if (it != m.end() && !it->second.custom_name.empty()) return it->second.custom_name;
    if (!a.display.empty()) return a.display;
    return a.login;
}

std::vector<Account> App::sorted_accounts(const std::string& pid) {
    std::vector<Account> v = accounts_[pid];
    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
    auto& metas = store_.meta[pid];
    auto order_of = [&](const Account& a) {
        auto it = metas.find(a.id);
        return it == metas.end() ? -1 : it->second.order;
    };
    std::stable_sort(v.begin(), v.end(), [&](const Account& a, const Account& b) {
        int oa = order_of(a), ob = order_of(b);
        if (oa >= 0 && ob >= 0) return oa < ob;
        if (oa >= 0) return true;
        if (ob >= 0) return false;
        return a.last_login > b.last_login;
    });
    return v;
}

void App::set_order(const std::string& pid, const std::vector<std::string>& ids) {
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        for (size_t i = 0; i < ids.size(); ++i) store_.meta[pid][ids[i]].order = (int)i;
    }
    store_.save_meta();
}

void App::toast(const std::string& text, bool error) {
    if (text.empty()) return;
    toasts_.push_back({text, error, ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0});
    if (toasts_.size() > 3) toasts_.erase(toasts_.begin());
    ui_wake();
}

// ---------------------------------------------------------------- actions
void App::run_switch(const std::string& pid, const SwitchRequest& req, bool from_tray) {
    if (busy_.exchange(true)) {
        toast(TR("Подождите — предыдущее переключение ещё идёт", "Please wait — a switch is in progress"), true);
        return;
    }
    Platform* p = platform(pid);
    if (!p) { busy_ = false; return; }
    busy_platform_ = pid;
    busy_account_ = req.account_id;
    busy_started_ = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    {
        std::lock_guard<std::mutex> lk(step_mtx_);
        busy_step_ = TR("Подготовка…", "Preparing…");
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, p, pid, req, from_tray] {
        OpResult r;
        try {
            r = p->switch_to(req, [this](const std::string& step) {
                {
                    std::lock_guard<std::mutex> lk(step_mtx_);
                    busy_step_ = step;
                }
                ui_wake();
            });
        } catch (const std::exception& e) {
            r.ok = false;
            r.message = std::string(TR("Внутренняя ошибка: ", "Internal error: ")) + e.what();
            log::error("switch exception: " + std::string(e.what()));
        }
        ui_post([this, r, pid, req, from_tray] {
            busy_ = false;
            rescan(pid);
            if (r.ok) {
                std::string who;
                for (auto& a : accounts_[pid])
                    if (a.id == req.account_id) who = display_name(pid, a);
                char sec[32];
                snprintf(sec, sizeof(sec), "%.1f", r.seconds);
                std::string msg = !r.message.empty() ? r.message
                                  : req.account_id.empty() ? std::string(TR("Открыт экран входа", "Login screen opened"))
                                                           : std::string(TR("Вход: ", "Signed in: ")) + who + " · " + sec + TR(" с", " s");
                toast(msg);
                if (!from_tray) after_switch_window();
            } else if (r.needs_admin) {
                admin_msg_ = r.message;
                open_dialog(Dlg::Admin, pid, "");
                host_show_window();
            } else {
                toast(r.message.empty() ? TR("Не получилось", "Failed") : r.message, true);
                host_show_window();
            }
        });
    });
}

void App::request_switch(const std::string& pid, const SwitchRequest& req) {
    if (pid == "steam" && !busy_) {
        uint32_t app = 0;
        if (os::reg_get_dword("HKCU\\Software\\Valve\\Steam", "RunningAppID", app) && app != 0 && os::any_running({"steam.exe"})) {
            std::string name = "AppID " + std::to_string(app);
            if (steam_)
                for (auto& g : steam_->games())
                    if (g.appid == std::to_string(app)) name = g.name;
            pending_req_ = req;
            pending_game_ = name;
            open_dialog(Dlg::GameRunning, pid, req.account_id);
            return;
        }
    }
    run_switch(pid, req);
}

void App::on_activated() {
    double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0;
    if (now - last_activate_ < 1.0 || busy_) return;
    last_activate_ = now;
    rescan(cur_);
}

void App::after_switch_window() {
    if (host_is_hidden()) return;  // started from the tray / a shortcut: leave the window alone
    switch (store_.settings.after_switch) {
        case AfterSwitch::Minimize: host_minimize(); break;
        case AfterSwitch::Tray: host_hide_to_tray(); break;
        case AfterSwitch::Quit: host_quit(); break;
        default: break;
    }
}

void App::forget(const std::string& pid, const std::string& id) {
    Platform* p = platform(pid);
    if (!p) return;
    if (busy_) { toast(TR("Подождите — идёт переключение", "Please wait — a switch is in progress"), true); return; }
    OpResult r = p->forget(id);
    if (selected_ == id) selected_.clear();
    rescan(pid);
    toast(r.message, !r.ok);
}

void App::save_current(const std::string& pid, const std::string& name, const std::string& overwrite_id) {
    Platform* p = platform(pid);
    if (!p || busy_.exchange(true)) return;
    busy_platform_ = pid;
    busy_account_.clear();
    busy_started_ = ImGui::GetTime();
    {
        std::lock_guard<std::mutex> lk(step_mtx_);
        busy_step_ = TR("Сохраняю вход…", "Saving login…");
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, p, pid, name, overwrite_id] {
        std::string id;
        OpResult r;
        try {
            r = p->save_current(name, overwrite_id, id);
        } catch (const std::exception& e) {
            r.ok = false;
            r.message = std::string(TR("Внутренняя ошибка: ", "Internal error: ")) + e.what();
            log::error("save exception: " + std::string(e.what()));
        }
        ui_post([this, r, pid, id] {
            busy_ = false;
            rescan(pid);
            if (r.ok) selected_ = id;
            toast(r.message, !r.ok);
        });
    });
}

void App::create_shortcut(const std::string& pid, const Account& a, int state) {
    std::string name = display_name(pid, a);
    Platform* p = platform(pid);
    std::string icons = fsx::join(store_.dir, "icons");
    fsx::mkdirs(icons);
    std::string ico = fsx::join(icons, sanitize_filename(pid + "_" + a.id) + ".ico");
    img::Image im;
    bool have = !a.avatar_path.empty() && img::load_file(a.avatar_path, im);
    if (!have) im = img::placeholder((unsigned)std::hash<std::string>{}(a.id), 256);
    im = img::resize(img::crop_square(im), 256, 256);
    img::round_corners(im, 0.22f);
    os::write_ico(ico, im.w, im.h, im.rgba);
    std::string arg = "--switch " + pid + ":" + a.id;
    std::string suffix;
    if (p && p->is_steam() && state >= 0) {
        arg += ":" + std::to_string(state);
        suffix = std::string(" (") + steam_state_label(state) + ")";
    }
    std::string lnk = fsx::join(os::desktop_dir(), sanitize_filename(name + " — " + (p ? p->name() : pid) + suffix) + ".lnk");
    std::string exe = os::self_exe_path();
    bool ok = os::create_shortcut(lnk, exe, arg, fsx::parent(exe), ico,
                                  std::string(TR("Войти в ", "Sign in to ")) + (p ? p->name() : pid) + ": " + name);
    toast(ok ? TR("Ярлык создан на рабочем столе", "Shortcut created on the desktop") : TR("Не удалось создать ярлык", "Could not create shortcut"), !ok);
}

void App::open_dialog(Dlg d, const std::string& pid, const std::string& acc) {
    dlg_ = d;
    dlg_open_req_ = true;
    dlg_platform_ = pid;
    dlg_account_ = acc;
    dlg_buf_[0] = 0;
    dlg_buf2_[0] = 0;
    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
    if (d == Dlg::Rename) snprintf(dlg_buf_, sizeof(dlg_buf_), "%s", store_.meta[pid][acc].custom_name.c_str());
    if (d == Dlg::Note) snprintf(dlg_buf_, sizeof(dlg_buf_), "%s", store_.meta[pid][acc].note.c_str());
}

// ---------------------------------------------------------------- CLI
// Supported:
//   --switch steam:<id64|login>[:<state>]   --switch <platform>:<id>   +s:<id64>[:<state>]
//   --new <platform>                        --run-app <appid>          --tray  --show
bool App::execute_cli(const std::vector<std::string>& args, bool headless, std::string& err) {
    std::string target, new_pid, app_id;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--guard-code" && i + 1 < args.size()) {
            // copy the current Steam Guard code of an account (login, SteamID64 or custom name)
            std::string who = args[++i];
            if (starts_with(to_lower(who), "steam:")) who = who.substr(6);
            std::string key;
            uint64_t v = 0;
            if (guard_) {
                if (parse_u64(who, v)) key = guard_->key_for_steamid(v);
                if (key.empty()) key = guard_->key_for_login(who);
                if (key.empty())
                    for (auto& g : guard_->list())
                        if (iequals(guard_name(g), who)) key = g.key;
            }
            if (key.empty()) { err = TR("Нет аутентификатора для ", "No authenticator for ") + who; return true; }
            std::string code = guard_->code(key);
            os::clipboard_set(code);
            if (!headless) toast(std::string(TR("Код Steam Guard скопирован: ", "Steam Guard code copied: ")) + code);
            return true;
        }
        if (a == "--guard") {
            if (!headless) { page_ = Page::Guard; host_show_window(); }
            return true;
        }
        if ((a == "--switch" || a == "-s") && i + 1 < args.size()) target = args[++i];
        else if (starts_with(a, "+s:")) target = "steam:" + a.substr(3);
        else if (starts_with(a, "vsemvamdam://")) target = a.substr(13);
        else if (a == "--new" && i + 1 < args.size()) new_pid = args[++i];
        else if ((a == "--run-app" || starts_with(a, "--run-appid=")) ) {
            if (a == "--run-app" && i + 1 < args.size()) app_id = args[++i];
            else if (starts_with(a, "--run-appid=")) app_id = a.substr(12);
        }
    }
    while (!target.empty() && target.back() == '/') target.pop_back();
    if (target.empty() && new_pid.empty()) return false;
    if (!new_pid.empty()) {
        if (!platform(new_pid)) { err = TR("Неизвестная платформа: ", "Unknown platform: ") + new_pid; return true; }
        SwitchRequest r;
        if (headless) {
            OpResult res = platform(new_pid)->switch_to(r, nullptr);
            if (!res.ok) err = res.message;
        } else {
            run_switch(new_pid, r);
        }
        return true;
    }
    auto parts = split(target, ':');
    std::string pid = to_lower(parts[0]);
    if (pid == "s" || pid == "valve") pid = "steam";
    Platform* p = platform(pid);
    if (!p || parts.size() < 2) { err = TR("Неверная команда: ", "Bad command: ") + target; return true; }
    std::string id = parts[1];
    SwitchRequest r;
    r.app_id = app_id;
    if (parts.size() >= 3) {
        int64_t st;
        if (parse_i64(parts[2], st)) r.state = (int)st;
        else {
            std::string s = to_lower(parts[2]);
            if (s == "online") r.state = steam::Online;
            else if (s == "invisible") r.state = steam::Invisible;
            else if (s == "offline") r.state = steam::Offline;
            else if (s == "away") r.state = steam::Away;
            else if (s == "offlinemode") r.state = kStateOfflineMode;
        }
    }
    // accept login names / custom names in place of ids
    auto list = p->scan();
    bool found = std::any_of(list.begin(), list.end(), [&](auto& a) { return a.id == id; });
    if (!found) {
        for (auto& a : list)
            if (iequals(a.login, id) || iequals(display_name(pid, a), id)) { id = a.id; found = true; break; }
    }
    if (!found && !(p->is_steam() && steam::is_valid_id64(id))) { err = TR("Аккаунт не найден: ", "Account not found: ") + parts[1]; return true; }
    r.account_id = id;
    if (headless) {
        OpResult res = p->switch_to(r, nullptr);
        if (!res.ok) err = res.message;
    } else {
        run_switch(pid, r);
    }
    return true;
}

bool App::handle_cli_early(const std::vector<std::string>& args, int& rc) {
    (void)args;
    (void)rc;
    return false;
}

bool App::handle_cli_headless(const std::vector<std::string>& args, int& rc) {
    bool has_cmd = std::any_of(args.begin(), args.end(), [](const std::string& a) {
        return a == "--switch" || a == "--new" || a == "--guard-code" || starts_with(a, "+s:") || starts_with(a, "vsemvamdam://");
    });
    if (!has_cmd) return false;
    bool keep_open = std::any_of(args.begin(), args.end(), [](const std::string& a) { return a == "--open" || a == "--show"; });
    if (keep_open) return false;
    store_.load(data_dir_default());
    log::init(fsx::join(store_.dir, "log.txt"));
    apply_language();
    load_platform_defs();
    if (std::find(args.begin(), args.end(), "--guard-code") != args.end()) {
        guard_ = std::make_unique<Guard>(store_);
        guard_->load(false);
    }
    std::string err;
    try {
        execute_cli(args, true, err);
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!err.empty()) {
        log::error("cli: " + err);
        os::message_box("vsemvamdam switcher", err, true);
        rc = 2;
    } else {
        rc = 0;
    }
    store_.save_meta();
    return true;
}

void App::on_external_command(const std::string& cmdline) {
    // re-split the forwarded command line
    std::vector<std::string> args;
    std::string cur;
    bool q = false;
    for (char c : cmdline) {
        if (c == '"') { q = !q; continue; }
        if ((c == ' ' || c == '\t') && !q) { if (!cur.empty()) args.push_back(cur), cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) args.push_back(cur);
    std::string err;
    bool did = execute_cli(args, false, err);
    if (!err.empty()) { toast(err, true); host_show_window(); }
    if (!did) host_show_window();
}

// ---------------------------------------------------------------- tray
std::vector<TrayEntry> App::tray_menu() {
    tray_actions_.clear();
    std::vector<TrayEntry> items;
    int next = 100;
    auto add_action = [&](std::function<void()> fn) {
        int id = next++;
        tray_actions_.push_back({id, std::move(fn)});
        return id;
    };
    for (auto* p : enabled_platforms()) {
        rescan(p->id());
        auto list = sorted_accounts(p->id());
        if (list.empty()) continue;
        TrayEntry sub;
        sub.text = p->name();
        // most recently switched first
        std::vector<Account> recent = list;
        {
            std::lock_guard<std::recursive_mutex> lk(store_.mtx);
            auto& m = store_.meta[p->id()];
            std::stable_sort(recent.begin(), recent.end(), [&](const Account& a, const Account& b) {
                return m[a.id].last_switched > m[b.id].last_switched;
            });
        }
        int limit = std::max(1, store_.settings.tray_accounts);
        for (int i = 0; i < (int)recent.size() && i < limit; ++i) {
            const Account& a = recent[i];
            TrayEntry e;
            std::string pid = p->id(), aid = a.id;
            e.text = display_name(pid, a);
            if (store_.settings.streamer_mode == false && !a.login.empty() && a.login != e.text) e.text += "  (" + a.login + ")";
            e.checked = a.current;
            e.id = add_action([this, pid, aid] {
                SwitchRequest r;
                r.account_id = aid;
                run_switch(pid, r, true);
            });
            sub.children.push_back(e);
        }
        sub.children.push_back(TrayEntry{});
        TrayEntry nw;
        std::string pid = p->id();
        nw.text = p->is_steam() ? TR("Добавить аккаунт (экран входа)", "Add account (login screen)") : TR("Новый вход", "New login");
        nw.id = add_action([this, pid] { run_switch(pid, SwitchRequest{}, true); });
        sub.children.push_back(nw);
        items.push_back(sub);
    }
    if (guard_ && !guard_->empty()) {
        TrayEntry sub;
        int pending = guard_->pending_total();
        sub.text = pending > 0 ? std::string("Steam Guard  (") + std::to_string(pending) + ")" : std::string("Steam Guard");
        for (auto& g : guard_->list()) {
            TrayEntry e;
            std::string key = g.key;
            e.text = std::string(TR("Код: ", "Code: ")) + guard_name(g);
            if (!g.confs.items.empty()) e.text += "  ·  " + std::to_string(g.confs.items.size());
            e.id = add_action([this, key] { guard_copy_code(key); });
            sub.children.push_back(e);
        }
        sub.children.push_back(TrayEntry{});
        TrayEntry op;
        op.text = TR("Открыть Steam Guard", "Open Steam Guard");
        op.id = add_action([this] { page_ = Page::Guard; host_show_window(); });
        sub.children.push_back(op);
        if (!items.empty()) items.push_back(TrayEntry{});
        items.push_back(sub);
    }
    if (!items.empty()) items.push_back(TrayEntry{});
    TrayEntry open;
    open.text = TR("Открыть vsemvamdam switcher", "Open vsemvamdam switcher");
    open.id = add_action([] { host_show_window(); });
    items.push_back(open);
    TrayEntry quit;
    quit.text = TR("Выход", "Quit");
    quit.id = add_action([] { host_quit(); });
    items.push_back(quit);
    return items;
}

void App::on_tray_command(int id) {
    for (auto& [i, fn] : tray_actions_)
        if (i == id) { fn(); return; }
}

}  // namespace vs
