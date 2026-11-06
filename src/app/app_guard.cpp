// vsemvamdam-switcher — Steam Guard page: codes, confirmations, import/export, login & linking wizard
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include "../core/crypto.h"
#include "../core/fsx.h"
#include "../core/i18n.h"
#include "../core/log.h"
#include "../core/steam_core.h"
#include "../core/str.h"
#include "../os/win.h"
#include "../ui/icons.h"
#include "../ui/textures.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "steam_platform.h"

namespace vs {

using namespace ui;

static void wipe_buf(char* b, size_t n) {
    volatile char* p = b;
    for (size_t i = 0; i < n; ++i) p[i] = 0;
}

static const char* conf_type_label(const guard::Confirmation& c) {
    switch (c.type) {
        case guard::ConfTrade: return TR("Обмен", "Trade offer");
        case guard::ConfMarket: return TR("Торговая площадка", "Market listing");
        case guard::ConfFeatureOptOut: return TR("Отказ от функции", "Feature opt-out");
        case guard::ConfPhoneChange: return TR("Смена номера телефона", "Phone number change");
        case guard::ConfRecovery: return TR("Восстановление аккаунта", "Account recovery");
        case guard::ConfApiKey: return TR("Создание ключа Web API", "Web API key");
        case guard::ConfFamily: return TR("Семейная группа Steam", "Steam Family");
        default: return TR("Подтверждение", "Confirmation");
    }
}

static const char* conf_type_icon(int type) {
    switch (type) {
        case guard::ConfTrade: return ICON_USERS;
        case guard::ConfMarket: return ICON_ARCHIVE;
        case guard::ConfPhoneChange: return ICON_ID_CARD;
        case guard::ConfApiKey: return ICON_COMMAND;
        default: return ICON_SHIELD;
    }
}

// spaced code text: "AB CDE" reads better, but Steam codes are typed as is — draw the letters with extra tracking
static void draw_code(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const std::string& code, float tracking) {
    float x = pos.x;
    for (char ch : code) {
        char s[2] = {ch, 0};
        dl->AddText(f, size, ImVec2(x, pos.y), col, s);
        x += f->CalcTextSizeA(size, FLT_MAX, 0, s).x + tracking;
    }
}
static float code_width(ImFont* f, float size, const std::string& code, float tracking) {
    float w = 0;
    for (char ch : code) {
        char s[2] = {ch, 0};
        w += f->CalcTextSizeA(size, FLT_MAX, 0, s).x + tracking;
    }
    return code.empty() ? 0 : w - tracking;
}

static std::string mask_code(const std::string& c) { return std::string(c.size(), '*'); }

// ================================================================ setup / events
void App::guard_init() {
    guard_ = std::make_unique<Guard>(store_);
    guard_->on_event([this](const GuardEvent& e) { guard_event(e); });
    guard_->load();
    guard_sel_ = store_.settings.guard_last;
}

std::string App::guard_name(const GuardView& v) {
    if (v.steamid) {
        std::string id = std::to_string(v.steamid);
        for (auto& a : accounts_["steam"])
            if (a.id == id) return display_name("steam", a);
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        auto& m = store_.meta["steam"];
        auto it = m.find(id);
        if (it != m.end() && !it->second.custom_name.empty()) return it->second.custom_name;
    }
    return v.account_name.empty() ? std::string("Steam") : v.account_name;
}

std::string App::guard_avatar(uint64_t steamid) {
    if (!steamid || !steam_) return {};
    return steam_->avatar_for(std::to_string(steamid));
}

void App::guard_event(const GuardEvent& e) {
    if (e.kind == GuardEventKind::Changed) return;
    GuardView v;
    if (!guard_->view(e.key, v)) return;
    std::string name = guard_name(v);
    bool foreground = !host_is_hidden() && GetForegroundWindow() == (HWND)main_hwnd();
    std::string text;
    if (e.kind == GuardEventKind::NewConfirmations) {
        text = std::string(TR("Новых подтверждений: ", "New confirmations: ")) + std::to_string(e.count);
        if (!e.text.empty()) text += " — " + e.text;
    } else if (e.kind == GuardEventKind::AutoConfirmed) {
        text = std::string(TR("Подтверждено автоматически: ", "Confirmed automatically: ")) + std::to_string(e.count);
        if (!e.text.empty()) text += " — " + e.text;
    } else if (e.kind == GuardEventKind::SessionExpired) {
        text = TR("Сессия истекла — войдите заново, чтобы видеть подтверждения", "Session expired — sign in again to see confirmations");
    }
    if (foreground) {
        toast(name + ": " + text, e.kind == GuardEventKind::SessionExpired);
        return;
    }
    bool notify;
    {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        notify = store_.settings.guard_notify;
    }
    if (notify && e.kind != GuardEventKind::SessionExpired) {
        guard_sel_ = e.key;
        host_notify("Steam Guard · " + name, text);
    }
}

void App::on_notify_click() {
    page_ = Page::Guard;
    host_show_window();
}

void App::guard_copy_code(const std::string& key) {
    std::string c = guard_->code(key);
    if (c.empty()) return;
    os::clipboard_set(c);
    toast(store_.settings.streamer_mode ? std::string(TR("Код скопирован", "Code copied"))
                                        : std::string(TR("Код скопирован: ", "Code copied: ")) + c);
}

void App::guard_open(const std::string& key) {
    page_ = Page::Guard;
    guard_sel_ = key;
}

// ================================================================ import / export
void App::guard_import(const std::vector<std::string>& paths, const std::string& passkey) {
    if (paths.empty()) return;
    rescan("steam");
    std::map<std::string, uint64_t> logins;
    for (auto& a : accounts_["steam"]) {
        uint64_t v = 0;
        if (parse_u64(a.id, v)) logins[to_lower(a.login)] = v;
    }
    auto resolve = [logins](const std::string& login) -> uint64_t {
        auto it = logins.find(to_lower(login));
        return it == logins.end() ? 0 : it->second;
    };
    ImportReport r = guard_->import_paths(paths, passkey, resolve);
    int got = r.added + r.updated;
    if (got > 0) {
        page_ = Page::Guard;
        guard_sel_ = r.keys.front();
        std::string msg = std::string(TR("Добавлено аккаунтов: ", "Accounts added: ")) + std::to_string(r.added);
        if (r.updated) msg += std::string(TR(", обновлено: ", ", updated: ")) + std::to_string(r.updated);
        toast(msg);
    }
    if (r.need_passkey) {
        g_retry_ = r.retry;
        g_error_ = r.wrong_passkey && !passkey.empty() ? TR("Ключ не подошёл — попробуйте ещё раз", "Wrong passkey — try again") : "";
        open_gdialog(GDlg::Passkey, "");
        return;
    }
    if (!r.errors.empty()) {
        g_import_errors_ = r.errors;
        open_gdialog(GDlg::ImportErrors, "");
    } else if (got == 0) {
        toast(TR("maFile не найдены", "No maFiles found"), true);
    }
}

void App::guard_pick_import(bool folder) {
    if (folder) {
        std::string d = os::pick_folder(main_hwnd(), TR("Папка maFiles из Steam Desktop Authenticator", "The maFiles folder of Steam Desktop Authenticator"));
        if (!d.empty()) guard_import({d}, "");
    } else {
        auto files = os::pick_files(main_hwnd(), TR("Выберите maFile", "Choose maFiles"),
                                    std::string(TR("Файлы maFile", "maFiles")) + "|*.maFile;*.json|" + TR("Все файлы", "All files") + "|*.*", true);
        if (!files.empty()) guard_import(files, "");
    }
}

void App::guard_start_export(const std::vector<std::string>& keys) {
    if (keys.empty()) return;
    std::string d = os::pick_folder(main_hwnd(), TR("Куда сохранить копию (будет создана папка maFiles)", "Where to save the backup (a maFiles folder is created)"));
    if (d.empty()) return;
    g_export_folder_ = d;
    gdlg_keys_ = keys;
    open_gdialog(GDlg::Export, keys.front());
}

void App::on_drop_files(const std::vector<std::string>& paths) {
    std::vector<std::string> use;
    for (auto& p : paths) {
        std::string l = to_lower(p);
        if (fsx::is_dir(p) || ends_with(l, ".mafile") || ends_with(l, ".json")) use.push_back(p);
    }
    if (use.empty()) {
        toast(TR("Перетащите maFile или папку maFiles из SDA", "Drop a maFile or SDA's maFiles folder"), true);
        return;
    }
    host_show_window();
    guard_import(use, "");
}

void App::open_gdialog(GDlg d, const std::string& key) {
    gdlg_ = d;
    gdlg_open_req_ = true;
    gdlg_key_ = key;
    wipe_buf(g_buf1_, sizeof(g_buf1_));
    wipe_buf(g_buf2_, sizeof(g_buf2_));
    wipe_buf(g_buf3_, sizeof(g_buf3_));
    g_check_ = false;
    if (d != GDlg::Passkey) g_error_.clear();
    if (d == GDlg::Deactivate) g_choice_ = 1;
}

// ================================================================ cards integration
bool App::guard_chip(const std::string& id64, ImVec2 pos, bool card_hovered) {
    auto it = guard_ids_.find(id64);
    if (it == guard_ids_.end()) return false;
    std::string code = guard_->code(it->second);
    if (code.empty()) return false;
    bool hide = store_.settings.streamer_mode;
    std::string shown = hide ? mask_code(code) : code;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float fs = S(kBase * 0.9f);
    float tw = code_width(F_SEMI, fs, shown, S(1.2f));
    ImVec2 a = pos, b(pos.x + tw + S(30), pos.y + S(22));
    bool hov = card_hovered && ImGui::IsMouseHoveringRect(a, b);
    dl->AddRectFilled(a, b, with_alpha(hov ? P.surface_active : P.surface, 0.95f), S(11));
    dl->AddRect(a, b, hov ? P.border_strong : P.border, S(11), S(1));
    float left = (float)guard_->seconds_left() / guard::kCodePeriod;
    countdown_ring(dl, ImVec2(a.x + S(11), a.y + S(11)), S(5), left, left < 0.2f ? P.danger : P.text2, with_alpha(P.border_strong, 0.6f), S(1.6f));
    draw_code(dl, F_SEMI, fs, ImVec2(a.x + S(21), a.y + (S(22) - fs) * 0.5f - S(0.5f)), P.text, shown, S(1.2f));
    if (hov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", TR("Код Steam Guard — нажмите, чтобы скопировать", "Steam Guard code — click to copy"));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            guard_copy_code(it->second);
            return true;
        }
        return true;  // swallow other clicks on the chip as well
    }
    return false;
}

