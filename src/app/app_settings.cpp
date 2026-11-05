// vsemvamdam-switcher — settings page
#include <algorithm>

#include "../core/fsx.h"
#include "../core/i18n.h"
#include "../core/str.h"
#include "../os/win.h"
#include "../ui/icons.h"
#include "../ui/textures.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../version.h"
#include "app.h"
#include "basic_platform.h"
#include "imgui.h"
#include "steam_platform.h"

namespace vs {

using namespace ui;

static bool combo_row(const char* id, int* v, const char* const* items, int n, float w) {
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(10));
    bool changed = false;
    if (ImGui::BeginCombo(id, items[std::clamp(*v, 0, n - 1)])) {
        for (int i = 0; i < n; ++i)
            if (ImGui::Selectable(items[i], *v == i)) { *v = i; changed = true; }
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar(2);
    return changed;
}

static bool text_row(const char* id, char* buf, size_t n, const char* hint, float w) {
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(8)));
    bool r = ImGui::InputTextWithHint(id, hint, buf, n, ImGuiInputTextFlags_EnterReturnsTrue);
    bool deact = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleVar();
    return r || deact;
}

void App::draw_settings(float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float px = x + S(28);
    dl->AddText(F_SEMI, S(27), ImVec2(px, y + S(24)), P.text, TR("Настройки", "Settings"));
    ImGui::SetCursorScreenPos(ImVec2(px, y + S(70)));
    const char* tabs[] = {TR("Общие", "General"), "Steam", TR("Платформы", "Platforms"), TR("О программе", "About")};
    segmented("##tabs", &settings_tab_, tabs, 4);

    float top = y + S(118);
    ImGui::SetCursorScreenPos(ImVec2(x, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(10)));
    ImGui::BeginChild("##settings_body", ImVec2(w, h - (top - y)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    float maxw = std::min(ImGui::GetContentRegionAvail().x, S(760));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(maxw);
    ImGui::BeginChild("##settings_col", ImVec2(maxw, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
    switch (settings_tab_) {
        case 0: draw_settings_general(); break;
        case 1: draw_settings_steam(); break;
        case 2: draw_settings_platforms(); break;
        default: draw_settings_about(); break;
    }
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, S(30)));
    ImGui::EndChild();
}

void App::draw_settings_general() {
    std::lock_guard<std::recursive_mutex> settings_lock(store_.mtx);  // worker threads read settings concurrently
    auto& s = store_.settings;
    bool dirty = false;
    float cw = S(230);

    setting_row_begin(TR("Язык интерфейса", "Language"), nullptr, cw);
    {
        int v = s.language == "en" ? 1 : 0;
        const char* it[] = {"Русский", "English"};
        if (combo_row("##lang", &v, it, 2, cw)) { s.language = v ? "en" : "ru"; apply_language(); dirty = true; }
    }
    setting_row_end();

    setting_row_begin(TR("Тема", "Theme"), nullptr, cw);
    {
        int v = s.theme == Theme::Dark ? 0 : s.theme == Theme::Light ? 1 : 2;  // Dark, Light, System
        const char* it[] = {TR("Тёмная", "Dark"), TR("Светлая", "Light"), TR("Как в Windows", "System")};
        if (combo_row("##theme", &v, it, 3, cw)) { s.theme = v == 0 ? Theme::Dark : v == 1 ? Theme::Light : Theme::System; dirty = true; }
    }
    setting_row_end();

    setting_row_begin(TR("После переключения", "After switching"), TR("Что делать с окном свитчера, когда вход выполнен", "What to do with this window after a switch"), cw);
    {
        int v = (int)s.after_switch;
        const char* it[] = {TR("Ничего", "Nothing"), TR("Свернуть", "Minimize"), TR("Убрать в трей", "Hide to tray"), TR("Закрыть", "Quit")};
        if (combo_row("##after", &v, it, 4, cw)) { s.after_switch = (AfterSwitch)v; dirty = true; }
    }
    setting_row_end();

    setting_row_begin(TR("Крестик сворачивает в трей", "Close button hides to tray"), TR("Свитчер остаётся в трее — переключайтесь правым кликом по иконке", "Stay in the tray — switch with a right-click on the icon"), S(40));
    if (toggle("##ctt", &s.close_to_tray)) dirty = true;
    setting_row_end();

    setting_row_begin(TR("Запускать вместе с Windows", "Start with Windows"), TR("Стартует свёрнутым в трей", "Starts hidden in the tray"), S(40));
    {
        bool v = s.run_at_startup;
        if (toggle("##startup", &v)) {
            s.run_at_startup = v;
            os::set_run_at_startup("vsemvamdam-switcher", "\"" + os::self_exe_path() + "\" --tray", v);
            dirty = true;
        }
    }
    setting_row_end();

    setting_row_begin(TR("Открываться свёрнутым", "Start minimized"), nullptr, S(40));
    if (toggle("##startmin", &s.start_minimized)) dirty = true;
    setting_row_end();

    setting_row_begin(TR("Режим стримера", "Streamer mode"), TR("Скрывает логины и SteamID в интерфейсе", "Hides logins and SteamIDs in the UI"), S(40));
    if (toggle("##streamer", &s.streamer_mode)) dirty = true;
    setting_row_end();

    setting_row_begin(TR("Размер карточек", "Card size"), nullptr, cw);
    {
        const char* it[] = {TR("Мелкие", "Small"), TR("Средние", "Medium"), TR("Крупные", "Large")};
        const float vals[] = {0.85f, 1.0f, 1.2f};
        int v = s.card_scale < 0.92f ? 0 : s.card_scale > 1.1f ? 2 : 1;
        if (segmented("##cards", &v, it, 3, cw)) { s.card_scale = vals[v]; dirty = true; }
    }
    setting_row_end();

    setting_row_begin(TR("Аккаунтов в меню трея", "Accounts in tray menu"), TR("На каждую платформу, последние использованные", "Per platform, most recently used"), cw);
    {
        const char* it[] = {"3", "5", "8", "12"};
        const int vals[] = {3, 5, 8, 12};
        int v = s.tray_accounts <= 3 ? 0 : s.tray_accounts <= 5 ? 1 : s.tray_accounts <= 8 ? 2 : 3;
        if (combo_row("##trayn", &v, it, 4, cw)) { s.tray_accounts = vals[v]; dirty = true; }
    }
    setting_row_end();

    setting_row_begin(TR("Папка данных", "Data folder"), store_.dir.c_str(), S(150));
    if (pill_button(TR("Открыть", "Open"), false, ImVec2(S(150), S(34)))) os::shell_open(store_.dir);
    setting_row_end();

    if (dirty) store_.save_settings();
}

void App::draw_settings_steam() {
    std::lock_guard<std::recursive_mutex> settings_lock(store_.mtx);  // worker threads read settings concurrently
    auto& s = store_.settings;
    bool dirty = false;
    float cw = S(260);
    if (ImGui::IsWindowAppearing() || path_buf_[0] == 0) {
        if (path_buf_[0] == 0) snprintf(path_buf_, sizeof(path_buf_), "%s", s.steam_path.c_str());
        snprintf(args_buf_, sizeof(args_buf_), "%s", s.steam_args.c_str());
    }

    setting_row_begin(TR("Папка Steam", "Steam folder"), (std::string(TR("Сейчас: ", "Detected: ")) + steam_->root()).c_str(), cw);
    if (text_row("##steampath", path_buf_, sizeof(path_buf_), TR("Автоопределение", "Auto-detect"), cw)) {
        s.steam_path = trim(path_buf_);
        dirty = true;
        rescan("steam");
    }
    setting_row_end();

    setting_row_begin(TR("Статус при входе", "Status on sign-in"), TR("Можно переопределить для каждого аккаунта в его меню", "Can be overridden per account"), cw);
    {
        const int vals[] = {kStateKeep, 1, 3, 7, 0, kStateOfflineMode};
        int idx = 0;
        for (int i = 0; i < 6; ++i)
            if (vals[i] == s.steam_default_state) idx = i;
        const char* it[6];
        for (int i = 0; i < 6; ++i) it[i] = steam_state_label(vals[i]);
        if (combo_row("##defstate", &idx, it, 6, cw)) { s.steam_default_state = vals[idx]; dirty = true; }
    }
    setting_row_end();

    setting_row_begin(TR("Как закрывать Steam", "How to close Steam"),
                      TR("«Быстро» — штатный выход и добивание через 2,5 с. «Мгновенно» — сразу завершает процессы.",
                         "“Fast” — regular exit, force after 2.5 s. “Instant” — kills processes right away."),
                      cw);
    {
        int v = s.steam_close == SteamClose::Fast ? 0 : s.steam_close == SteamClose::Instant ? 1 : 2;
        const char* it[] = {TR("Быстро", "Fast"), TR("Мгновенно", "Instant"), TR("Аккуратно (до 15 с)", "Gentle (up to 15 s)")};
        if (combo_row("##close", &v, it, 3, cw)) { s.steam_close = v == 0 ? SteamClose::Fast : v == 1 ? SteamClose::Instant : SteamClose::Gentle; dirty = true; }
    }
    setting_row_end();

    setting_row_begin(TR("Параметры запуска Steam", "Steam launch options"), TR("Например: -silent (без окна) или -vgui", "E.g. -silent (no window) or -vgui"), cw);
    if (text_row("##steamargs", args_buf_, sizeof(args_buf_), "-silent", cw)) { s.steam_args = trim(args_buf_); dirty = true; }
    setting_row_end();

    setting_row_begin(TR("Запускать Steam после переключения", "Start Steam after switching"), nullptr, S(40));
    if (toggle("##steamlaunch", &s.steam_launch)) dirty = true;
    setting_row_end();

    setting_row_begin(TR("Загружать аватары и статус VAC", "Load avatars and VAC status"),
                      TR("Публичные данные профиля с steamcommunity.com, раз в сутки", "Public profile data from steamcommunity.com, once a day"), S(40));
    if (toggle("##fetch", &s.steam_fetch_profiles)) dirty = true;
    setting_row_end();

    setting_row_begin(TR("Показывать VAC", "Show VAC badge"), nullptr, S(40));
    if (toggle("##vac", &s.steam_show_vac)) dirty = true;
    setting_row_end();

    setting_row_begin(TR("Показывать ограниченные аккаунты (LTD)", "Show limited accounts (LTD)"), nullptr, S(40));
    if (toggle("##ltd", &s.steam_show_limited)) dirty = true;
    setting_row_end();

    if (dirty) store_.save_settings();
}

void App::draw_settings_platforms() {
    std::lock_guard<std::recursive_mutex> settings_lock(store_.mtx);  // worker threads read settings concurrently
    auto& s = store_.settings;
    bool dirty = false;
    ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(TR("Включите клиенты, которыми пользуетесь. Для них свитчер сохраняет данные входа каждого аккаунта и подменяет их при переключении. "
                              "Пароли не сохраняются.",
                              "Enable the clients you use. The switcher saves each account's login data and swaps it when switching. Passwords are never stored."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(12)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (auto& up : all_) {
        Platform* p = up.get();
        if (p->is_steam()) continue;
        auto& pp = s.platforms[p->id()];
        ImGui::PushID(p->id().c_str());
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool inst = p->installed();
        ImTextureID ic = tex_exe_icon(p->exe_path(), 32);
        if (ic) dl->AddImage(ImTextureRef(ic), ImVec2(pos.x, pos.y + S(4)), ImVec2(pos.x + S(24), pos.y + S(28)));
        else dl->AddRectFilled(ImVec2(pos.x, pos.y + S(4)), ImVec2(pos.x + S(24), pos.y + S(28)), P.accent_soft, S(6));
        ImGui::Indent(S(38));
        float avail = ImGui::GetContentRegionAvail().x;
        setting_row_begin(p->name().c_str(), inst ? TR("Установлен", "Installed") : TR("Не найден на этом компьютере", "Not found on this PC"), S(40));
        bool en = pp.enabled;
        if (toggle("##en", &en)) {
            pp.enabled = en;
            dirty = true;
            if (en) rescan(p->id());
            if (!en && cur_ == p->id()) cur_ = "steam";
        }
        (void)avail;
        setting_row_end();
        if (pp.enabled) {
            // path override
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", pp.exe_path.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
            ImGui::TextUnformatted(TR("Путь к клиенту (если не найден автоматически)", "Client path (if not detected)"));
            ImGui::PopStyleColor();
            if (text_row("##path", buf, sizeof(buf), p->exe_path().c_str(), ImGui::GetContentRegionAvail().x)) {
                pp.exe_path = trim(buf);
                dirty = true;
            }
            bool la = pp.launch_after;
            if (toggle("##launch", &la)) { pp.launch_after = la; dirty = true; }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("Запускать клиент после переключения", "Start the client after switching"));
            std::string h = p->help();
            if (!h.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, P.text3);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(h.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, S(14)));
        }
        ImGui::Unindent(S(38));
        ImGui::PopID();
    }
    if (dirty) store_.save_settings();
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::PushStyleColor(ImGuiCol_Text, P.text3);
    ImGui::PushTextWrapPos(0);
    std::string tip = std::string(TR("Свои платформы можно описать в файле ", "Custom platforms can be defined in ")) + fsx::join(store_.dir, "platforms.json") +
                      TR(" (тот же формат, что встроенный каталог).", " (same format as the built-in catalogue).");
    ImGui::TextUnformatted(tip.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void App::draw_settings_about() {
    ImGui::PushFont(F_SEMI, kBase * 1.3f);
    ImGui::TextUnformatted("vsemvamdam switcher " VS_VER_STRING);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(TR("Быстрый переключатель аккаунтов для Steam, Epic Games, Battle.net, Riot, Ubisoft, Discord и других клиентов. "
                              "Нативный интерфейс без браузерного движка, один файл, ничего не устанавливает.",
                              "A fast account switcher for Steam, Epic Games, Battle.net, Riot, Ubisoft, Discord and more. "
                              "Native UI with no browser engine, single file, installs nothing."));
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::TextUnformatted(TR("Пароли не хранятся: свитчер подменяет файлы и ключи реестра, в которых клиент помнит последний вход.",
                              "No passwords are stored: the switcher swaps the files and registry values where a client remembers the last sign-in."));
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::TextUnformatted(TR("Горячие клавиши: Enter — войти, двойной клик — войти, F2 — переименовать, Del — забыть, Ctrl+F — поиск, F5 — обновить, "
                              "Ctrl+1…9 — быстрый вход, Ctrl+, — настройки.",
                              "Shortcuts: Enter / double-click — sign in, F2 — rename, Del — forget, Ctrl+F — search, F5 — refresh, "
                              "Ctrl+1…9 — quick sign-in, Ctrl+, — settings."));
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::TextUnformatted(TR("Командная строка: --switch steam:<SteamID64|логин>[:online|invisible|offline|away|offlinemode], "
                              "--switch <платформа>:<id>, --new <платформа>, --run-app <appid>, --tray.",
                              "Command line: --switch steam:<SteamID64|login>[:online|invisible|offline|away|offlinemode], "
                              "--switch <platform>:<id>, --new <platform>, --run-app <appid>, --tray."));
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::TextUnformatted(TR("Лицензия GPL-3.0. Механика переключения и описания платформ основаны на открытом TcNo Account Switcher (GPL-3.0). "
                              "Шрифты Inter (OFL) и Lucide (ISC), интерфейс — Dear ImGui (MIT).",
                              "GPL-3.0 licence. Switching mechanics and platform definitions are based on the open-source TcNo Account Switcher (GPL-3.0). "
                              "Fonts: Inter (OFL), Lucide (ISC); UI: Dear ImGui (MIT)."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(14)));
    if (pill_button(TR("Открыть журнал", "Open log"), false, ImVec2(S(170), S(36)))) os::shell_open(fsx::join(store_.dir, "log.txt"));
}

}  // namespace vs
