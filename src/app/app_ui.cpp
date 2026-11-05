// vsemvamdam-switcher — main UI (sidebar, header, account grid/list, bottom bar, menus, dialogs)
#include <algorithm>
#include <cmath>
#include <thread>

#include "../core/fsx.h"
#include "../core/image.h"
#include "../core/i18n.h"
#include "../core/steam_core.h"
#include "../core/str.h"
#include "../os/win.h"
#include "../ui/icons.h"
#include "../ui/textures.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../version.h"
#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "steam_platform.h"

namespace vs {

using namespace ui;

static const int kStates[] = {kStateKeep, steam::Online, steam::Away, steam::Invisible, steam::Offline, kStateOfflineMode};
static const int kStateCount = 6;

static ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

static unsigned seed_of(const std::string& s) { return (unsigned)std::hash<std::string>{}(s); }

static std::string initials(const std::string& name) {
    if (name.empty()) return "?";
    // first UTF-8 codepoint, uppercased for ASCII / Cyrillic
    size_t len = 1;
    unsigned char c = (unsigned char)name[0];
    if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
    std::string f = name.substr(0, len);
    if (len == 1) f[0] = (char)toupper(c);
    else if (len == 2) {
        unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)name[1] & 0x3F);
        if (cp >= 0x430 && cp <= 0x44F) cp -= 0x20;
        if (cp == 0x451) cp = 0x401;
        f = std::string() + (char)(0xC0 | (cp >> 6)) + (char)(0x80 | (cp & 0x3F));
    }
    return f;
}

static void draw_avatar(ImDrawList* dl, const std::string& path, const std::string& name, const std::string& id, ImVec2 a, float size,
                        float round_frac) {
    int px = (int)std::ceil(size);
    ImTextureID t = tex_file(path, px > 256 ? 256 : px, round_frac);
    ImVec2 b(a.x + size, a.y + size);
    if (t) {
        dl->AddImage(ImTextureRef(t), a, b);
    } else {
        ImTextureID ph = tex_placeholder(seed_of(id), 96, round_frac);
        if (ph) dl->AddImage(ImTextureRef(ph), a, b);
        else dl->AddRectFilled(a, b, P.accent_soft, size * round_frac);
        std::string ini = initials(name);
        float fs = size * 0.42f;
        ImVec2 ts = F_SEMI->CalcTextSizeA(fs, FLT_MAX, 0, ini.c_str());
        dl->AddText(F_SEMI, fs, ImVec2(a.x + (size - ts.x) * 0.5f, a.y + (size - ts.y) * 0.5f), with_alpha(IM_COL32(23, 23, 26, 255), 0.55f), ini.c_str());
    }
}

// ================================================================ frame
void App::frame() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, P.bg);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    float sw = S(236);
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    draw_sidebar(sw, H);
    if (page_ == Page::Settings) {
        draw_settings(sw, 0, W - sw, H);
    } else {
        draw_accounts(sw, 0, W - sw, H);
    }
    draw_context_menu();
    draw_dialogs();
    draw_toasts();

    // global shortcuts
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
            rescan_all();
            tex_clear();
            if (cur_ == "steam" && steam_) steam_->invalidate_games();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma)) page_ = page_ == Page::Settings ? Page::Accounts : Page::Settings;
    }
    ImGui::End();
}