// ================================================================ sidebar
void App::draw_sidebar_guard(float iw) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0, S(14)));
    ImGui::SetCursorPosX(S(22));
    section_label(TR("ИНСТРУМЕНТЫ", "TOOLS"));
    ImGui::Dummy(ImVec2(0, S(4)));
    ImVec2 pos(S(12), ImGui::GetCursorScreenPos().y);
    ImGui::SetCursorScreenPos(pos);
    bool sel = page_ == Page::Guard;
    bool clicked = ImGui::InvisibleButton("##guardnav", ImVec2(iw, S(40)));
    bool hov = ImGui::IsItemHovered();
    float hv = anim(ImGui::GetItemID(), hov || sel, 18.0f);
    if (clicked) {
        page_ = Page::Guard;
        search_[0] = 0;
    }
    if (hv > 0) dl->AddRectFilled(pos, ImVec2(pos.x + iw, pos.y + S(40)), with_alpha(sel ? P.surface : P.surface_active, sel ? 1.0f : hv * 0.8f), S(10));
    if (sel) dl->AddRect(pos, ImVec2(pos.x + iw, pos.y + S(40)), P.border, S(10), S(1));
    ImVec2 ip(pos.x + S(10), pos.y + S(10));
    dl->AddRectFilled(ip, ImVec2(ip.x + S(20), ip.y + S(20)), P.accent_soft, S(6));
    ImVec2 ts = F_REG->CalcTextSizeA(S(13), FLT_MAX, 0, ICON_SHIELD);
    dl->AddText(F_REG, S(13), ImVec2(ip.x + (S(20) - ts.x) * 0.5f, ip.y + (S(20) - ts.y) * 0.5f), sel ? P.text : P.text2, ICON_SHIELD);
    dl->AddText(sel ? F_SEMI : F_MED, S(kBase), ImVec2(pos.x + S(40), pos.y + S(11)), sel ? P.text : P.text2, "Steam Guard");
    int pending = guard_->pending_total();
    if (pending > 0) {
        std::string cnt = std::to_string(pending);
        ImVec2 bs = badge_size(cnt.c_str(), 10.5f);
        badge(dl, ImVec2(pos.x + iw - bs.x - S(10), pos.y + (S(40) - bs.y) * 0.5f), cnt.c_str(), P.accent_text, P.accent, 10.5f);
    } else {
        auto n = guard_->list().size();
        if (n) {
            std::string cnt = std::to_string(n);
            ImVec2 cs = F_MED->CalcTextSizeA(S(kBase * 0.85f), FLT_MAX, 0, cnt.c_str());
            dl->AddText(F_MED, S(kBase * 0.85f), ImVec2(pos.x + iw - cs.x - S(12), pos.y + S(12)), P.text3, cnt.c_str());
        }
    }
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + S(42)));
    ImGui::Dummy(ImVec2(0, 0));
}

// ================================================================ page
void App::draw_guard(float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto items = guard_->list();
    float px = x + S(28), py = S(26);
    dl->AddText(F_SEMI, S(27), ImVec2(px, py - S(2)), P.text, "Steam Guard");
    std::string sub;
    if (items.empty()) {
        sub = TR("Мобильный аутентификатор: коды входа и подтверждения обменов", "Mobile authenticator: sign-in codes and trade confirmations");
    } else {
        sub = accounts_count((long long)items.size());
        int pending = 0;
        for (auto& v : items) pending += (int)v.confs.items.size();
        if (pending) sub += std::string(" · ") + TR("подтверждений: ", "confirmations: ") + std::to_string(pending);
        sub += std::string(" · ") + TR("новый код через ", "new code in ") + std::to_string(guard_->seconds_left()) + TR(" с", " s");
    }
    dl->AddText(F_REG, S(kBase * 0.95f), ImVec2(px, py + S(34)), P.text2, sub.c_str());

    // header controls
    float right = x + w - S(28);
    float bx = right - S(34);
    ImGui::SetCursorScreenPos(ImVec2(bx, py + S(6)));
    if (icon_button("##gmore", ICON_ELLIPSIS, TR("Ещё", "More"))) ImGui::OpenPopup("##gmenu");
    if (!items.empty()) {
        bx -= S(38);
        ImGui::SetCursorScreenPos(ImVec2(bx, py + S(6)));
        if (icon_button("##grefresh", ICON_REFRESH_CW, TR("Обновить подтверждения", "Refresh confirmations"))) {
            guard_->fetch_all(false);
            guard_->sync_time_async();
        }
        bx -= S(38);
        ImGui::SetCursorScreenPos(ImVec2(bx, py + S(6)));
        if (icon_button("##gadd", ICON_PLUS, TR("Добавить аккаунт", "Add account"))) ImGui::OpenPopup("##gaddmenu");
        float sw = std::min(S(220), w * 0.26f);
        bx -= sw + S(10);
        ImGui::SetCursorScreenPos(ImVec2(bx, py + S(5)));
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
        search_box("##gsearch", guard_search_, sizeof(guard_search_), TR("Поиск", "Search"), sw);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(6)));
    auto add_items = [&] {
        if (menu_item(ICON_FILE_TEXT, TR("Импорт maFile…", "Import maFile…"))) guard_pick_import(false);
        if (menu_item(ICON_FOLDER_OPEN, TR("Импорт папки SDA…", "Import SDA folder…"))) guard_pick_import(true);
        if (menu_item(ICON_LINK, TR("Привязать новый аккаунт…", "Link a new account…"))) {
            guard_->flow_reset();
            open_gdialog(GDlg::Link, "");
        }
    };
    if (ImGui::BeginPopup("##gaddmenu")) {
        add_items();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##gmenu")) {
        add_items();
        ImGui::Separator();
        std::vector<std::string> all;
        for (auto& v : items) all.push_back(v.key);
        if (menu_item(ICON_DOWNLOAD, TR("Резервная копия всех (формат SDA)…", "Back up all (SDA format)…"), nullptr, !all.empty())) guard_start_export(all);
        if (menu_item(ICON_FOLDER, TR("Папка Steam Guard", "Steam Guard folder"))) {
            fsx::mkdirs(guard_->dir());
            os::shell_open(guard_->dir());
        }
        ImGui::Separator();
        if (menu_item(ICON_SLIDERS_HORIZONTAL, TR("Настройки Steam Guard", "Steam Guard settings"))) {
            page_ = Page::Settings;
            settings_tab_ = 1;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    float header_h = S(104);
    if (items.empty()) {
        draw_guard_empty(x, y + header_h, w, h - header_h);
        return;
    }
    // filter + selection
    std::vector<GuardView> list;
    for (auto& v : items) {
        if (guard_search_[0]) {
            std::string hay = guard_name(v) + " " + v.account_name + " " + std::to_string(v.steamid);
            if (!icontains(hay, guard_search_)) continue;
        }
        list.push_back(v);
    }
    if (std::none_of(items.begin(), items.end(), [&](const GuardView& v) { return v.key == guard_sel_; })) guard_sel_ = items.front().key;
    if (store_.settings.guard_last != guard_sel_) {
        std::lock_guard<std::recursive_mutex> lk(store_.mtx);
        store_.settings.guard_last = guard_sel_;
    }
    const GuardView* selv = nullptr;
    for (auto& v : items)
        if (v.key == guard_sel_) selv = &v;
    // load confirmations once for the account that is opened
    if (selv && guard_fetched_sel_ != guard_sel_) {
        guard_fetched_sel_ = guard_sel_;
        if (selv->has_session && !selv->confs.loading && (!selv->confs.loaded || unix_now() - selv->confs.fetched_at > 30)) guard_->fetch(guard_sel_);
    }

    float body_y = y + header_h;
    float body_h = h - header_h;
    float lw = std::clamp(w * 0.38f, S(270), S(360));
    bool streamer = store_.settings.streamer_mode;

    // ---- left: authenticators
    ImGui::SetCursorScreenPos(ImVec2(x, body_y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(4)));
    ImGui::BeginChild("##glist", ImVec2(lw, body_h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        float avail = ImGui::GetContentRegionAvail().x;
        float rh = S(68);
        int secs = guard_->seconds_left();
        float left_frac = (float)secs / guard::kCodePeriod;
        if (list.empty()) {
            ImGui::Dummy(ImVec2(0, S(20)));
            ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
            ImGui::TextUnformatted(TR("Ничего не найдено", "Nothing found"));
            ImGui::PopStyleColor();
        }
        for (auto& v : list) {
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::PushID(v.key.c_str());
            ImGui::InvisibleButton("##grow", ImVec2(avail, rh));
            bool hov = ImGui::IsItemHovered();
            ImGuiID iid = ImGui::GetItemID();
            ImGui::PopID();
            float hv = anim(iid, hov, 16.0f);
            bool sel = v.key == guard_sel_;
            ImVec2 p1(p0.x + avail, p0.y + rh - S(6));
            if (hv > 0 || sel) ldl->AddRectFilled(p0, p1, with_alpha(P.surface, sel ? 1.0f : hv), S(12));
            if (sel) ldl->AddRect(p0, p1, P.selection, S(12), S(1.4f));
            std::string name = guard_name(v);
            float av = S(38);
            ImVec2 ap(p0.x + S(10), p0.y + (rh - S(6) - av) * 0.5f);
            draw_avatar(ldl, guard_avatar(v.steamid), name, v.steamid ? std::to_string(v.steamid) : v.key, ap, av, 0.22f);
            if (!v.confs.items.empty()) {
                std::string cnt = std::to_string(v.confs.items.size());
                ImVec2 bs = badge_size(cnt.c_str(), 9.5f);
                badge(ldl, ImVec2(ap.x + av - bs.x * 0.6f, ap.y - S(4)), cnt.c_str(), P.accent_text, P.accent, 9.5f);
            }
            // code on the right
            std::string code = guard_->code(v.key);
            std::string shown = streamer ? mask_code(code) : code;
            float cfs = S(19);
            float cw = code_width(F_SEMI, cfs, shown, S(1.5f));
            float cx = p0.x + avail - S(14) - cw;
            float cy = p0.y + (rh - S(6) - cfs) * 0.5f - S(1);
            ImVec2 ca(cx - S(26), p0.y + S(8)), cb(p0.x + avail - S(6), p1.y - S(8));
            bool code_hov = hov && ImGui::IsMouseHoveringRect(ca, cb);
            if (code_hov) ldl->AddRectFilled(ca, cb, with_alpha(P.surface_active, 0.9f), S(9));
            draw_code(ldl, F_SEMI, cfs, ImVec2(cx, cy), v.fully_enrolled ? P.text : P.text3, shown, S(1.5f));
            countdown_ring(ldl, ImVec2(cx - S(13), p0.y + (rh - S(6)) * 0.5f), S(6), left_frac, left_frac < 0.2f ? P.danger : P.text2,
                           with_alpha(P.border_strong, 0.7f), S(2));
            // texts
            float tx = ap.x + av + S(12);
            float tmax = cx - S(34) - tx;
            text_clipped(ldl, F_MED, kBase, ImVec2(tx, ap.y + S(1)), tmax, P.text, name);
            std::string subl = streamer ? std::string("•••••") : v.account_name;
            ImU32 subc = P.text3;
            if (!v.fully_enrolled) { subl = TR("привязка не завершена", "linking not finished"); subc = P.danger; }
            else if (!v.has_session) { subl = TR("нужен вход", "sign-in needed"); subc = P.warn; }
            text_clipped(ldl, F_REG, kBase * 0.85f, ImVec2(tx, ap.y + S(21)), tmax, subc, subl);
            if (hov) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (code_hov) ImGui::SetTooltip("%s", TR("Нажмите, чтобы скопировать код", "Click to copy the code"));
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    guard_sel_ = v.key;
                    if (code_hov) guard_copy_code(v.key);
                }
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !code_hov) guard_copy_code(v.key);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    guard_sel_ = v.key;
                    gctx_key_ = v.key;
                    gctx_open_ = true;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rh));
        }
        ImGui::Dummy(ImVec2(0, S(8)));
        // keyboard: Up/Down select, Ctrl+C copy
        if (!list.empty() && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            int idx = 0;
            for (int i = 0; i < (int)list.size(); ++i)
                if (list[i].key == guard_sel_) idx = i;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) guard_sel_ = list[std::min(idx + 1, (int)list.size() - 1)].key;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) guard_sel_ = list[std::max(idx - 1, 0)].key;
            if ((ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) guard_copy_code(guard_sel_);
        }
    }
    ImGui::EndChild();
    dl->AddLine(ImVec2(x + lw, body_y + S(6)), ImVec2(x + lw, y + h - S(18)), P.border, S(1));

    // ---- right: details
    ImGui::SetCursorScreenPos(ImVec2(x + lw, body_y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(4)));
    ImGui::BeginChild("##gdetail", ImVec2(w - lw, body_h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    if (selv) draw_guard_detail(*selv, ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();

    // context menu for a row
    if (gctx_open_) {
        ImGui::OpenPopup("##gctx");
        gctx_open_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(7)));
    if (ImGui::BeginPopup("##gctx")) {
        bool found = false;
        for (auto& v : items)
            if (v.key == gctx_key_) {
                found = true;
                guard_menu_items(v);
            }
        if (!found) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void App::guard_menu_items(const GuardView& v) {
    std::string key = v.key;
    if (menu_item(ICON_COPY, TR("Копировать код", "Copy code"), "Ctrl+C")) guard_copy_code(key);
    if (menu_item(ICON_USER, TR("Копировать логин", "Copy login"), nullptr, !v.account_name.empty())) {
        os::clipboard_set(v.account_name);
        toast(TR("Скопировано", "Copied"));
    }
    if (menu_item(ICON_REFRESH_CW, TR("Обновить подтверждения", "Refresh confirmations"), nullptr, v.has_session)) guard_->fetch(key);
    if (menu_item(ICON_LOG_IN, TR("Войти заново…", "Sign in again…"))) {
        guard_->flow_reset();
        open_gdialog(GDlg::Login, key);
    }
    std::string id = v.steamid ? std::to_string(v.steamid) : std::string();
    bool in_switcher = !id.empty() && std::any_of(accounts_["steam"].begin(), accounts_["steam"].end(), [&](const Account& a) { return a.id == id; });
    if (menu_item(ICON_ARROW_UP_RIGHT, TR("Переключиться на аккаунт в Steam", "Switch Steam to this account"), nullptr, in_switcher && !busy_)) {
        SwitchRequest r;
        r.account_id = id;
        request_switch("steam", r);
    }
    if (menu_item(ICON_EXTERNAL_LINK, TR("Открыть профиль", "Open profile"), nullptr, !id.empty())) os::shell_open(steam::profile_url(id));
    ImGui::Separator();
    if (!v.fully_enrolled && menu_item(ICON_BADGE_CHECK, TR("Код работает — отметить как подключённый", "The code works — mark as active"))) {
        guard_->mark_enrolled(key);
        toast(TR("Аутентификатор отмечен как подключённый", "Marked as active"));
    }
    if (menu_item(ICON_KEYBOARD, TR("Код восстановления…", "Revocation code…"), nullptr, v.has_revocation)) open_gdialog(GDlg::Revocation, key);
    if (menu_item(ICON_DOWNLOAD, TR("Резервная копия maFile…", "Back up maFile…"))) guard_start_export({key});
    ImGui::Separator();
    if (menu_item(ICON_SHIELD_ALERT, TR("Удалить аутентификатор из Steam…", "Remove authenticator from Steam…"), nullptr, v.has_revocation, true))
        open_gdialog(GDlg::Deactivate, key);
    if (menu_item(ICON_TRASH_2, TR("Убрать из свитчера…", "Remove from the switcher…"), nullptr, true, true)) open_gdialog(GDlg::RemoveLocal, key);
}

void App::draw_guard_detail(const GuardView& v, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool streamer = store_.settings.streamer_mode;
    std::string name = guard_name(v);
    ImGui::Dummy(ImVec2(0, S(6)));

    // ---- code card
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    float ch = S(176);
    ImVec2 c1(c0.x + w, c0.y + ch);
    dl->AddRectFilled(c0, c1, P.surface, S(18));
    dl->AddRect(c0, c1, P.border, S(18), S(1));
    float av = S(44);
    ImVec2 ap(c0.x + S(20), c0.y + S(20));
    draw_avatar(dl, guard_avatar(v.steamid), name, v.steamid ? std::to_string(v.steamid) : v.key, ap, av, 0.24f);
    float tx = ap.x + av + S(14);
    text_clipped(dl, F_SEMI, kBase * 1.15f, ImVec2(tx, ap.y + S(2)), w - (tx - c0.x) - S(70), P.text, name);
    std::string sub = streamer ? std::string("••••• · SteamID ••••") : v.account_name + (v.steamid ? " · " + std::to_string(v.steamid) : std::string());
    text_clipped(dl, F_REG, kBase * 0.85f, ImVec2(tx, ap.y + S(24)), w - (tx - c0.x) - S(70), P.text3, sub);
    ImGui::SetCursorScreenPos(ImVec2(c1.x - S(54), c0.y + S(20)));
    if (icon_button("##gdmore", ICON_ELLIPSIS_VERTICAL, TR("Действия", "Actions"))) {
        gctx_key_ = v.key;
        gctx_open_ = true;
    }
    // big code + ring
    std::string code = guard_->code(v.key);
    std::string shown = streamer ? mask_code(code) : code;
    float fs = S(46);
    float track = S(5);
    float cw = code_width(F_SEMI, fs, shown, track);
    ImVec2 cp(c0.x + S(20), c0.y + S(84));
    ImVec2 ca(cp.x - S(8), cp.y - S(6)), cb(cp.x + cw + S(8), cp.y + fs + S(6));
    ImGui::SetCursorScreenPos(ca);
    bool click = ImGui::InvisibleButton("##bigcode", ImVec2(cb.x - ca.x, cb.y - ca.y));
    bool hov = ImGui::IsItemHovered();
    if (hov) {
        dl->AddRectFilled(ca, cb, P.surface_active, S(10));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    draw_code(dl, F_SEMI, fs, cp, v.fully_enrolled ? P.text : P.text3, shown, track);
    if (click) guard_copy_code(v.key);
    int secs = guard_->seconds_left();
    float frac = std::max(0.0f, 1.0f - guard_->period_progress());
    ImVec2 rc(cb.x + S(34), cp.y + fs * 0.5f + S(2));
    countdown_ring(dl, rc, S(15), frac, secs <= 5 ? P.danger : P.accent, with_alpha(P.border_strong, 0.6f), S(3));
    std::string sl = std::to_string(secs);
    ImVec2 sls = F_SEMI->CalcTextSizeA(S(12.5f), FLT_MAX, 0, sl.c_str());
    dl->AddText(F_SEMI, S(12.5f), ImVec2(rc.x - sls.x * 0.5f, rc.y - sls.y * 0.5f), secs <= 5 ? P.danger : P.text2, sl.c_str());
    // copy button
    float bw = S(140);
    ImGui::SetCursorScreenPos(ImVec2(c1.x - bw - S(20), cp.y + (fs - S(38)) * 0.5f + S(2)));
    if (pill_button(TR("Копировать", "Copy"), true, ImVec2(bw, S(38)), !code.empty(), ICON_COPY)) guard_copy_code(v.key);
    if (!v.fully_enrolled) {
        dl->AddText(F_REG, S(kBase * 0.85f), ImVec2(c0.x + S(20), c1.y - S(26)), P.danger,
                    TR("Привязка не завершена — код ещё не действует", "Linking isn't finished — the code is not active yet"));
    }
    ImGui::SetCursorScreenPos(ImVec2(c0.x, c1.y + S(22)));

    // ---- confirmations
    const auto& cf = v.confs;
    ImVec2 hp = ImGui::GetCursorScreenPos();
    dl->AddText(F_SEMI, S(kBase * 1.15f), hp, P.text, TR("Подтверждения", "Confirmations"));
    float hx = hp.x + F_SEMI->CalcTextSizeA(S(kBase * 1.15f), FLT_MAX, 0, TR("Подтверждения", "Confirmations")).x + S(10);
    if (!cf.items.empty()) {
        std::string n = std::to_string(cf.items.size());
        badge(dl, ImVec2(hx, hp.y + S(2)), n.c_str(), P.accent_text, P.accent, 10.5f);
    } else if (cf.loaded && cf.fetched_at) {
        std::string when = ago(cf.fetched_at);
        dl->AddText(F_REG, S(kBase * 0.85f), ImVec2(hx, hp.y + S(2)), P.text3, when.c_str());
    }
    float rx = hp.x + w;
    float next_y = hp.y + S(34);
    if (v.has_session) {
        rx -= S(34);
        ImGui::SetCursorScreenPos(ImVec2(rx, hp.y - S(8)));
        if (cf.loading) spinner(dl, ImVec2(rx + S(17), hp.y + S(9)), S(8), P.text2, S(2));
        else if (icon_button("##cfrefresh", ICON_REFRESH_CW, TR("Обновить", "Refresh"))) guard_->fetch(v.key);
        if (cf.items.size() > 1) {
            ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + S(32)));
            std::string all = std::string(TR("Подтвердить все", "Accept all")) + " (" + std::to_string(cf.items.size()) + ")";
            if (pill_button(all.c_str(), true, ImVec2(0, S(34)), cf.busy.empty(), ICON_CHECK)) {
                g_accept_ = true;
                open_gdialog(GDlg::RespondAll, v.key);
            }
            ImGui::SameLine(0, S(8));
            if (pill_button(TR("Отклонить все", "Deny all"), false, ImVec2(0, S(34)), cf.busy.empty(), ICON_X)) {
                g_accept_ = false;
                open_gdialog(GDlg::RespondAll, v.key);
            }
            next_y = hp.y + S(80);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(hp.x, next_y));

    auto info_panel = [&](const char* icon, const std::string& title, const std::string& text, const char* btn, bool warn) -> bool {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float wrap = w - S(80);
        ImVec2 ts = F_REG->CalcTextSizeA(S(kBase * 0.92f), FLT_MAX, wrap, text.c_str());
        float ph = S(56) + ts.y + (btn ? S(50) : 0);
        ImVec2 p1(p0.x + w, p0.y + ph);
        dl->AddRectFilled(p0, p1, warn ? with_alpha(P.warn_soft, 0.9f) : with_alpha(P.surface, 0.6f), S(14));
        dl->AddRect(p0, p1, P.border, S(14), S(1));
        dl->AddText(F_REG, S(kBase * 1.2f), ImVec2(p0.x + S(18), p0.y + S(16)), warn ? P.warn : P.text2, icon);
        dl->AddText(F_SEMI, S(kBase), ImVec2(p0.x + S(50), p0.y + S(16)), P.text, title.c_str());
        dl->AddText(F_REG, S(kBase * 0.92f), ImVec2(p0.x + S(50), p0.y + S(40)), P.text2, text.c_str(), nullptr, wrap);
        bool pressed = false;
        if (btn) {
            ImGui::SetCursorScreenPos(ImVec2(p0.x + S(50), p0.y + S(50) + ts.y));
            pressed = pill_button(btn, true, ImVec2(0, S(36)));
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + S(12)));
        return pressed;
    };

    if (!v.fully_enrolled) {
        if (info_panel(ICON_TRIANGLE_ALERT, TR("Привязка не завершена", "Linking not finished"),
                       TR("Steam не подтвердил активацию. Если в аккаунте Steam мобильный аутентификатор так и не появился — уберите его отсюда и привяжите заново. "
                          "Если код при входе в Steam всё же подходит — отметьте его как подключённый в меню «⋮». Код восстановления сохраните в любом случае.",
                          "Steam didn't confirm the activation. If the account still has no mobile authenticator, remove it here and link again. "
                          "If the code does work when signing in, mark it as active in the ⋮ menu. Keep the revocation code either way."),
                       TR("Убрать…", "Remove…"), true))
            open_gdialog(GDlg::RemoveLocal, v.key);
    } else if (!v.has_session) {
        std::string text = cf.needauth ? TR("Steam завершил сессию этого аккаунта. Войдите снова — код Steam Guard подставится сам, пароль нигде не сохраняется.",
                                            "Steam ended this account's session. Sign in again — the Steam Guard code is filled in automatically, the password is not stored.")
                                       : TR("Коды уже работают. Чтобы подтверждать обмены и продажи на Торговой площадке, один раз войдите в аккаунт — пароль нигде не сохраняется.",
                                            "Codes already work. To confirm trades and Market listings, sign in once — the password is not stored.");
        if (info_panel(ICON_LOG_IN, cf.needauth ? TR("Нужен вход", "Sign-in needed") : TR("Подтверждения ещё не подключены", "Confirmations aren't set up"), text,
                       TR("Войти", "Sign in"), cf.needauth)) {
            guard_->flow_reset();
            open_gdialog(GDlg::Login, v.key);
        }
    } else if (!cf.error.empty() && cf.items.empty()) {
        if (info_panel(ICON_WIFI_OFF, TR("Не удалось загрузить", "Couldn't load"), cf.error, TR("Повторить", "Retry"), true)) guard_->fetch(v.key);
    } else if (cf.loading && !cf.loaded) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        spinner(dl, ImVec2(p.x + S(16), p.y + S(16)), S(9), P.text2, S(2.2f));
        dl->AddText(F_REG, S(kBase), ImVec2(p.x + S(36), p.y + S(7)), P.text2, TR("Загружаю…", "Loading…"));
        ImGui::Dummy(ImVec2(0, S(40)));
    } else if (cf.items.empty()) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(F_REG, S(kBase * 1.1f), ImVec2(p.x + S(2), p.y + S(6)), P.ok, ICON_CIRCLE_CHECK);
        dl->AddText(F_REG, S(kBase), ImVec2(p.x + S(28), p.y + S(6)), P.text2,
                    TR("Нет ожидающих подтверждений", "Nothing waiting for confirmation"));
        ImGui::Dummy(ImVec2(0, S(36)));
    } else {
        if (!cf.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, P.danger);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
            ImGui::TextUnformatted(cf.error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, S(4)));
        }
        for (auto& c : cf.items) {
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            int lines = (int)std::min<size_t>(c.summary.size(), 6);
            float ph = S(76) + lines * S(19) + (c.warn.empty() ? 0 : S(20));
            ImVec2 p1(p0.x + w, p0.y + ph);
            dl->AddRectFilled(p0, p1, P.surface, S(14));
            dl->AddRect(p0, p1, P.border, S(14), S(1));
            float is = S(48);
            ImVec2 ip(p0.x + S(16), p0.y + S(16));
            std::string icon = guard_->icon_path(c.icon);
            ImTextureID it = icon.empty() ? ImTextureID() : tex_file(icon, 96, 0.18f);
            if (it) dl->AddImage(ImTextureRef(it), ip, ImVec2(ip.x + is, ip.y + is));
            else {
                dl->AddRectFilled(ip, ImVec2(ip.x + is, ip.y + is), P.accent_soft, S(10));
                const char* ic = conf_type_icon(c.type);
                ImVec2 ts = F_REG->CalcTextSizeA(S(20), FLT_MAX, 0, ic);
                dl->AddText(F_REG, S(20), ImVec2(ip.x + (is - ts.x) * 0.5f, ip.y + (is - ts.y) * 0.5f), P.text2, ic);
            }
            float tx2 = ip.x + is + S(14);
            float bw2 = S(118);
            float tmax = w - (tx2 - p0.x) - bw2 - S(28);
            std::string kind = conf_type_label(c);
            if (c.time) kind += " · " + ago(c.time);
            text_clipped(dl, F_REG, kBase * 0.8f, ImVec2(tx2, p0.y + S(14)), tmax, P.text3, kind);
            text_clipped(dl, F_SEMI, kBase, ImVec2(tx2, p0.y + S(32)), tmax, P.text, c.headline.empty() ? std::string(conf_type_label(c)) : c.headline);
            float ly = p0.y + S(54);
            for (int i = 0; i < lines; ++i) {
                text_clipped(dl, F_REG, kBase * 0.88f, ImVec2(tx2, ly), tmax, P.text2, c.summary[i]);
                ly += S(19);
            }
            if (!c.warn.empty()) text_clipped(dl, F_REG, kBase * 0.85f, ImVec2(tx2, ly + S(2)), tmax, P.warn, c.warn);
            if (ImGui::IsMouseHoveringRect(p0, ImVec2(p1.x - bw2 - S(24), p1.y)) && ImGui::IsWindowHovered()) {
                std::string full = c.headline;
                for (auto& l : c.summary) full += "\n" + l;
                if (!c.warn.empty()) full += "\n" + c.warn;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(9)));
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(S(360));
                ImGui::TextUnformatted(full.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
                ImGui::PopStyleVar();
            }
            bool busy = cf.busy.count(c.id) > 0;
            ImGui::PushID(c.id.c_str());
            if (busy) {
                spinner(dl, ImVec2(p1.x - S(16) - bw2 * 0.5f, p0.y + S(40)), S(10), P.text2, S(2.2f));
            } else {
                ImGui::SetCursorScreenPos(ImVec2(p1.x - S(16) - bw2, p0.y + S(16)));
                if (pill_button(c.accept.empty() || c.accept.size() > 18 ? TR("Подтвердить", "Accept") : c.accept.c_str(), true, ImVec2(bw2, S(34)), true, ICON_CHECK))
                    guard_->respond(v.key, {c.id}, true);
                ImGui::SetCursorScreenPos(ImVec2(p1.x - S(16) - bw2, p0.y + S(56)));
                if (pill_button(c.cancel.empty() || c.cancel.size() > 18 ? TR("Отклонить", "Deny") : c.cancel.c_str(), false, ImVec2(bw2, S(34)), true, ICON_X))
                    guard_->respond(v.key, {c.id}, false);
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + S(10)));
        }
    }
    ImGui::Dummy(ImVec2(0, S(18)));
}