// ================================================================ sidebar
void App::draw_sidebar(float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), P.sidebar);
    dl->AddLine(ImVec2(w - 0.5f, 0), ImVec2(w - 0.5f, h), P.border, S(1));

    // brand mark
    float x = S(22), y = S(24);
    {
        float r = S(9);
        ImVec2 c1(x + r, y + r + S(1)), c2(x + r * 2.1f, y + r * 1.6f + S(1));
        dl->AddCircle(c1, r * 0.82f, g_dark ? IM_COL32(214, 196, 168, 255) : IM_COL32(176, 156, 126, 255), 32, S(2.2f));
        dl->AddCircleFilled(c2, r * 0.82f, P.text, 32);
        dl->AddText(F_SEMI, S(kBase * 1.08f), ImVec2(x + S(40), y - S(1)), P.text, "vsemvamdam");
        dl->AddText(F_REG, S(kBase * 0.86f), ImVec2(x + S(40), y + S(16)), P.text3, "switcher");
    }

    ImGui::SetCursorScreenPos(ImVec2(S(16), S(84)));
    ImGui::BeginGroup();
    ImGui::SetCursorPosX(S(22));
    section_label(TR("ПЛАТФОРМЫ", "PLATFORMS"));
    ImGui::Dummy(ImVec2(0, S(4)));

    auto plats = enabled_platforms();
    float iw = w - S(24);
    for (auto* p : plats) {
        ImVec2 pos(S(12), ImGui::GetCursorScreenPos().y);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(p->id().c_str());
        bool sel = page_ == Page::Accounts && cur_ == p->id();
        bool clicked = ImGui::InvisibleButton("##plat", ImVec2(iw, S(40)));
        bool hov = ImGui::IsItemHovered();
        float hv = anim(ImGui::GetItemID(), hov || sel, 18.0f);
        ImGui::PopID();
        if (clicked) {
            cur_ = p->id();
            page_ = Page::Accounts;
            selected_.clear();
            search_[0] = 0;
            rescan(cur_);
            store_.settings.last_platform = cur_;
        }
        if (hv > 0) dl->AddRectFilled(pos, ImVec2(pos.x + iw, pos.y + S(40)), with_alpha(sel ? P.surface : P.surface_active, sel ? 1.0f : hv * 0.8f), S(10));
        if (sel) dl->AddRect(pos, ImVec2(pos.x + iw, pos.y + S(40)), P.border, S(10), S(1));
        // icon from the client exe
        ImTextureID ic = tex_exe_icon(p->exe_path(), 32);
        ImVec2 ip(pos.x + S(10), pos.y + S(10));
        if (ic) dl->AddImage(ImTextureRef(ic), ip, ImVec2(ip.x + S(20), ip.y + S(20)));
        else {
            dl->AddRectFilled(ip, ImVec2(ip.x + S(20), ip.y + S(20)), P.accent_soft, S(6));
            std::string ini = initials(p->name());
            ImVec2 ts = F_SEMI->CalcTextSizeA(S(11), FLT_MAX, 0, ini.c_str());
            dl->AddText(F_SEMI, S(11), ImVec2(ip.x + (S(20) - ts.x) * 0.5f, ip.y + (S(20) - ts.y) * 0.5f), P.text2, ini.c_str());
        }
        dl->AddText(sel ? F_SEMI : F_MED, S(kBase), ImVec2(pos.x + S(40), pos.y + S(11)), sel ? P.text : P.text2, p->name().c_str());
        auto it = accounts_.find(p->id());
        int n = it == accounts_.end() ? 0 : (int)it->second.size();
        if (n > 0) {
            std::string cnt = std::to_string(n);
            ImVec2 ts = F_MED->CalcTextSizeA(S(kBase * 0.85f), FLT_MAX, 0, cnt.c_str());
            dl->AddText(F_MED, S(kBase * 0.85f), ImVec2(pos.x + iw - ts.x - S(12), pos.y + S(12)), P.text3, cnt.c_str());
        }
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + S(42)));
    }

    // add platforms
    {
        ImVec2 pos(S(12), ImGui::GetCursorScreenPos().y + S(2));
        ImGui::SetCursorScreenPos(pos);
        bool clicked = ImGui::InvisibleButton("##addplat", ImVec2(iw, S(36)));
        bool hov = ImGui::IsItemHovered();
        if (hov) dl->AddRectFilled(pos, ImVec2(pos.x + iw, pos.y + S(36)), with_alpha(P.surface_active, 0.8f), S(10));
        dl->AddText(F_REG, S(kBase * 1.05f), ImVec2(pos.x + S(12), pos.y + S(9)), P.text3, ICON_PLUS);
        dl->AddText(F_REG, S(kBase * 0.95f), ImVec2(pos.x + S(40), pos.y + S(9)), hov ? P.text : P.text3, TR("Добавить платформу", "Add platform"));
        if (clicked) { page_ = Page::Settings; settings_tab_ = 2; }
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::EndGroup();

    // bottom: settings
    {
        ImVec2 pos(S(12), h - S(60));
        ImGui::SetCursorScreenPos(pos);
        bool sel = page_ == Page::Settings;
        bool clicked = ImGui::InvisibleButton("##settings", ImVec2(iw, S(40)));
        bool hov = ImGui::IsItemHovered();
        if (hov || sel) dl->AddRectFilled(pos, ImVec2(pos.x + iw, pos.y + S(40)), sel ? P.surface : with_alpha(P.surface_active, 0.8f), S(10));
        if (sel) dl->AddRect(pos, ImVec2(pos.x + iw, pos.y + S(40)), P.border, S(10), S(1));
        dl->AddText(F_REG, S(kBase * 1.1f), ImVec2(pos.x + S(11), pos.y + S(10)), sel || hov ? P.text : P.text2, ICON_SETTINGS);
        dl->AddText(sel ? F_SEMI : F_MED, S(kBase), ImVec2(pos.x + S(40), pos.y + S(11)), sel || hov ? P.text : P.text2, TR("Настройки", "Settings"));
        dl->AddText(F_REG, S(kBase * 0.8f), ImVec2(pos.x + iw - S(44), pos.y + S(13)), P.text3, "v" VS_VER_STRING);
        if (clicked) page_ = sel ? Page::Accounts : Page::Settings;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
}

// ================================================================ accounts page
void App::draw_accounts(float x, float y, float w, float h) {
    Platform* p = current();
    if (!p) return;
    float header_h = S(104), bar_h = S(72);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    draw_header(x, w);

    ImGui::SetCursorScreenPos(ImVec2(x, y + header_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(6)));
    ImGui::BeginChild("##content", ImVec2(w, h - header_h - bar_h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();

    auto all = sorted_accounts(cur_);
    std::vector<Account> list;
    for (auto& a : all) {
        if (search_[0]) {
            std::string hay = display_name(cur_, a) + " " + a.login + " " + a.display + " " + a.id;
            std::lock_guard<std::recursive_mutex> lk(store_.mtx);
            hay += " " + store_.meta[cur_][a.id].note;
            if (!icontains(hay, search_)) continue;
        }
        list.push_back(a);
    }
    float cw = ImGui::GetContentRegionAvail().x;
    if (!p->installed() && all.empty()) {
        draw_empty(cw, h - header_h - bar_h);
    } else if (all.empty()) {
        draw_empty(cw, h - header_h - bar_h);
    } else if (list.empty()) {
        ImGui::Dummy(ImVec2(0, S(40)));
        ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
        ImGui::TextUnformatted(TR("Ничего не найдено", "Nothing found"));
        ImGui::PopStyleColor();
    } else if (store_.settings.grid_view) {
        draw_grid(list, cw);
    } else {
        draw_list(list, cw);
    }

    // keyboard navigation over the visible list
    if (!list.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        int idx = -1;
        for (int i = 0; i < (int)list.size(); ++i)
            if (list[i].id == selected_) idx = i;
        float card_w = S(168) * store_.settings.card_scale, gap = S(16);
        int cols = store_.settings.grid_view ? std::max(1, (int)((cw + gap) / (card_w + gap))) : 1;
        int ni = idx;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ni = idx + 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) ni = idx - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ni = idx < 0 ? 0 : idx + cols;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) ni = idx - cols;
        if (ni != idx) {
            ni = std::clamp(ni, 0, (int)list.size() - 1);
            selected_ = list[ni].id;
        }
        if (idx >= 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                SwitchRequest r;
                r.account_id = selected_;
                request_switch(cur_, r);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) open_dialog(Dlg::Forget, cur_, selected_);
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) open_dialog(Dlg::Rename, cur_, selected_);
        }
        // 1..9 quick switch
        for (int k = 0; k < 9 && k < (int)list.size(); ++k) {
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + k))) {
                SwitchRequest r;
                r.account_id = list[k].id;
                request_switch(cur_, r);
            }
        }
    }
    ImGui::EndChild();

    draw_bottom_bar(x, y + h - bar_h, w, bar_h);
}

void App::draw_header(float x, float w) {
    Platform* p = current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float px = x + S(28), py = S(26);
    dl->AddText(F_SEMI, S(27), ImVec2(px, py - S(2)), P.text, p->name().c_str());
    // subtitle
    std::string sub;
    auto& acc = accounts_[cur_];
    if (!p->installed() && acc.empty()) sub = TR("Клиент не найден на этом компьютере", "Client not found on this PC");
    else {
        sub = accounts_count((long long)acc.size());
        for (auto& a : acc)
            if (a.current) { sub += std::string(" · ") + TR("последний вход: ", "last used: ") + (store_.settings.streamer_mode ? std::string("•••") : display_name(cur_, a)); break; }
    }
    dl->AddText(F_REG, S(kBase * 0.95f), ImVec2(px, py + S(34)), P.text2, sub.c_str());

    // right side controls
    float right = x + w - S(28);
    float bx = right - S(34);
    ImGui::SetCursorScreenPos(ImVec2(bx, py + S(6)));
    if (icon_button("##more", ICON_ELLIPSIS, TR("Ещё", "More"))) ImGui::OpenPopup("##platmenu");
    bx -= S(38);
    ImGui::SetCursorScreenPos(ImVec2(bx, py + S(6)));
    if (icon_button("##refresh", ICON_REFRESH_CW, TR("Обновить (F5)", "Refresh (F5)"))) {
        rescan(cur_);
        tex_clear();
        if (cur_ == "steam" && steam_) {
            steam_->invalidate_games();
            if (store_.settings.steam_fetch_profiles)
                steam_->refresh_profiles(true, [this](bool) { ui_post([this] { tex_clear(); rescan("steam"); }); });
        }
        toast(TR("Список обновлён", "List refreshed"));
    }
    bx -= S(38);
    ImGui::SetCursorScreenPos(ImVec2(bx, py + S(6)));
    bool grid = store_.settings.grid_view;
    if (icon_button("##view", grid ? ICON_LAYOUT_LIST : ICON_LAYOUT_GRID, grid ? TR("Списком", "List view") : TR("Плиткой", "Grid view"))) {
        store_.settings.grid_view = !grid;
        store_.save_settings();
    }
    float sw = std::min(S(250), w * 0.3f);
    bx -= sw + S(10);
    ImGui::SetCursorScreenPos(ImVec2(bx, py + S(5)));
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
    search_box("##search", search_, sizeof(search_), TR("Поиск", "Search"), sw);

    // platform menu
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(6)));
    if (ImGui::BeginPopup("##platmenu")) {
        std::string nm = p->name();
        if (menu_item(ICON_PLAY, (std::string(TR("Запустить ", "Start ")) + nm).c_str())) toast(p->start_client().message);
        if (menu_item(ICON_POWER, (std::string(TR("Закрыть ", "Close ")) + nm).c_str())) {
            std::thread([this, p] {
                OpResult r = p->close_client();
                ui_post([this, r] { toast(r.message, !r.ok); });
            }).detach();
        }
        if (menu_item(ICON_FOLDER_OPEN, TR("Открыть папку клиента", "Open client folder"), nullptr, !p->data_folder().empty()))
            os::shell_open(p->data_folder());
        if (p->is_steam()) {
            ImGui::Separator();
            if (menu_item(ICON_DOWNLOAD, TR("Обновить аватары и VAC", "Refresh avatars & VAC"))) {
                steam_->refresh_profiles(true, [this](bool) { ui_post([this] { tex_clear(); rescan("steam"); toast(TR("Профили обновлены", "Profiles refreshed")); }); });
            }
        } else {
            ImGui::Separator();
            if (menu_item(ICON_ARCHIVE, TR("Папка сохранённых аккаунтов", "Saved accounts folder"))) {
                std::string dir = fsx::join(store_.dir, "platforms\\" + p->id());
                fsx::mkdirs(dir);
                os::shell_open(dir);
            }
        }
        ImGui::Separator();
        if (menu_item(ICON_SLIDERS_HORIZONTAL, TR("Настройки платформы", "Platform settings"))) {
            page_ = Page::Settings;
            settings_tab_ = p->is_steam() ? 1 : 2;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void App::draw_empty(float w, float h) {
    Platform* p = current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float cy = o.y + h * 0.32f;
    float cx = o.x + w * 0.5f;
    bool installed = p->installed();
    const char* icon = installed ? ICON_USERS : ICON_TRIANGLE_ALERT;
    ImVec2 ts = F_REG->CalcTextSizeA(S(40), FLT_MAX, 0, icon);
    dl->AddCircleFilled(ImVec2(cx, cy), S(44), P.accent_soft, 48);
    dl->AddText(F_REG, S(40), ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f + S(4)), P.text2, icon);
    std::string title, desc;
    if (!installed) {
        title = p->name() + TR(" не найден", " not found");
        desc = TR("Укажите путь к клиенту в настройках платформы.", "Set the client path in the platform settings.");
    } else if (p->is_steam()) {
        title = TR("Здесь появятся ваши аккаунты Steam", "Your Steam accounts will appear here");
        desc = TR("Войдите в аккаунт через Steam с галочкой «Запомнить меня» — он сразу окажется в списке.",
                  "Sign in through Steam with “Remember me” checked — it will show up here.");
    } else {
        title = TR("Сохранённых аккаунтов пока нет", "No saved accounts yet");
        desc = std::string(TR("Войдите в ", "Sign in to ")) + p->name() +
               TR(", затем нажмите «Сохранить текущий». Для следующего аккаунта — «Новый вход».",
                  ", then press “Save current”. For the next account use “New login”.");
    }
    ImVec2 t1 = F_SEMI->CalcTextSizeA(S(18), FLT_MAX, 0, title.c_str());
    dl->AddText(F_SEMI, S(18), ImVec2(cx - t1.x * 0.5f, cy + S(64)), P.text, title.c_str());
    float wrap = std::min(w - S(40), S(440));
    ImVec2 t2 = F_REG->CalcTextSizeA(S(kBase), FLT_MAX, wrap, desc.c_str());
    dl->AddText(F_REG, S(kBase), ImVec2(cx - t2.x * 0.5f, cy + S(96)), P.text2, desc.c_str(), nullptr, wrap);
    ImGui::SetCursorScreenPos(ImVec2(cx - S(90), cy + S(116) + t2.y));
    if (!installed) {
        if (pill_button(TR("Открыть настройки", "Open settings"), true, ImVec2(S(180), 0))) { page_ = Page::Settings; settings_tab_ = p->is_steam() ? 1 : 2; }
    } else if (p->is_steam()) {
        if (pill_button(TR("Открыть экран входа", "Open login screen"), true, ImVec2(S(180), 0), !busy_)) request_switch(cur_, SwitchRequest{});
    } else {
        if (pill_button(TR("Сохранить текущий", "Save current"), true, ImVec2(S(180), 0), !busy_)) open_dialog(Dlg::SaveCurrent, cur_, "");
    }
    ImGui::Dummy(ImVec2(0, S(10)));
}

bool App::handle_card_input(const std::string& pid, const Account& a, bool hovered) {
    if (!hovered) return false;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected_ = a.id;
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        SwitchRequest r;
        r.account_id = a.id;
        request_switch(pid, r);
        return true;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        selected_ = a.id;
        ctx_account_ = a.id;
        open_ctx_ = true;
    }
    return false;
}