void App::draw_guard_empty(float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!g_sda_scanned_) {
        g_sda_scanned_ = true;
        std::thread([this] {
            auto found = Guard::find_sda_folders();
            ui_post([this, found] { g_sda_found_ = found; });
        }).detach();
    }
    float cx = x + w * 0.5f;
    float cy = y + h * 0.22f;
    dl->AddCircleFilled(ImVec2(cx, cy), S(44), P.accent_soft, 48);
    ImVec2 ts = F_REG->CalcTextSizeA(S(40), FLT_MAX, 0, ICON_SHIELD);
    dl->AddText(F_REG, S(40), ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f + S(4)), P.text2, ICON_SHIELD);
    const char* title = TR("Steam Guard прямо в свитчере", "Steam Guard right in the switcher");
    ImVec2 t1 = F_SEMI->CalcTextSizeA(S(18), FLT_MAX, 0, title);
    dl->AddText(F_SEMI, S(18), ImVec2(cx - t1.x * 0.5f, cy + S(64)), P.text, title);
    const char* desc = TR("Коды входа, подтверждение обменов и продаж на Торговой площадке — как в Steam Desktop Authenticator. "
                          "Перенесите аккаунты из SDA (просто перетащите папку maFiles в окно) или привяжите новый.",
                          "Sign-in codes, trade and Market confirmations — like Steam Desktop Authenticator. "
                          "Bring your accounts over from SDA (just drop the maFiles folder onto the window) or link a new one.");
    float wrap = std::min(w - S(60), S(500));
    ImVec2 t2 = F_REG->CalcTextSizeA(S(kBase), FLT_MAX, wrap, desc);
    // center each wrapped line block
    dl->AddText(F_REG, S(kBase), ImVec2(cx - wrap * 0.5f, cy + S(96)), P.text2, desc, nullptr, wrap);
    float by = cy + S(116) + t2.y;
    float bw = S(200);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw - S(6), by));
    if (pill_button(TR("Импорт из SDA", "Import from SDA"), true, ImVec2(bw, S(40)), true, ICON_FOLDER_OPEN)) guard_pick_import(true);
    ImGui::SetCursorScreenPos(ImVec2(cx + S(6), by));
    if (pill_button(TR("Привязать аккаунт", "Link an account"), false, ImVec2(bw, S(40)), true, ICON_LINK)) {
        guard_->flow_reset();
        open_gdialog(GDlg::Link, "");
    }
    ImGui::SetCursorScreenPos(ImVec2(cx - bw - S(6), by + S(50)));
    if (pill_button(TR("Выбрать maFile…", "Choose maFiles…"), false, ImVec2(bw * 2 + S(12), S(36)), true, ICON_FILE_TEXT)) guard_pick_import(false);
    float fy = by + S(104);
    if (!g_sda_found_.empty()) {
        const char* lbl = TR("Найдено на этом компьютере:", "Found on this PC:");
        ImVec2 ls = F_MED->CalcTextSizeA(S(kBase * 0.92f), FLT_MAX, 0, lbl);
        dl->AddText(F_MED, S(kBase * 0.92f), ImVec2(cx - ls.x * 0.5f, fy), P.text2, lbl);
        fy += S(26);
        for (size_t i = 0; i < g_sda_found_.size() && i < 4; ++i) {
            const std::string& f = g_sda_found_[i];
            float rw = std::min(w - S(60), S(520));
            ImVec2 r0(cx - rw * 0.5f, fy), r1(cx + rw * 0.5f, fy + S(44));
            dl->AddRectFilled(r0, r1, P.surface, S(12));
            dl->AddRect(r0, r1, P.border, S(12), S(1));
            dl->AddText(F_REG, S(kBase), ImVec2(r0.x + S(14), r0.y + S(13)), P.text2, ICON_FOLDER);
            text_clipped(dl, F_REG, kBase * 0.9f, ImVec2(r0.x + S(40), r0.y + S(13)), rw - S(170), P.text, f);
            ImGui::SetCursorScreenPos(ImVec2(r1.x - S(124), r0.y + S(6)));
            ImGui::PushID((int)i);
            if (pill_button(TR("Импорт", "Import"), true, ImVec2(S(116), S(32)))) guard_import({f}, "");
            ImGui::PopID();
            fy += S(52);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(x, fy));
    ImGui::Dummy(ImVec2(0, 0));
}

// ================================================================ settings (Steam tab)
void App::draw_settings_guard() {
    auto& s = store_.settings;
    bool dirty = false;
    float cw = S(260);
    ImGui::Dummy(ImVec2(0, S(14)));
    section_label("STEAM GUARD");
    ImGui::Dummy(ImVec2(0, S(6)));
    setting_row_begin(TR("Коды на карточках Steam", "Codes on Steam cards"),
                      TR("Текущий код Steam Guard прямо на карточке аккаунта — щелчок копирует", "The current Steam Guard code on the account card — click to copy"), S(40));
    if (toggle("##gcards", &s.guard_codes_on_cards)) dirty = true;
    setting_row_end();

    setting_row_begin(TR("Проверять подтверждения в фоне", "Check confirmations in the background"),
                      TR("Как «Periodic checking» в SDA: свитчер сам смотрит новые обмены и продажи", "Like SDA's periodic checking: looks for new trades and listings"), S(40));
    if (toggle("##gpoll", &s.guard_poll)) dirty = true;
    setting_row_end();

    if (s.guard_poll) {
        setting_row_begin(TR("Как часто", "How often"), nullptr, cw);
        {
            const int vals[] = {30, 60, 120, 300, 900};
            const char* it[] = {TR("Каждые 30 секунд", "Every 30 seconds"), TR("Каждую минуту", "Every minute"), TR("Каждые 2 минуты", "Every 2 minutes"),
                                TR("Каждые 5 минут", "Every 5 minutes"), TR("Каждые 15 минут", "Every 15 minutes")};
            int idx = 1;
            for (int i = 0; i < 5; ++i)
                if (vals[i] == s.guard_poll_sec) idx = i;
            ImGui::SetNextItemWidth(cw);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(8)));
            if (ImGui::BeginCombo("##gpollsec", it[idx])) {
                for (int i = 0; i < 5; ++i)
                    if (ImGui::Selectable(it[i], idx == i)) { s.guard_poll_sec = vals[i]; dirty = true; }
                ImGui::EndCombo();
            }
            ImGui::PopStyleVar();
        }
        setting_row_end();

        setting_row_begin(TR("Уведомлять о новых", "Notify about new ones"), TR("Уведомление Windows, когда свитчер свёрнут", "A Windows notification while the switcher is in the background"), S(40));
        if (toggle("##gnotify", &s.guard_notify)) dirty = true;
        setting_row_end();

        setting_row_begin(TR("Автоматически подтверждать продажи", "Auto-confirm Market listings"),
                          TR("Выставленные на Торговую площадку предметы подтвердятся сами", "Items you list on the Community Market get confirmed by themselves"), S(40));
        if (toggle("##gautomarket", &s.guard_auto_market)) dirty = true;
        setting_row_end();

        setting_row_begin(TR("Автоматически подтверждать обмены", "Auto-confirm trades"),
                          TR("Осторожно: подтвердится и обмен, созданный мошенником через ваш аккаунт", "Careful: a trade created by a scammer through your account is confirmed too"), S(40));
        if (toggle("##gautotrade", &s.guard_auto_trades)) dirty = true;
        setting_row_end();
    }
    if (dirty) store_.save_settings();
}