void App::draw_grid(const std::vector<Account>& list, float avail) {
    float scale = store_.settings.card_scale;
    float cw = S(168) * scale, ch = S(214) * scale, gap = S(16);
    int cols = std::max(1, (int)((avail + gap) / (cw + gap)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    origin.y += S(6);
    bool streamer = store_.settings.streamer_mode;
    std::vector<std::string> order;
    for (auto& a : list) order.push_back(a.id);
    int move_from = -1, move_to = -1;

    for (int i = 0; i < (int)list.size(); ++i) {
        const Account& a = list[i];
        int col = i % cols, row = i / cols;
        ImVec2 p0(origin.x + col * (cw + gap), origin.y + row * (ch + gap));
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(a.id.c_str());
        ImGui::InvisibleButton("##card", ImVec2(cw, ch));
        bool hov = ImGui::IsItemHovered();
        ImGuiID iid = ImGui::GetItemID();
        // drag to reorder (only when not searching)
        if (!search_[0] && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload("VS_ACC", &i, sizeof(int));
            ImGui::EndDragDropSource();
        }
        if (!search_[0] && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("VS_ACC", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                dl->AddRect(ImVec2(p0.x - S(3), p0.y - S(3)), ImVec2(p0.x + cw + S(3), p0.y + ch + S(3)), with_alpha(P.accent, 0.5f), S(18), S(2));
                if (pl->IsDelivery()) { move_from = *(const int*)pl->Data; move_to = i; }
            }
            ImGui::EndDragDropTarget();
        }
        bool dragging_me = ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
        ImGui::PopID();
        handle_card_input(cur_, a, hov && !dragging_me);
        float hv = anim(iid, hov, 14.0f);
        bool sel = selected_ == a.id;
        bool busy_me = busy_ && busy_account_ == a.id && busy_platform_ == cur_;
        float lift = S(2) * hv;
        ImVec2 c0(p0.x, p0.y - lift), c1(p0.x + cw, p0.y + ch - lift);
        float rnd = S(16);
        if (hv > 0.01f) soft_shadow(dl, c0, c1, rnd, with_alpha(P.shadow, hv * 1.4f), S(14));
        dl->AddRectFilled(c0, c1, P.surface, rnd);
        dl->AddRect(c0, c1, sel ? P.selection : mix(P.border, P.border_strong, hv * 0.6f), rnd, sel ? S(1.6f) : S(1));

        // avatar
        float av = cw - S(44);
        ImVec2 ap(c0.x + (cw - av) * 0.5f, c0.y + S(18));
        std::string name = display_name(cur_, a);
        draw_avatar(dl, a.avatar_path, name, a.id, ap, av, 0.16f);
        if (busy_me) {
            dl->AddRectFilled(ap, ImVec2(ap.x + av, ap.y + av), with_alpha(P.surface, 0.72f), av * 0.16f);
            spinner(dl, ImVec2(ap.x + av * 0.5f, ap.y + av * 0.5f), av * 0.16f, P.text, S(2.5f));
        }
        // badges
        float bx = ap.x + S(6), by = ap.y + S(6);
        auto put_badge = [&](const char* t, ImU32 fg, ImU32 bg) {
            ImVec2 bs = badge_size(t, 10.5f);
            badge(dl, ImVec2(bx, by), t, fg, bg, 10.5f);
            bx += bs.x + S(4);
        };
        if (a.current) put_badge(TR("СЕЙЧАС", "ACTIVE"), P.ok, with_alpha(g_dark ? P.ok_soft : IM_COL32(255, 255, 255, 255), 0.94f));
        if (a.vac && store_.settings.steam_show_vac) put_badge("VAC", IM_COL32(255, 255, 255, 255), P.danger);
        if (a.limited && store_.settings.steam_show_limited) put_badge("LTD", IM_COL32(255, 255, 255, 255), P.warn);
        {
            std::lock_guard<std::recursive_mutex> lk(store_.mtx);
            auto& m = store_.meta[cur_][a.id];
            if (!m.note.empty()) {
                ImVec2 np(ap.x + av - S(26), ap.y + S(6));
                dl->AddRectFilled(np, ImVec2(np.x + S(20), np.y + S(20)), with_alpha(P.surface, 0.94f), S(6));
                dl->AddText(F_REG, S(12), ImVec2(np.x + S(4), np.y + S(4)), P.text2, ICON_STICKY_NOTE);
            }
        }
        // texts
        float ty = ap.y + av + S(12);
        text_clipped(dl, F_MED, kBase, ImVec2(c0.x + S(12), ty), cw - S(24), P.text, name, true);
        std::string sub = streamer ? std::string("•••••") : a.login;
        if (sub == name || sub.empty()) sub = a.last_login ? ago(a.last_login) : std::string();
        else if (a.last_login) sub += " · " + ago(a.last_login);
        text_clipped(dl, F_REG, kBase * 0.84f, ImVec2(c0.x + S(12), ty + S(20)), cw - S(24), P.text3, sub, true);

        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            std::string note;
            {
                std::lock_guard<std::recursive_mutex> lk(store_.mtx);
                note = store_.meta[cur_][a.id].note;
            }
            if (!note.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(S(280));
                ImGui::TextUnformatted(note.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }
    int rows = ((int)list.size() + cols - 1) / cols;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (ch + gap)));
    ImGui::Dummy(ImVec2(avail, S(8)));
    if (move_from >= 0 && move_to >= 0 && move_from != move_to) {
        std::string id = order[move_from];
        order.erase(order.begin() + move_from);
        order.insert(order.begin() + move_to, id);
        set_order(cur_, order);
    }
}

void App::draw_list(const std::vector<Account>& list, float avail) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool streamer = store_.settings.streamer_mode;
    float rh = S(64);
    std::vector<std::string> order;
    for (auto& a : list) order.push_back(a.id);
    int move_from = -1, move_to = -1;
    ImGui::Dummy(ImVec2(0, S(4)));
    for (int i = 0; i < (int)list.size(); ++i) {
        const Account& a = list[i];
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::PushID(a.id.c_str());
        ImGui::InvisibleButton("##row", ImVec2(avail, rh));
        bool hov = ImGui::IsItemHovered();
        ImGuiID iid = ImGui::GetItemID();
        if (!search_[0] && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload("VS_ACC", &i, sizeof(int));
            ImGui::EndDragDropSource();
        }
        if (!search_[0] && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("VS_ACC", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x + avail, p0.y), P.accent, S(2));
                if (pl->IsDelivery()) { move_from = *(const int*)pl->Data; move_to = i; }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        handle_card_input(cur_, a, hov);
        float hv = anim(iid, hov, 16.0f);
        bool sel = selected_ == a.id;
        ImVec2 p1(p0.x + avail, p0.y + rh - S(6));
        if (hv > 0 || sel) dl->AddRectFilled(p0, p1, with_alpha(P.surface, sel ? 1.0f : hv), S(12));
        if (sel) dl->AddRect(p0, p1, P.selection, S(12), S(1.4f));
        std::string name = display_name(cur_, a);
        float av = S(40);
        ImVec2 ap(p0.x + S(10), p0.y + (rh - S(6) - av) * 0.5f);
        draw_avatar(dl, a.avatar_path, name, a.id, ap, av, 0.22f);
        if (busy_ && busy_account_ == a.id && busy_platform_ == cur_) {
            dl->AddRectFilled(ap, ImVec2(ap.x + av, ap.y + av), with_alpha(P.surface, 0.7f), av * 0.22f);
            spinner(dl, ImVec2(ap.x + av * 0.5f, ap.y + av * 0.5f), av * 0.22f, P.text, S(2));
        }
        float tx = ap.x + av + S(14);
        dl->AddText(F_MED, S(kBase), ImVec2(tx, ap.y + S(2)), P.text, name.c_str());
        float nx = tx + F_MED->CalcTextSizeA(S(kBase), FLT_MAX, 0, name.c_str()).x + S(8);
        auto put_badge = [&](const char* t, ImU32 fg, ImU32 bg) {
            ImVec2 bs = badge_size(t, 10.0f);
            badge(dl, ImVec2(nx, ap.y + S(3)), t, fg, bg, 10.0f);
            nx += bs.x + S(4);
        };
        if (a.current) put_badge(TR("СЕЙЧАС", "ACTIVE"), P.ok, P.ok_soft);
        if (a.vac && store_.settings.steam_show_vac) put_badge("VAC", IM_COL32(255, 255, 255, 255), P.danger);
        if (a.limited && store_.settings.steam_show_limited) put_badge("LTD", IM_COL32(255, 255, 255, 255), P.warn);
        std::string sub = streamer ? std::string("•••••") : a.login;
        std::string note;
        {
            std::lock_guard<std::recursive_mutex> lk(store_.mtx);
            note = store_.meta[cur_][a.id].note;
        }
        if (!note.empty()) sub += (sub.empty() ? "" : "  ·  ") + note;
        text_clipped(dl, F_REG, kBase * 0.88f, ImVec2(tx, ap.y + S(22)), avail - (tx - p0.x) - S(200), P.text3, sub);
        std::string when = a.last_login ? ago(a.last_login) : std::string();
        ImVec2 ws = F_REG->CalcTextSizeA(S(kBase * 0.85f), FLT_MAX, 0, when.c_str());
        float right = p0.x + avail - S(14);
        if (hov && !busy_) {
            // quick sign-in button
            float bw = S(92), bh = S(30);
            ImVec2 bp(right - bw, p0.y + (rh - S(6) - bh) * 0.5f);
            ImGui::SetCursorScreenPos(bp);
            ImGui::PushID(a.id.c_str());
            if (pill_button(TR("Войти", "Sign in"), true, ImVec2(bw, bh))) {
                SwitchRequest r;
                r.account_id = a.id;
                request_switch(cur_, r);
            }
            ImGui::PopID();
            right -= bw + S(14);
        }
        dl->AddText(F_REG, S(kBase * 0.85f), ImVec2(right - ws.x, p0.y + (rh - S(6) - ws.y) * 0.5f), P.text3, when.c_str());
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rh));
    }
    if (move_from >= 0 && move_to >= 0 && move_from != move_to) {
        std::string id = order[move_from];
        order.erase(order.begin() + move_from);
        order.insert(order.begin() + move_to, id);
        set_order(cur_, order);
    }
}

// ================================================================ bottom bar
void App::draw_bottom_bar(float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Platform* p = current();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), P.bg);
    dl->AddLine(ImVec2(x + S(28), y), ImVec2(x + w - S(28), y), P.border, S(1));
    float cy = y + h * 0.5f;
    const Account* sel = nullptr;
    for (auto& a : accounts_[cur_])
        if (a.id == selected_) sel = &a;

    float lx = x + S(28);
    if (busy_ && busy_platform_ == cur_) {
        spinner(dl, ImVec2(lx + S(14), cy), S(10), P.text, S(2.2f));
        std::string step;
        {
            std::lock_guard<std::mutex> lk(step_mtx_);
            step = busy_step_;
        }
        char t[32];
        snprintf(t, sizeof(t), "  %.1f %s", ImGui::GetTime() - busy_started_, TR("с", "s"));
        dl->AddText(F_MED, S(kBase), ImVec2(lx + S(36), cy - S(9)), P.text, step.c_str());
        float sx = F_MED->CalcTextSizeA(S(kBase), FLT_MAX, 0, step.c_str()).x;
        dl->AddText(F_REG, S(kBase), ImVec2(lx + S(36) + sx, cy - S(9)), P.text3, t);
    } else if (sel) {
        float av = S(36);
        std::string name = display_name(cur_, *sel);
        draw_avatar(dl, sel->avatar_path, name, sel->id, ImVec2(lx, cy - av * 0.5f), av, 0.24f);
        dl->AddText(F_MED, S(kBase), ImVec2(lx + av + S(12), cy - S(17)), P.text, name.c_str());
        std::string sub;
        if (p->is_steam()) sub = store_.settings.streamer_mode ? std::string("SteamID ••••") : sel->login + " · " + sel->id;
        else sub = store_.settings.streamer_mode ? std::string("•••") : sel->login;
        text_clipped(dl, F_REG, kBase * 0.85f, ImVec2(lx + av + S(12), cy + S(2)), w * 0.36f, P.text3, sub);
    } else {
        dl->AddText(F_REG, S(kBase * 0.95f), ImVec2(lx, cy - S(9)), P.text3,
                    TR("Выберите аккаунт · двойной клик — войти", "Pick an account · double-click to sign in"));
    }

    // right side buttons
    float rx = x + w - S(28);
    float bh = S(40);
    float by = cy - bh * 0.5f;
    bool can = sel && !busy_;
    float bw = S(132);
    rx -= bw;
    ImGui::SetCursorScreenPos(ImVec2(rx, by));
    if (pill_button(TR("Войти", "Sign in"), true, ImVec2(bw, bh), can, ICON_LOG_IN)) {
        SwitchRequest r;
        r.account_id = selected_;
        if (p->is_steam() && state_choice_ > 0) r.state = kStates[state_choice_ - 1];
        request_switch(cur_, r);
    }
    if (p->is_steam()) {
        // state picker
        const char* label = state_choice_ == 0 ? TR("Статус: по умолчанию", "Status: default") : steam_state_label(kStates[state_choice_ - 1]);
        ImGui::PushFont(F_MED, kBase * 0.95f);
        float sw2 = ImGui::CalcTextSize(label).x + S(52);
        ImGui::PopFont();
        rx -= sw2 + S(10);
        ImGui::SetCursorScreenPos(ImVec2(rx, by));
        std::string l = std::string(label) + "  " ICON_CHEVRON_DOWN;
        if (pill_button(l.c_str(), false, ImVec2(sw2, bh), !busy_)) ImGui::OpenPopup("##statepick");
        ImGui::SetNextWindowPos(ImVec2(rx, by - S(8)), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
        if (ImGui::BeginPopup("##statepick")) {
            if (ImGui::Selectable(TR("По умолчанию", "Default"), state_choice_ == 0)) state_choice_ = 0;
            ImGui::Separator();
            for (int i = 0; i < kStateCount; ++i)
                if (ImGui::Selectable(steam_state_label(kStates[i]), state_choice_ == i + 1)) state_choice_ = i + 1;
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        float nb = S(40);
        rx -= nb + S(10);
        ImGui::SetCursorScreenPos(ImVec2(rx, by));
        if (pill_button(ICON_USER_PLUS, false, ImVec2(nb, bh), !busy_)) request_switch(cur_, SwitchRequest{});
        tooltip(TR("Добавить аккаунт — откроет экран входа Steam", "Add account — opens the Steam login screen"));
    } else {
        float b2 = S(150);
        rx -= b2 + S(10);
        ImGui::SetCursorScreenPos(ImVec2(rx, by));
        if (pill_button(TR("Сохранить текущий", "Save current"), false, ImVec2(b2, bh), !busy_, ICON_SAVE)) open_dialog(Dlg::SaveCurrent, cur_, "");
        tooltip(TR("Сохранить аккаунт, в который сейчас выполнен вход", "Save the account that is signed in right now"));
        float nb = S(40);
        rx -= nb + S(10);
        ImGui::SetCursorScreenPos(ImVec2(rx, by));
        if (pill_button(ICON_USER_PLUS, false, ImVec2(nb, bh), !busy_)) open_dialog(Dlg::NewLogin, cur_, "");
        tooltip(TR("Новый вход — выйти и открыть экран входа", "New login — sign out and open the login screen"));
    }
}

// ================================================================ context menu
void App::account_menu_items(const std::string& pid, const Account& a) {
    Platform* p = platform(pid);
    if (!p) return;
    bool steam = p->is_steam();
    if (menu_item(ICON_LOG_IN, TR("Войти", "Sign in"), "Enter", !busy_)) {
        SwitchRequest r;
        r.account_id = a.id;
        request_switch(pid, r);
    }
    if (steam) {
        if (begin_menu(ICON_GHOST, TR("Войти со статусом", "Sign in as"), !busy_)) {
            for (int i = 1; i < kStateCount; ++i) {
                if (ImGui::MenuItem(steam_state_label(kStates[i]))) {
                    SwitchRequest r;
                    r.account_id = a.id;
                    r.state = kStates[i];
                    request_switch(pid, r);
                }
            }
            ImGui::EndMenu();
        }
        auto games = steam_->games();
        if (begin_menu(ICON_GAMEPAD_2, TR("Войти и запустить", "Sign in & launch"), !busy_ && !games.empty())) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
            int shown = 0;
            for (auto& g : games) {
                ImTextureID ic = tex_file(g.icon_path, 32, 0.2f);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                std::string label = std::string("      ") + g.name + "##" + g.appid;
                if (ImGui::MenuItem(label.c_str())) {
                    SwitchRequest r;
                    r.account_id = a.id;
                    r.app_id = g.appid;
                    request_switch(pid, r);
                }
                if (ic) {
                    float s = ImGui::GetTextLineHeight();
                    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(ic), ImVec2(cp.x, cp.y + S(1)), ImVec2(cp.x + s, cp.y + s + S(1)));
                }
                if (++shown > 60) break;
            }
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
    }
    ImGui::Separator();
    if (begin_menu(ICON_COPY, TR("Копировать", "Copy"))) {
        if (ImGui::MenuItem(TR("Логин", "Login"), nullptr, false, !a.login.empty())) { os::clipboard_set(a.login); toast(TR("Скопировано", "Copied")); }
        if (ImGui::MenuItem(TR("Имя", "Name"))) { os::clipboard_set(display_name(pid, a)); toast(TR("Скопировано", "Copied")); }
        if (steam) {
            uint64_t v = 0;
            parse_u64(a.id, v);
            if (ImGui::MenuItem("SteamID64")) { os::clipboard_set(a.id); toast(TR("Скопировано", "Copied")); }
            if (ImGui::MenuItem("SteamID3")) { os::clipboard_set(steam::steam3(v)); toast(TR("Скопировано", "Copied")); }
            if (ImGui::MenuItem("SteamID")) { os::clipboard_set(steam::steam2(v)); toast(TR("Скопировано", "Copied")); }
            if (ImGui::MenuItem(TR("Ссылка на профиль", "Profile link"))) { os::clipboard_set(steam::profile_url(a.id)); toast(TR("Скопировано", "Copied")); }
        } else {
            if (ImGui::MenuItem("ID")) { os::clipboard_set(a.id); toast(TR("Скопировано", "Copied")); }
        }
        ImGui::EndMenu();
    }
    if (steam) {
        if (menu_item(ICON_EXTERNAL_LINK, TR("Открыть профиль", "Open profile"))) os::shell_open(steam::profile_url(a.id));
        if (menu_item(ICON_FOLDER_OPEN, TR("Папка userdata", "userdata folder"), nullptr, fsx::exists(steam_->userdata_dir(a.id))))
            os::shell_open(steam_->userdata_dir(a.id));
    } else {
        std::string dir = fsx::join(store_.dir, "platforms\\" + pid + "\\" + sanitize_filename(a.id));
        if (menu_item(ICON_FOLDER_OPEN, TR("Папка сохранения", "Saved data folder"), nullptr, fsx::exists(dir))) os::shell_open(dir);
        if (menu_item(ICON_IMAGE, TR("Сменить картинку…", "Change picture…"))) {
            std::string f = os::pick_image_file(main_hwnd());
            img::Image im;
            if (!f.empty() && img::load_file(f, im)) {
                im = img::resize(img::crop_square(im), 256, 256);
                auto png = img::encode_png(im);
                std::string out = fsx::join(dir, "avatar.png");
                fsx::write_file(out, std::string(png.begin(), png.end()));
                fsx::remove_file(fsx::join(dir, "avatar.jpg"));
                tex_forget(out);
                rescan(pid);
            } else if (!f.empty()) {
                toast(TR("Не удалось открыть картинку", "Could not open the image"), true);
            }
        }
        if (menu_item(ICON_REFRESH_CW, TR("Перезаписать текущим входом", "Overwrite with current login"), nullptr, !busy_)) {
            std::string nm = display_name(pid, a);
            save_current(pid, nm, a.id);
        }
        tooltip(TR("Если вход в этот аккаунт выполнен сейчас — обновит сохранённые данные", "If this account is signed in now — refreshes its saved data"));
    }
    ImGui::Separator();
    if (menu_item(ICON_PENCIL, TR("Переименовать…", "Rename…"), "F2")) open_dialog(Dlg::Rename, pid, a.id);
    if (menu_item(ICON_STICKY_NOTE, TR("Заметка…", "Note…"))) open_dialog(Dlg::Note, pid, a.id);
    if (steam) {
        int cur_def;
        {
            std::lock_guard<std::recursive_mutex> lk(store_.mtx);
            cur_def = store_.meta[pid][a.id].default_state;
        }
        if (begin_menu(ICON_CIRCLE_DOT, TR("Статус по умолчанию", "Default status"))) {
            if (ImGui::MenuItem(TR("Как в настройках", "Use settings"), nullptr, cur_def == kStateDefault)) {
                std::lock_guard<std::recursive_mutex> lk(store_.mtx);
                store_.meta[pid][a.id].default_state = kStateDefault;
                store_.save_meta();
            }
            for (int i = 0; i < kStateCount; ++i) {
                if (ImGui::MenuItem(steam_state_label(kStates[i]), nullptr, cur_def == kStates[i])) {
                    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
                    store_.meta[pid][a.id].default_state = kStates[i];
                    store_.save_meta();
                }
            }
            ImGui::EndMenu();
        }
    }
    if (begin_menu(ICON_SQUARE_PLUS, TR("Ярлык на рабочий стол", "Desktop shortcut"))) {
        if (ImGui::MenuItem(TR("Обычный вход", "Regular sign-in"))) create_shortcut(pid, a, -1);
        if (steam) {
            for (int i = 1; i < kStateCount - 1; ++i)
                if (ImGui::MenuItem(steam_state_label(kStates[i]))) create_shortcut(pid, a, kStates[i]);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (menu_item(ICON_TRASH_2, steam ? TR("Забыть аккаунт…", "Forget account…") : TR("Удалить…", "Delete…"), "Del", !busy_, true))
        open_dialog(Dlg::Forget, pid, a.id);
}

void App::draw_context_menu() {
    if (open_ctx_) {
        ImGui::OpenPopup("##accmenu");
        open_ctx_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(7)));
    if (ImGui::BeginPopup("##accmenu")) {
        const Account* a = nullptr;
        for (auto& x : accounts_[cur_])
            if (x.id == ctx_account_) a = &x;
        if (a) {
            Account copy = *a;
            ImGui::PushFont(F_SEMI, kBase * 0.9f);
            ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
            ImGui::TextUnformatted(display_name(cur_, copy).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Separator();
            account_menu_items(cur_, copy);
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// ================================================================ dialogs
static bool begin_dialog(const char* id, const char* title, float width) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(S(width), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26), S(22)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, S(1));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, P.surface);
    bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (open) {
        ImGui::PushFont(F_SEMI, kBase * 1.25f);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    return open;
}

static void dialog_text(const std::string& t) {
    ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// Two buttons right-aligned; returns 1 = primary, 2 = secondary
static int dialog_buttons(const char* primary, const char* secondary, bool primary_enabled = true, bool danger = false) {
    ImGui::Dummy(ImVec2(0, S(10)));
    float w = ImGui::GetContentRegionAvail().x;
    float bw = S(132);
    ImVec2 p = ImGui::GetCursorScreenPos();
    int r = 0;
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw * 2 - S(10), p.y));
    if (secondary && pill_button(secondary, false, ImVec2(bw, S(38)))) r = 2;
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw, p.y));
    if (danger) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.danger);
    }
    if (pill_button(primary, true, ImVec2(bw, S(38)), primary_enabled)) r = 1;
    if (danger) ImGui::PopStyleColor();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) r = 2;
    return r;
}