// ================================================================ dialogs
static void error_line(const std::string& e) {
    if (e.empty()) return;
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::PushStyleColor(ImGuiCol_Text, P.danger);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(e.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void field_label(const char* t) {
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::PushFont(F_MED, kBase * 0.9f);
    ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static void plain_input(const char* id, char* buf, size_t n, const char* hint, int flags = 0, bool focus = false) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(9)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, P.bg);
    ImGui::SetNextItemWidth(-1);
    if (focus && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint(id, hint, buf, n, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

static void code_box(const std::string& code, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float h = S(64);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), P.bg, S(12));
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), P.border_strong, S(12), S(1));
    float fs = S(32);
    float cw = code_width(F_SEMI, fs, code, S(4));
    draw_code(dl, F_SEMI, fs, ImVec2(p0.x + (w - cw) * 0.5f, p0.y + (h - fs) * 0.5f), P.text, code, S(4));
    ImGui::Dummy(ImVec2(w, h));
}

void App::draw_guard_dialogs() {
    static const char* ids[] = {"", "##gd_passkey", "##gflow", "##gflow", "##gd_revoc", "##gd_export", "##gd_remove", "##gd_deact", "##gd_all", "##gd_errors"};
    if (gdlg_ != GDlg::None && gdlg_open_req_) {
        ImGui::OpenPopup(ids[(int)gdlg_]);
        gdlg_open_req_ = false;
    }
    if (gdlg_ == GDlg::None) return;
    if (gdlg_ == GDlg::Login || gdlg_ == GDlg::Link) {
        draw_guard_flow_dialog();
        return;
    }
    GuardView v;
    bool have = !gdlg_key_.empty() && guard_->view(gdlg_key_, v);
    std::string name = have ? guard_name(v) : std::string();
    bool close = false;
    float dw = 470;
    switch (gdlg_) {
        case GDlg::Passkey:
            if (begin_dialog(ids[1], TR("Папка SDA зашифрована", "The SDA folder is encrypted"), dw)) {
                dialog_text(TR("Введите ключ шифрования (passkey), который вы задавали в Steam Desktop Authenticator. "
                               "Файлы расшифруются только для импорта — в свитчере они хранятся под защитой Windows (DPAPI).",
                               "Enter the encryption passkey you set in Steam Desktop Authenticator. The files are decrypted for the import only — "
                               "the switcher keeps them protected by Windows (DPAPI)."));
                for (auto& r : g_retry_) {
                    ImGui::PushStyleColor(ImGuiCol_Text, P.text3);
                    ImGui::TextUnformatted(r.c_str());
                    ImGui::PopStyleColor();
                }
                field_label(TR("Ключ шифрования", "Passkey"));
                plain_input("##pk", g_buf2_, sizeof(g_buf2_), "", ImGuiInputTextFlags_Password, true);
                error_line(g_error_);
                int r = dialog_buttons(TR("Импортировать", "Import"), TR("Отмена", "Cancel"), g_buf2_[0] != 0);
                if ((r == 1 || (ImGui::IsKeyPressed(ImGuiKey_Enter) && g_buf2_[0]))) {
                    std::string pk = g_buf2_;
                    wipe_buf(g_buf2_, sizeof(g_buf2_));
                    auto retry = g_retry_;
                    close = true;
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    gdlg_ = GDlg::None;
                    guard_import(retry, pk);
                    crypto::wipe(pk);
                    return;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case GDlg::ImportErrors:
            if (begin_dialog(ids[9], TR("Не всё удалось импортировать", "Some files were not imported"), 520)) {
                ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
                ImGui::PushTextWrapPos(0);
                for (auto& e : g_import_errors_) ImGui::BulletText("%s", e.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                int r = dialog_buttons(TR("Понятно", "OK"), nullptr);
                if (r) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case GDlg::Revocation:
            if (begin_dialog(ids[4], TR("Код восстановления", "Revocation code"), dw)) {
                std::string code = guard_->revocation_code(gdlg_key_);
                dialog_text(name + "\n\n" +
                            TR("С этим кодом можно удалить аутентификатор, если доступ к свитчеру и maFile потерян. Храните его отдельно от компьютера.",
                               "This code removes the authenticator if you lose the switcher and the maFile. Keep it away from this PC."));
                ImGui::Dummy(ImVec2(0, S(8)));
                code_box(store_.settings.streamer_mode ? mask_code(code) : code, ImGui::GetContentRegionAvail().x);
                int r = dialog_buttons(TR("Копировать", "Copy"), TR("Закрыть", "Close"), !code.empty());
                if (r == 1) { os::clipboard_set(code); toast(TR("Скопировано", "Copied")); close = true; }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case GDlg::Export:
            if (begin_dialog(ids[5], TR("Резервная копия maFile", "maFile backup"), 500)) {
                std::string who = gdlg_keys_.size() == 1 ? name : accounts_count((long long)gdlg_keys_.size());
                dialog_text(who + "\n" + TR("Папка: ", "Folder: ") + g_export_folder_ + "\n\n" +
                            TR("Будет создана папка maFiles в формате Steam Desktop Authenticator — её можно положить рядом с SDA или импортировать обратно. "
                               "Задайте ключ, чтобы зашифровать файлы, как в SDA. Без ключа любой, кто получит файлы, получит доступ к аккаунту.",
                               "A maFiles folder in Steam Desktop Authenticator format is created — put it next to SDA or import it back. "
                               "Set a passkey to encrypt the files like SDA does. Without it, anyone who gets the files gets the account."));
                field_label(TR("Ключ шифрования (необязательно)", "Passkey (optional)"));
                plain_input("##ek1", g_buf1_, sizeof(g_buf1_), "", ImGuiInputTextFlags_Password, true);
                field_label(TR("Ещё раз", "Repeat"));
                plain_input("##ek2", g_buf2_, sizeof(g_buf2_), "", ImGuiInputTextFlags_Password);
                bool match = std::strcmp(g_buf1_, g_buf2_) == 0;
                if (!match && g_buf2_[0]) error_line(TR("Ключи не совпадают", "Passkeys don't match"));
                else if (!g_buf1_[0]) error_line(TR("Без ключа файлы будут не зашифрованы", "Without a passkey the files are not encrypted"));
                int r = dialog_buttons(TR("Сохранить", "Save"), TR("Отмена", "Cancel"), match);
                if (r == 1 && match) {
                    std::string out, err;
                    if (guard_->export_sda(gdlg_keys_, g_export_folder_, g_buf1_, out, err)) {
                        toast(std::string(TR("Сохранено: ", "Saved: ")) + out);
                        os::shell_open(out);
                    } else {
                        toast(err, true);
                    }
                    wipe_buf(g_buf1_, sizeof(g_buf1_));
                    wipe_buf(g_buf2_, sizeof(g_buf2_));
                    close = true;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case GDlg::RemoveLocal:
            if (begin_dialog(ids[6], TR("Убрать из свитчера?", "Remove from the switcher?"), dw)) {
                dialog_text(name + "\n\n" +
                            TR("Аутентификатор останется привязан к аккаунту Steam, но его секреты будут удалены из свитчера. "
                               "Если это единственная копия, а кода восстановления нет — вы потеряете доступ к аккаунту. "
                               "Сначала сделайте резервную копию.",
                               "The authenticator stays linked to the Steam account, but its secrets are removed from the switcher. "
                               "If this is the only copy and you have no revocation code, you lose the account. Back it up first."));
                ImGui::Dummy(ImVec2(0, S(8)));
                if (pill_button(TR("Резервная копия…", "Back up…"), false, ImVec2(S(190), S(36)), true, ICON_DOWNLOAD)) {
                    std::string k = gdlg_key_;
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    gdlg_ = GDlg::None;
                    guard_start_export({k});
                    return;
                }
                int r = dialog_buttons(TR("Убрать", "Remove"), TR("Отмена", "Cancel"), true, true);
                if (r == 1) {
                    guard_->remove_local(gdlg_key_);
                    toast(TR("Аутентификатор убран из свитчера", "Authenticator removed from the switcher"));
                    close = true;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case GDlg::Deactivate:
            if (begin_dialog(ids[7], TR("Удалить аутентификатор из Steam?", "Remove the authenticator from Steam?"), 500)) {
                dialog_text(name + "\n\n" +
                            TR("Steam отвяжет этот мобильный аутентификатор от аккаунта (с помощью кода восстановления). "
                               "После этого обмены и продажи на Торговой площадке будут задерживаться до 15 дней.",
                               "Steam unlinks this mobile authenticator from the account (using the revocation code). "
                               "After that, trades and Market listings are held for up to 15 days."));
                ImGui::Dummy(ImVec2(0, S(6)));
                ImGui::RadioButton(TR("Вернуть коды Steam Guard на почту", "Go back to e-mail Steam Guard codes"), &g_choice_, 1);
                ImGui::RadioButton(TR("Отключить Steam Guard полностью (не рекомендуется)", "Turn Steam Guard off completely (not recommended)"), &g_choice_, 2);
                error_line(g_error_);
                int r = dialog_buttons(TR("Удалить", "Remove"), TR("Отмена", "Cancel"), true, true);
                if (r == 1) {
                    int scheme = g_choice_;
                    guard_->deactivate(gdlg_key_, scheme, [this](bool ok, const std::string& msg) { toast(msg, !ok); });
                    toast(TR("Отправляю запрос в Steam…", "Asking Steam…"));
                    close = true;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case GDlg::RespondAll:
            if (begin_dialog(ids[8], g_accept_ ? TR("Подтвердить все?", "Accept all?") : TR("Отклонить все?", "Deny all?"), dw)) {
                int trades = 0, n = (int)v.confs.items.size();
                for (auto& c : v.confs.items)
                    if (c.type == guard::ConfTrade) ++trades;
                std::string t = name + "\n\n" + (g_accept_ ? TR("Будут подтверждены: ", "To accept: ") : TR("Будут отклонены: ", "To deny: ")) + std::to_string(n);
                if (g_accept_ && trades)
                    t += std::string("\n\n") + TR("Среди них есть обмены — убедитесь, что все они ваши: мошенники подменяют обмены через взломанные API-ключи.",
                                                  "Some are trades — make sure all of them are yours: scammers swap trades through stolen API keys.");
                dialog_text(t);
                int r = dialog_buttons(g_accept_ ? TR("Подтвердить", "Accept") : TR("Отклонить", "Deny"), TR("Отмена", "Cancel"), n > 0, !g_accept_);
                if (r == 1) {
                    std::vector<std::string> idsv;
                    for (auto& c : v.confs.items) idsv.push_back(c.id);
                    guard_->respond(gdlg_key_, idsv, g_accept_);
                    close = true;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        default:
            close = true;
            break;
    }
    if (close) gdlg_ = GDlg::None;
}

void App::draw_guard_flow_dialog() {
    FlowState f = guard_->flow();
    bool link = gdlg_ == GDlg::Link;
    GuardView v;
    bool have = !gdlg_key_.empty() && guard_->view(gdlg_key_, v);
    const char* title = link ? TR("Привязать аутентификатор", "Link an authenticator") : TR("Вход в Steam", "Sign in to Steam");
    if (!begin_dialog("##gflow", title, 500)) {
        // closed from outside
        if (f.step != FlowStep::Done) guard_->flow_reset();
        gdlg_ = GDlg::None;
        return;
    }
    bool close = false;
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    auto spinner_line = [&](const std::string& msg) {
        ImGui::Dummy(ImVec2(0, S(6)));
        ImVec2 p = ImGui::GetCursorScreenPos();
        spinner(ImGui::GetWindowDrawList(), ImVec2(p.x + S(12), p.y + S(12)), S(10), P.text, S(2.4f));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(34), p.y + S(2)));
        ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(msg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(10)));
    };
    auto msg_line = [&](const FlowState& fs) {
        if (fs.message.empty()) return;
        if (fs.error) error_line(fs.message);
        else {
            ImGui::Dummy(ImVec2(0, S(4)));
            dialog_text(fs.message);
        }
    };
    switch (f.step) {
        case FlowStep::Idle:
            if (link) {
                dialog_text(TR("Свитчер станет мобильным аутентификатором аккаунта — как Steam Desktop Authenticator. "
                               "Коды входа и подтверждения обменов будут здесь, а не в приложении Steam на телефоне.",
                               "The switcher becomes the account's mobile authenticator — like Steam Desktop Authenticator. "
                               "Sign-in codes and trade confirmations will live here instead of the Steam phone app."));
                field_label(TR("Логин Steam", "Steam login"));
                plain_input("##lg", g_buf1_, sizeof(g_buf1_), "", 0, true);
                field_label(TR("Пароль", "Password"));
                plain_input("##pw", g_buf2_, sizeof(g_buf2_), "", ImGuiInputTextFlags_Password);
                ImGui::Dummy(ImVec2(0, S(4)));
                ImGui::PushStyleColor(ImGuiCol_Text, P.text3);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(TR("Пароль нужен один раз, чтобы войти, и нигде не сохраняется.", "The password is used once to sign in and is never stored."));
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                bool ok = trim(g_buf1_).size() > 0 && g_buf2_[0];
                int r = dialog_buttons(TR("Продолжить", "Continue"), TR("Отмена", "Cancel"), ok);
                if ((r == 1 || enter) && ok) {
                    guard_->flow_link(trim(g_buf1_), g_buf2_);
                    wipe_buf(g_buf2_, sizeof(g_buf2_));
                }
                if (r == 2) close = true;
            } else {
                std::string nm = have ? guard_name(v) : std::string();
                std::string who = have ? nm + (v.account_name.empty() || iequals(v.account_name, nm) ? "" : " (" + v.account_name + ")") : std::string();
                dialog_text(who + "\n\n" +
                            TR("Сессия нужна, чтобы подтверждать обмены и продажи. Код Steam Guard подставится автоматически, пароль нигде не сохраняется.",
                               "A session is needed to confirm trades and listings. The Steam Guard code is filled in automatically; the password is never stored."));
                field_label(TR("Пароль Steam", "Steam password"));
                plain_input("##pw", g_buf2_, sizeof(g_buf2_), "", ImGuiInputTextFlags_Password, true);
                bool ok = g_buf2_[0] != 0 && have;
                int r = dialog_buttons(TR("Войти", "Sign in"), TR("Отмена", "Cancel"), ok);
                if ((r == 1 || enter) && ok) {
                    guard_->flow_login(gdlg_key_, g_buf2_);
                    wipe_buf(g_buf2_, sizeof(g_buf2_));
                }
                if (r == 2) close = true;
            }
            break;
        case FlowStep::Working:
        case FlowStep::WaitConfirmation: {
            spinner_line(f.message.empty() ? std::string(TR("Секунду…", "One moment…")) : f.message);
            int r = dialog_buttons(TR("Отмена", "Cancel"), nullptr);
            if (r) close = true;
            break;
        }
        case FlowStep::NeedEmailCode:
        case FlowStep::NeedDeviceCode: {
            if (f.step == FlowStep::NeedEmailCode)
                dialog_text(std::string(TR("Steam отправил код подтверждения на почту", "Steam sent a confirmation code to your e-mail")) +
                            (f.hint.empty() ? std::string(".") : " (@" + f.hint + ")."));
            else
                dialog_text(TR("Введите код Steam Guard из мобильного приложения Steam.", "Enter the Steam Guard code from the Steam mobile app."));
            field_label(TR("Код", "Code"));
            plain_input("##code", g_buf3_, sizeof(g_buf3_), "", ImGuiInputTextFlags_CharsUppercase, true);
            if (f.error) error_line(f.message);
            bool ok = trim(g_buf3_).size() >= 5;
            int r = dialog_buttons(TR("Продолжить", "Continue"), TR("Отмена", "Cancel"), ok);
            if ((r == 1 || enter) && ok) {
                guard_->flow_code(g_buf3_);
                wipe_buf(g_buf3_, sizeof(g_buf3_));
            }
            if (r == 2) close = true;
            break;
        }
        case FlowStep::NeedPhone: {
            dialog_text(TR("Для привязки Steam требует номер телефона: на него придёт SMS с кодом. Номер можно будет убрать позже в настройках аккаунта Steam.",
                           "Steam needs a phone number to link the authenticator: an SMS code will be sent to it. You can remove the number later in your Steam account settings."));
            field_label(TR("Номер телефона", "Phone number"));
            plain_input("##phone", g_buf1_, sizeof(g_buf1_), "+7 999 123-45-67", 0, true);
            field_label(TR("Код страны (необязательно)", "Country code (optional)"));
            plain_input("##cc", g_buf3_, sizeof(g_buf3_), "RU", ImGuiInputTextFlags_CharsUppercase);
            if (f.error) error_line(f.message);
            bool ok = trim(g_buf1_).size() >= 6;
            int r = dialog_buttons(TR("Продолжить", "Continue"), TR("Отмена", "Cancel"), ok);
            if ((r == 1 || enter) && ok) guard_->flow_phone(g_buf1_, g_buf3_);
            if (r == 2) close = true;
            break;
        }
        case FlowStep::NeedPhoneEmail: {
            dialog_text(std::string(TR("Steam отправил письмо на ", "Steam sent an e-mail to ")) + (f.hint.empty() ? TR("вашу почту", "your address") : f.hint) +
                        TR(". Откройте его, нажмите ссылку для подтверждения номера и вернитесь сюда.", ". Open it, click the link to confirm the number and come back here."));
            if (f.error) error_line(f.message);
            int r = dialog_buttons(TR("Готово", "Done"), TR("Отмена", "Cancel"));
            if (r == 1) guard_->flow_phone_email_done();
            if (r == 2) close = true;
            break;
        }
        case FlowStep::NeedPhoneSms: {
            dialog_text(TR("Введите код из SMS, чтобы подтвердить номер телефона.", "Enter the SMS code to confirm the phone number."));
            field_label(TR("Код из SMS", "SMS code"));
            plain_input("##sms", g_buf3_, sizeof(g_buf3_), "", ImGuiInputTextFlags_CharsUppercase, true);
            if (f.error) error_line(f.message);
            bool ok = trim(g_buf3_).size() >= 4;
            int r = dialog_buttons(TR("Продолжить", "Continue"), TR("Отмена", "Cancel"), ok);
            if ((r == 1 || enter) && ok) {
                guard_->flow_sms(g_buf3_);
                wipe_buf(g_buf3_, sizeof(g_buf3_));
            }
            if (r == 2) close = true;
            break;
        }
        case FlowStep::NeedActivation: {
            dialog_text(TR("Аутентификатор добавлен, осталось его активировать. Сначала запишите код восстановления:",
                           "The authenticator is added — one step left. First write down the revocation code:"));
            ImGui::Dummy(ImVec2(0, S(6)));
            float bw = ImGui::GetContentRegionAvail().x;
            code_box(f.revocation, bw);
            if (pill_button(TR("Копировать код восстановления", "Copy the revocation code"), false, ImVec2(bw, S(34)), true, ICON_COPY)) {
                os::clipboard_set(f.revocation);
                toast(TR("Скопировано", "Copied"));
            }
            ImGui::Dummy(ImVec2(0, S(4)));
            ImGui::PushStyleColor(ImGuiCol_Text, P.danger);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(TR("Храните его отдельно от компьютера. Без него при потере свитчера и maFile доступ к аккаунту не вернуть.",
                                      "Keep it away from this PC. Without it, losing the switcher and the maFile means losing the account."));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Checkbox(TR("Я записал(а) код восстановления", "I wrote down the revocation code"), &g_check_);
            std::string hint = f.hint.empty()
                                   ? std::string(TR("Код активации из письма Steam (или из SMS, если к аккаунту привязан телефон)",
                                                    "Activation code from Steam's e-mail (or SMS if a phone is linked)"))
                                   : std::string(TR("Код активации из SMS на номер, оканчивающийся на ", "Activation code from the SMS sent to the number ending in ")) + f.hint;
            field_label(hint.c_str());
            plain_input("##act", g_buf3_, sizeof(g_buf3_), "", ImGuiInputTextFlags_CharsUppercase, true);
            if (f.error) error_line(f.message);
            bool ok = g_check_ && trim(g_buf3_).size() >= 5;
            int r = dialog_buttons(TR("Активировать", "Activate"), TR("Отмена", "Cancel"), ok);
            if ((r == 1 || (enter && ok)) && ok) guard_->flow_finalize(g_buf3_);
            if (r == 2) close = true;
            break;
        }
        case FlowStep::Done: {
            if (f.kind == FlowKind::Link) {
                ImGui::PushStyleColor(ImGuiCol_Text, P.ok);
                ImGui::TextUnformatted(ICON_CIRCLE_CHECK);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextUnformatted(TR("Аутентификатор подключён", "The authenticator is active"));
                dialog_text(TR("Теперь коды и подтверждения для этого аккаунта — в свитчере. Сохраните резервную копию maFile: "
                               "данные защищены вашей учётной записью Windows и после переустановки системы без копии не откроются.",
                               "Codes and confirmations for this account now live in the switcher. Save a maFile backup: "
                               "the data is protected by your Windows account and can't be opened after a reinstall without a copy."));
                ImGui::Dummy(ImVec2(0, S(6)));
                code_box(f.revocation, ImGui::GetContentRegionAvail().x);
                int r = dialog_buttons(TR("Резервная копия…", "Back up…"), TR("Готово", "Done"));
                if (r == 1) {
                    std::string k = f.key;
                    guard_->flow_reset();
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    gdlg_ = GDlg::None;
                    guard_sel_ = k;
                    page_ = Page::Guard;
                    guard_start_export({k});
                    return;
                }
                if (r == 2) {
                    guard_sel_ = f.key;
                    page_ = Page::Guard;
                    close = true;
                }
            } else {
                msg_line(f);
                int r = dialog_buttons(TR("Готово", "Done"), nullptr);
                if (r || enter) close = true;
            }
            break;
        }
        case FlowStep::Failed: {
            error_line(f.message.empty() ? std::string(TR("Не получилось", "Failed")) : f.message);
            int r = dialog_buttons(TR("Ещё раз", "Try again"), TR("Закрыть", "Close"));
            if (r == 1) guard_->flow_reset();  // back to the first step of the same dialog
            if (r == 2) close = true;
            break;
        }
    }
    if (close) {
        guard_->flow_reset();
        wipe_buf(g_buf2_, sizeof(g_buf2_));
        ImGui::CloseCurrentPopup();
        gdlg_ = GDlg::None;
    }
    ImGui::EndPopup();
}

}  // namespace vs