static void dialog_input(const char* id, char* buf, size_t n, const char* hint, bool multiline = false) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(9)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, g_dark ? P.bg : P.bg);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (multiline) ImGui::InputTextMultiline(id, buf, n, ImVec2(-1, S(110)));
    else ImGui::InputTextWithHint(id, hint, buf, n);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void App::draw_dialogs() {
    const char* ids[] = {"", "##dlg_rename", "##dlg_note", "##dlg_forget", "##dlg_save", "##dlg_new", "##dlg_admin", "##dlg_about", "##dlg_short", "##dlg_game"};
    if (dlg_ != Dlg::None && dlg_open_req_) {
        ImGui::OpenPopup(ids[(int)dlg_]);
        dlg_open_req_ = false;
    }
    if (dlg_ == Dlg::None) return;
    Platform* p = platform(dlg_platform_);
    std::string acc_name;
    for (auto& a : accounts_[dlg_platform_])
        if (a.id == dlg_account_) acc_name = display_name(dlg_platform_, a);
    bool close = false;

    switch (dlg_) {
        case Dlg::Rename:
            if (begin_dialog(ids[1], TR("Переименовать", "Rename"), 420)) {
                dialog_text(TR("Имя видно только в свитчере. Пусто — вернуть исходное.", "Shown only in the switcher. Leave empty to reset."));
                ImGui::Dummy(ImVec2(0, S(6)));
                dialog_input("##name", dlg_buf_, 128, acc_name.c_str());
                int r = dialog_buttons(TR("Сохранить", "Save"), TR("Отмена", "Cancel"));
                if (r == 1 || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
                    store_.meta[dlg_platform_][dlg_account_].custom_name = trim(dlg_buf_);
                    store_.save_meta();
                    close = true;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case Dlg::Note:
            if (begin_dialog(ids[2], (std::string(TR("Заметка · ", "Note · ")) + acc_name).c_str(), 460)) {
                dialog_input("##note", dlg_buf_, sizeof(dlg_buf_), "", true);
                int r = dialog_buttons(TR("Сохранить", "Save"), TR("Отмена", "Cancel"));
                if (r == 1) {
                    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
                    store_.meta[dlg_platform_][dlg_account_].note = trim(dlg_buf_);
                    store_.save_meta();
                    close = true;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case Dlg::Forget:
            if (begin_dialog(ids[3], p && p->is_steam() ? TR("Забыть аккаунт?", "Forget account?") : TR("Удалить аккаунт?", "Delete account?"), 440)) {
                std::string t = acc_name + "\n\n";
                if (p && p->is_steam())
                    t += TR("Аккаунт уберётся из списка Steam и свитчера. Игры, настройки и userdata не удаляются. "
                            "Чтобы вернуть — войдите в него через Steam.",
                            "The account is removed from Steam's and the switcher's list. Games, settings and userdata stay. "
                            "Sign in through Steam to bring it back.");
                else
                    t += TR("Сохранённые данные входа этого аккаунта будут удалены из свитчера. Сам аккаунт не пострадает.",
                            "Saved login data for this account will be removed from the switcher. The account itself is not affected.");
                dialog_text(t);
                int r = dialog_buttons(p && p->is_steam() ? TR("Забыть", "Forget") : TR("Удалить", "Delete"), TR("Отмена", "Cancel"));
                if (r == 1) { forget(dlg_platform_, dlg_account_); close = true; }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case Dlg::SaveCurrent:
            if (begin_dialog(ids[4], TR("Сохранить текущий аккаунт", "Save current account"), 460)) {
                dialog_text(std::string(TR("Убедитесь, что в ", "Make sure you are signed in to ")) + (p ? p->name() : "") +
                            TR(" выполнен вход в нужный аккаунт. Клиент будет закрыт, чтобы данные сохранились полностью.",
                               " with the right account. The client will be closed so the data is saved completely."));
                ImGui::Dummy(ImVec2(0, S(6)));
                dialog_input("##savename", dlg_buf_, 128, TR("Название, например «Основной»", "Name, e.g. “Main”"));
                int r = dialog_buttons(TR("Сохранить", "Save"), TR("Отмена", "Cancel"), dlg_buf_[0] != 0);
                if ((r == 1 || (ImGui::IsKeyPressed(ImGuiKey_Enter) && dlg_buf_[0])) && dlg_buf_[0]) {
                    save_current(dlg_platform_, trim(dlg_buf_), "");
                    close = true;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case Dlg::NewLogin:
            if (begin_dialog(ids[5], TR("Новый вход", "New login"), 460)) {
                dialog_text(std::string(TR("Клиент ", "")) + (p ? p->name() : "") +
                            TR(" закроется, текущий вход будет сброшен, и откроется экран входа. Сохраните текущий аккаунт заранее, если ещё не сделали этого.",
                               " will close, the current sign-in is cleared and the login screen opens. Save the current account first if you haven't yet."));
                int r = dialog_buttons(TR("Продолжить", "Continue"), TR("Отмена", "Cancel"));
                if (r == 1) { run_switch(dlg_platform_, SwitchRequest{}); close = true; }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case Dlg::Admin:
            if (begin_dialog(ids[6], TR("Нужны права администратора", "Administrator rights needed"), 460)) {
                dialog_text(admin_msg_);
                int r = dialog_buttons(TR("Перезапустить", "Restart"), TR("Отмена", "Cancel"));
                if (r == 1) {
                    if (os::relaunch_self_elevated("--show")) host_quit();
                    close = true;
                }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        case Dlg::GameRunning:
            if (begin_dialog(ids[9], TR("Сейчас запущена игра", "A game is running"), 460)) {
                dialog_text(pending_game_ + "\n\n" +
                            TR("Чтобы сменить аккаунт, Steam будет закрыт — игра тоже закроется или потеряет соединение. Продолжить?",
                               "Steam will be closed to switch accounts — the game will close or lose connection. Continue?"));
                int r = dialog_buttons(TR("Переключить", "Switch"), TR("Отмена", "Cancel"));
                if (r == 1) { run_switch(dlg_platform_, pending_req_); close = true; }
                if (r == 2) close = true;
                if (close) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            } else close = true;
            break;
        default:
            close = true;
            break;
    }
    if (close) dlg_ = Dlg::None;
}

// ================================================================ toasts
void App::draw_toasts() {
    if (toasts_.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
    float y = io.DisplaySize.y - S(84);
    for (int i = (int)toasts_.size() - 1; i >= 0; --i) {
        Toast& t = toasts_[i];
        if (t.t0 <= 0) t.t0 = now;
        double age = now - t.t0;
        double life = t.error ? 5.0 : 3.0;
        float a = (float)std::min(1.0, std::min(age / 0.15, (life - age) / 0.35));
        if (a <= 0) continue;
        ImFont* f = F_MED;
        float fs = S(kBase * 0.95f);
        float maxw = S(440);
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, maxw, t.text.c_str());
        float w = ts.x + S(48), h = ts.y + S(22);
        float x = io.DisplaySize.x - w - S(28);
        float yy = y - h + (1 - a) * S(8);
        ImU32 bg = t.error ? P.danger : (g_dark ? P.surface_active : P.accent);
        ImU32 fg = t.error || !g_dark ? IM_COL32(250, 248, 245, 255) : P.text;
        soft_shadow(dl, ImVec2(x, yy), ImVec2(x + w, yy + h), h * 0.5f, with_alpha(P.shadow, a * 1.5f), S(10));
        dl->AddRectFilled(ImVec2(x, yy), ImVec2(x + w, yy + h), with_alpha(bg, a), S(14));
        dl->AddText(F_REG, fs, ImVec2(x + S(16), yy + S(11)), with_alpha(fg, a), t.error ? ICON_TRIANGLE_ALERT : ICON_CHECK);
        dl->AddText(f, fs, ImVec2(x + S(38), yy + S(11)), with_alpha(fg, a), t.text.c_str(), nullptr, maxw);
        y = yy - S(10);
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return now - t.t0 > (t.error ? 5.0 : 3.0); }),
                  toasts_.end());
}

}  // namespace vs
