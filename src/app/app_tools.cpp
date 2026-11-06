// vsemvamdam-switcher — "Achievements" (Steam Achievement Manager) and "Farm" (hours / trading cards) pages
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "../core/fsx.h"
#include "../core/i18n.h"
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

static std::string hms(int64_t secs) {
    if (secs < 0) secs = 0;
    char b[32];
    if (secs >= 3600) snprintf(b, sizeof(b), "%lld:%02lld:%02lld", (long long)(secs / 3600), (long long)(secs / 60 % 60), (long long)(secs % 60));
    else snprintf(b, sizeof(b), "%lld:%02lld", (long long)(secs / 60), (long long)(secs % 60));
    return b;
}

static std::string fmt_num(double v) {
    char b[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) snprintf(b, sizeof(b), "%.0f", v);
    else snprintf(b, sizeof(b), "%g", v);
    return b;
}

static std::string hours_text(double h) {
    char b[32];
    if (h < 10) snprintf(b, sizeof(b), "%.1f", h);
    else snprintf(b, sizeof(b), "%.0f", h);
    return std::string(b) + TR(" ч", " h");
}

static std::string date_text(uint32_t t) {
    if (!t) return {};
    time_t tt = (time_t)t;
    struct tm tmv;
    localtime_s(&tmv, &tt);
    char b[32];
    strftime(b, sizeof(b), "%d.%m.%Y %H:%M", &tmv);
    return b;
}

// "" when the typed value can be stored, otherwise why not
static std::string stat_edit_error(const stats::Stat& st, const std::string& text, double* out = nullptr) {
    std::string t = replace_all(trim(text), ",", ".");
    double v = 0;
    size_t used = 0;
    try {
        v = std::stod(t, &used);
    } catch (...) {
        used = 0;
    }
    if (t.empty() || used != t.size() || !std::isfinite(v)) return TR("не число", "not a number");
    if (st.def.type == stats::StatInt && v != std::floor(v)) return TR("нужно целое число", "must be a whole number");
    if (st.def.type == stats::StatInt && (v < -2147483648.0 || v > 2147483647.0)) return TR("слишком большое число", "number too large");
    if (v < st.def.min) return std::string(TR("меньше минимума ", "below the minimum ")) + fmt_num(st.def.min);
    if (v > st.def.max) return std::string(TR("больше максимума ", "above the maximum ")) + fmt_num(st.def.max);
    if (st.def.incrementonly && v < st.value) return TR("можно только увеличить", "can only increase");
    if (out) *out = v;
    return {};
}

static void game_icon(ImDrawList* dl, const std::string& path, const std::string& name, uint32_t appid, ImVec2 p, float size) {
    draw_avatar(dl, path, name.empty() ? std::to_string(appid) : name, std::to_string(appid), p, size, 0.2f);
}

static void info_line(const std::string& t, ImU32 col) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// ================================================================ setup
void App::tools_init() {
    tools_ = std::make_unique<SteamTools>(store_, *guard_, [this] { return steam_ ? steam_->root() : std::string(); });
    tools_->on_event([this](const ToolEvent& e) { tools_event(e); });
}

void App::tools_event(const ToolEvent& e) {
    if (e.kind == ToolEventKind::Changed || e.text.empty()) return;
    bool foreground = !host_is_hidden() && GetForegroundWindow() == (HWND)main_hwnd();
    bool err = e.kind == ToolEventKind::FarmError;
    if (foreground) toast(e.text, err);
    else host_notify(TR("Фарм Steam", "Steam farm"), e.text);
}

uint64_t App::current_steam_id() {
    for (auto& a : accounts_["steam"])
        if (a.current) {
            uint64_t v = 0;
            parse_u64(a.id, v);
            return v;
        }
    return 0;
}

std::string App::current_steam_login() {
    for (auto& a : accounts_["steam"])
        if (a.current) return a.login;
    return {};
}

void App::open_session_login(uint64_t steamid, const std::string& login) {
    // an account with an authenticator here signs in through it (the code is filled in automatically)
    std::string key = guard_->key_for_steamid(steamid);
    guard_->flow_reset();
    if (!key.empty()) {
        open_gdialog(GDlg::Login, key);
        return;
    }
    session_for_ = steamid;
    open_gdialog(GDlg::Session, "");
    snprintf(g_buf1_, sizeof(g_buf1_), "%s", login.c_str());
}

// ================================================================ achievements
void App::draw_achievements(float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    LibraryView lib = tools_->library();
    if (!lib.loaded && !lib.loading && lib.error.empty()) tools_->load_library(false);
    float px = x + S(28), py = S(26);
    dl->AddText(F_SEMI, S(27), ImVec2(px, py - S(2)), P.text, TR("Достижения", "Achievements"));
    std::string sub = TR("Открыть или закрыть достижения и править статистику — как в Steam Achievement Manager",
                         "Unlock or lock achievements and edit stats — like Steam Achievement Manager");
    if (lib.loaded) {
        int n = 0;
        for (auto& g : lib.games) n += !ach_only_stats_ || g.has_stats;
        sub = std::string(TR("Игр: ", "Games: ")) + std::to_string(n) + " · " + TR("аккаунт в Steam: ", "Steam account: ") +
              (store_.settings.streamer_mode ? std::string("•••") : current_steam_login());
    }
    dl->AddText(F_REG, S(kBase * 0.95f), ImVec2(px, py + S(34)), P.text2, sub.c_str());
    float right = x + w - S(28);
    float bx = right - S(34);
    ImGui::SetCursorScreenPos(ImVec2(bx, py + S(6)));
    if (icon_button("##libref", ICON_REFRESH_CW, TR("Обновить список игр", "Refresh the game list"))) tools_->load_library(true);
    float sw = std::min(S(220), w * 0.26f);
    bx -= sw + S(10);
    ImGui::SetCursorScreenPos(ImVec2(bx, py + S(5)));
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
    search_box("##achsearch", ach_search_, sizeof(ach_search_), TR("Игра или AppID", "Game or AppID"), sw);

    float top = y + S(104);
    float bh = h - S(104);
    if (lib.loading && lib.games.empty()) {
        ImVec2 c(x + w * 0.5f, top + bh * 0.3f);
        spinner(dl, c, S(14), P.text2, S(2.5f));
        const char* t = TR("Собираю библиотеку игр…", "Collecting the game library…");
        ImVec2 ts = F_REG->CalcTextSizeA(S(kBase), FLT_MAX, 0, t);
        dl->AddText(F_REG, S(kBase), ImVec2(c.x - ts.x * 0.5f, c.y + S(26)), P.text2, t);
        return;
    }
    if (!lib.error.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(px, top + S(10)));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(px + std::min(w - S(56), S(560)) - x + x);
        info_line(lib.error, P.danger);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(8)));
        if (pill_button(TR("Повторить", "Retry"), true, ImVec2(S(140), S(36)))) tools_->load_library(true);
        ImGui::EndGroup();
        return;
    }
    // a narrow window shows one pane at a time: the list, or the picked game with a way back
    ach_narrow_ = w < S(700);
    float lw = ach_narrow_ ? (ach_app_ ? 0.0f : w) : std::clamp(w * 0.36f, S(250), S(340));
    if (lw > 0) {
    // ---- game list
    ImGui::SetCursorScreenPos(ImVec2(x, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(4)));
    ImGui::BeginChild("##achgames", ImVec2(lw, bh), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Checkbox(TR("Только с достижениями", "Only with achievements"), &ach_only_stats_);
        if (!lib.from_web) {
            ImGui::PushStyleColor(ImGuiCol_Text, P.text3);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(TR("Список собран с этого ПК. Полный — после входа в аккаунт в разделе «Фарм».",
                                      "Built from this PC. The full list appears after signing in on the Farm page."));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        uint64_t typed = 0;
        bool typed_id = parse_u64(trim(ach_search_), typed) && typed > 0 && typed < 0xFFFFFFFFu;
        int shown = 0;
        for (auto& g : lib.games) {
            if (ach_only_stats_ && !g.has_stats && g.appid != ach_app_) continue;
            if (ach_search_[0] && !icontains(g.name + " " + std::to_string(g.appid), ach_search_)) continue;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float rh = S(52);
            ImGui::PushID((int)g.appid);
            bool clicked = ImGui::InvisibleButton("##g", ImVec2(avail, rh));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            bool sel = g.appid == ach_app_;
            ImVec2 p1(p0.x + avail, p0.y + rh - S(4));
            if (hov || sel) ldl->AddRectFilled(p0, p1, with_alpha(P.surface, sel ? 1.0f : 0.7f), S(10));
            if (sel) ldl->AddRect(p0, p1, P.selection, S(10), S(1.3f));
            game_icon(ldl, tools_->icon_for(g.appid), g.name, g.appid, ImVec2(p0.x + S(8), p0.y + S(8)), S(32));
            text_clipped(ldl, F_MED, kBase * 0.95f, ImVec2(p0.x + S(50), p0.y + S(8)), avail - S(58), P.text,
                         g.name.empty() ? "AppID " + std::to_string(g.appid) : g.name);
            std::string meta = "AppID " + std::to_string(g.appid);
            if (g.minutes > 0) meta += " · " + hours_text(g.minutes / 60.0);
            text_clipped(ldl, F_REG, kBase * 0.8f, ImVec2(p0.x + S(50), p0.y + S(27)), avail - S(58), P.text3, meta);
            if (clicked && g.appid != ach_app_) {
                ach_app_ = g.appid;
                ach_changes_.clear();
                stat_edits_.clear();
                tools_->load_stats(g.appid);
            }
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rh));
            if (++shown > 1500) break;
        }
        if (typed_id && std::none_of(lib.games.begin(), lib.games.end(), [&](const ToolGame& g) { return g.appid == typed; })) {
            ImGui::Dummy(ImVec2(0, S(6)));
            std::string l = std::string(TR("Открыть AppID ", "Open AppID ")) + std::to_string(typed);
            if (pill_button(l.c_str(), false, ImVec2(avail, S(36)))) {
                ach_app_ = (uint32_t)typed;
                ach_changes_.clear();
                stat_edits_.clear();
                tools_->load_stats(ach_app_);
            }
        } else if (shown == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
            ImGui::TextUnformatted(TR("Ничего не найдено", "Nothing found"));
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, S(8)));
    }
    ImGui::EndChild();
    if (lw < w) dl->AddLine(ImVec2(x + lw, top + S(6)), ImVec2(x + lw, y + h - S(18)), P.border, S(1));
    }
    if (lw >= w) return;
    // ---- details
    ImGui::SetCursorScreenPos(ImVec2(x + lw, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(4)));
    ImGui::BeginChild("##achdetail", ImVec2(w - lw, bh), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    draw_ach_detail(ImGui::GetContentRegionAvail().x, bh);
    ImGui::EndChild();
}

void App::draw_ach_detail(float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!ach_app_) {
        ImGui::Dummy(ImVec2(0, S(30)));
        info_line(TR("Выберите игру слева. Steam должен быть запущен, а вход — в тот аккаунт, чьи достижения меняете.",
                     "Pick a game on the left. Steam must be running and signed in to the account whose achievements you change."),
                  P.text2);
        return;
    }
    StatsView sv = tools_->stats_view();
    const auto& g = sv.data;
    std::string name = !g.name.empty() ? g.name : tools_->game_name(ach_app_);
    if (name.empty()) name = "AppID " + std::to_string(ach_app_);
    ImGui::Dummy(ImVec2(0, S(4)));
    if (ach_narrow_) {
        ImVec2 b = ImGui::GetCursorScreenPos();
        if (icon_button("##achback", ICON_ARROW_LEFT, TR("К списку игр", "Back to the games"))) {
            ach_app_ = 0;
            ach_changes_.clear();
            stat_edits_.clear();
            return;
        }
        ImGui::SetCursorScreenPos(ImVec2(b.x + S(44), b.y));
        w -= S(44);
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    game_icon(dl, tools_->icon_for(ach_app_), name, ach_app_, p0, S(44));
    text_clipped(dl, F_SEMI, kBase * 1.15f, ImVec2(p0.x + S(56), p0.y + S(2)), w - S(110), P.text, name);
    int got = 0;
    for (auto& a : g.achievements) got += a.achieved;
    std::string meta = "AppID " + std::to_string(ach_app_);
    if (!g.achievements.empty()) meta += std::string(" · ") + TR("получено ", "unlocked ") + std::to_string(got) + TR(" из ", " of ") + std::to_string(g.achievements.size());
    text_clipped(dl, F_REG, kBase * 0.85f, ImVec2(p0.x + S(56), p0.y + S(24)), w - S(110), P.text3, meta);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - S(34), p0.y + S(4)));
    if (sv.loading) spinner(dl, ImVec2(p0.x + w - S(17), p0.y + S(21)), S(9), P.text2, S(2));
    else if (icon_button("##achref", ICON_REFRESH_CW, TR("Перечитать из Steam", "Reload from Steam"))) {
        ach_changes_.clear();
        stat_edits_.clear();
        tools_->load_stats(ach_app_);
    }
    if (ach_narrow_) {
        p0.x -= S(44);
        w += S(44);
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + S(60)));

    if (!sv.error.empty() && g.achievements.empty() && g.stats.empty()) {
        info_line(sv.error, P.danger);
        ImGui::Dummy(ImVec2(0, S(6)));
        if (pill_button(TR("Повторить", "Retry"), true, ImVec2(S(140), S(36)))) tools_->load_stats(ach_app_);
        return;
    }
    if (sv.loading && g.achievements.empty() && g.stats.empty()) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        spinner(dl, ImVec2(p.x + S(14), p.y + S(14)), S(9), P.text2, S(2.2f));
        dl->AddText(F_REG, S(kBase), ImVec2(p.x + S(34), p.y + S(5)), P.text2, TR("Спрашиваю Steam…", "Asking Steam…"));
        ImGui::Dummy(ImVec2(0, S(40)));
        return;
    }
    const char* tabs[] = {TR("Достижения", "Achievements"), TR("Статистика", "Stats")};
    segmented("##achtab", &ach_tab_, tabs, 2);
    ImGui::Dummy(ImVec2(0, S(8)));
    int pending = (int)ach_changes_.size() + (int)stat_edits_.size();
    int invalid = 0;
    for (auto& [id, txt] : stat_edits_)
        for (auto& st : g.stats)
            if (st.def.id == id && !stat_edit_error(st, txt).empty()) ++invalid;

    // bulk actions
    if (ach_tab_ == 0) {
        if (pill_button(TR("Отметить все", "Unlock all"), false, ImVec2(0, S(36)), !g.achievements.empty() && !sv.applying)) {
            for (auto& a : g.achievements)
                if (!stats::is_protected(a.def.permission)) {
                    if (!a.achieved) ach_changes_[a.def.id] = true;
                    else ach_changes_.erase(a.def.id);
                }
        }
        ImGui::SameLine(0, S(8));
        if (pill_button(TR("Снять все", "Lock all"), false, ImVec2(0, S(36)), !g.achievements.empty() && !sv.applying)) {
            for (auto& a : g.achievements)
                if (!stats::is_protected(a.def.permission)) {
                    if (a.achieved) ach_changes_[a.def.id] = false;
                    else ach_changes_.erase(a.def.id);
                }
        }
    } else {
        if (pill_button(TR("Сбросить всё…", "Reset everything…"), false, ImVec2(0, S(36)), (!g.stats.empty() || !g.achievements.empty()) && !sv.applying))
            open_gdialog(GDlg::ResetStats, "");
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    // save bar: always in place, so the list never jumps under the cursor
    {
        ImVec2 b0 = ImGui::GetCursorScreenPos();
        float bh2 = S(52);
        bool active = pending > 0 || sv.applying;
        dl->AddRectFilled(b0, ImVec2(b0.x + w, b0.y + bh2), active ? P.accent_soft : with_alpha(P.surface, 0.6f), S(14));
        if (active) dl->AddRect(b0, ImVec2(b0.x + w, b0.y + bh2), P.selection, S(14), S(1));
        const char* sl = TR("Сохранить", "Save");
        const char* dlb = TR("Отменить", "Discard");
        ImGui::PushFont(F_MED, kBase);
        float sw2 = ImGui::CalcTextSize((std::string(ICON_SAVE) + "  " + sl).c_str()).x + S(32);
        float dw2 = ImGui::CalcTextSize(dlb).x + S(32);
        ImGui::PopFont();
        float text_w = w - sw2 - (pending ? dw2 + S(8) : 0) - S(40);
        float ty = b0.y + (bh2 - S(kBase)) * 0.5f;
        if (sv.applying) {
            spinner(dl, ImVec2(b0.x + S(24), b0.y + bh2 * 0.5f), S(8), P.text, S(2));
            text_clipped(dl, F_MED, kBase, ImVec2(b0.x + S(42), ty), text_w, P.text, TR("Сохраняю в Steam…", "Saving to Steam…"));
        } else if (invalid) {
            text_clipped(dl, F_MED, kBase * 0.92f, ImVec2(b0.x + S(16), ty), text_w, P.danger, TR("Есть ошибки в значениях", "Some values are invalid"));
        } else if (pending) {
            text_clipped(dl, F_MED, kBase, ImVec2(b0.x + S(16), ty), text_w, P.text, std::string(TR("Изменений: ", "Changes: ")) + std::to_string(pending));
        } else if (!sv.result.empty()) {
            text_clipped(dl, F_MED, kBase * 0.92f, ImVec2(b0.x + S(16), ty), text_w, sv.result_error ? P.danger : P.ok, sv.result);
            if (ImGui::IsMouseHoveringRect(b0, ImVec2(b0.x + text_w, b0.y + bh2))) ImGui::SetTooltip("%s", sv.result.c_str());
        } else {
            text_clipped(dl, F_REG, kBase * 0.9f, ImVec2(b0.x + S(16), ty), text_w, P.text3,
                         ach_tab_ == 0 ? TR("Отметьте достижения и сохраните", "Tick achievements and save") : TR("Измените значения и сохраните", "Edit the values and save"));
        }
        ImGui::SetCursorScreenPos(ImVec2(b0.x + w - sw2 - S(8), b0.y + S(8)));
        if (pill_button(sl, true, ImVec2(sw2, S(36)), pending > 0 && !invalid && !sv.applying, ICON_SAVE)) {
            stats::Changes ch;
            ch.achievements = ach_changes_;
            for (auto& [id, txt] : stat_edits_)
                for (auto& st : g.stats) {
                    double v = 0;
                    if (st.def.id == id && stat_edit_error(st, txt, &v).empty()) ch.stats[id] = v;
                }
            tools_->apply(ach_app_, ch);
            ach_changes_.clear();
            stat_edits_.clear();
        }
        if (pending && !sv.applying) {
            ImGui::SetCursorScreenPos(ImVec2(b0.x + w - sw2 - dw2 - S(16), b0.y + S(8)));
            if (pill_button(dlb, false, ImVec2(dw2, S(36)))) {
                ach_changes_.clear();
                stat_edits_.clear();
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(b0.x, b0.y + bh2 + S(10)));
        ImGui::Dummy(ImVec2(0, 0));
    }

    if (ach_tab_ == 0) {
        const char* filters[] = {TR("Все", "All"), TR("Не получены", "Locked"), TR("Получены", "Unlocked")};
        segmented("##achfilter", &ach_filter_, filters, 3);
        ImGui::Dummy(ImVec2(0, S(6)));
        if (g.achievements.empty()) {
            info_line(TR("У этой игры нет достижений.", "This game has no achievements."), P.text2);
            return;
        }
        for (auto& a : g.achievements) {
            auto ch = ach_changes_.find(a.def.id);
            bool state = ch != ach_changes_.end() ? ch->second : a.achieved;
            if (ach_filter_ == 1 && state) continue;
            if (ach_filter_ == 2 && !state) continue;
            bool prot = stats::is_protected(a.def.permission);
            ImVec2 q0 = ImGui::GetCursorScreenPos();
            float rh = S(62);
            ImGui::PushID(a.def.id.c_str());
            bool clicked = ImGui::InvisibleButton("##a", ImVec2(w, rh));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            ImVec2 q1(q0.x + w, q0.y + rh - S(6));
            bool changed = ch != ach_changes_.end();
            dl->AddRectFilled(q0, q1, changed ? with_alpha(P.accent_soft, 0.9f) : with_alpha(P.surface, hov ? 1.0f : 0.6f), S(12));
            if (changed) dl->AddRect(q0, q1, P.selection, S(12), S(1.2f));
            float is = S(40);
            ImVec2 ip(q0.x + S(10), q0.y + (rh - S(6) - is) * 0.5f);
            std::string icon = tools_->achievement_icon(ach_app_, state ? a.def.icon : a.def.icon_gray);
            ImTextureID t = icon.empty() ? ImTextureID() : tex_file(icon, 64, 0.16f);
            if (t) dl->AddImage(ImTextureRef(t), ip, ImVec2(ip.x + is, ip.y + is), ImVec2(0, 0), ImVec2(1, 1), state ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 150));
            else dl->AddRectFilled(ip, ImVec2(ip.x + is, ip.y + is), state ? P.accent_soft : P.surface_active, S(8));
            float tx = ip.x + is + S(12);
            float tmax = w - (tx - q0.x) - S(60);
            text_clipped(dl, F_MED, kBase * 0.95f, ImVec2(tx, ip.y), tmax, state ? P.text : P.text2, a.def.name);
            std::string desc = a.def.desc;
            if (desc.empty() && a.def.hidden) desc = TR("Скрытое достижение", "Hidden achievement");
            if (a.achieved && a.unlock_time && !changed) desc += (desc.empty() ? "" : "  ·  ") + date_text(a.unlock_time);
            text_clipped(dl, F_REG, kBase * 0.82f, ImVec2(tx, ip.y + S(20)), tmax, P.text3, desc);
            // state control on the right
            ImVec2 cc(q1.x - S(30), q0.y + (rh - S(6)) * 0.5f);
            if (prot) {
                ImVec2 ts = F_REG->CalcTextSizeA(S(15), FLT_MAX, 0, ICON_LOCK);
                dl->AddText(F_REG, S(15), ImVec2(cc.x - ts.x * 0.5f, cc.y - ts.y * 0.5f), P.text3, ICON_LOCK);
                if (hov) ImGui::SetTooltip("%s", TR("Защищено игрой: меняется только на её серверах", "Protected by the game: only its servers can change it"));
            } else {
                float r = S(11);
                if (state) {
                    dl->AddCircleFilled(cc, r, P.ok, 24);
                    ImVec2 ts = F_REG->CalcTextSizeA(S(13), FLT_MAX, 0, ICON_CHECK);
                    dl->AddText(F_REG, S(13), ImVec2(cc.x - ts.x * 0.5f, cc.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), ICON_CHECK);
                } else {
                    dl->AddCircle(cc, r, P.border_strong, 24, S(1.6f));
                }
                if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (clicked) {
                    bool next = !state;
                    if (next == a.achieved) ach_changes_.erase(a.def.id);
                    else ach_changes_[a.def.id] = next;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(q0.x, q0.y + rh));
        }
        ImGui::Dummy(ImVec2(0, S(10)));
        return;
    }
    // ---- stats
    if (g.stats.empty()) {
        info_line(TR("У этой игры нет статистики.", "This game has no stats."), P.text2);
        return;
    }
    info_line(TR("Изменения сразу уходят в Steam. В сетевых играх с рейтингом или античитом статистику лучше не трогать.",
                 "Changes go straight to Steam. Better leave stats alone in online games with ranking or anti-cheat."),
              P.text3);
    ImGui::Dummy(ImVec2(0, S(6)));
    for (auto& st : g.stats) {
        bool prot = stats::is_protected(st.def.permission);
        ImVec2 q0 = ImGui::GetCursorScreenPos();
        float rh = S(54);
        auto ed = stat_edits_.find(st.def.id);
        bool changed = ed != stat_edits_.end();
        dl->AddRectFilled(q0, ImVec2(q0.x + w, q0.y + rh - S(6)), changed ? with_alpha(P.accent_soft, 0.9f) : with_alpha(P.surface, 0.6f), S(12));
        float vw = S(150);
        text_clipped(dl, F_MED, kBase * 0.92f, ImVec2(q0.x + S(12), q0.y + S(7)), w - vw - S(40), P.text, st.def.name.empty() ? st.def.id : st.def.name);
        std::string meta = st.def.id;
        if (st.def.incrementonly) meta += std::string(" · ") + TR("только растёт", "increment only");
        if (st.def.max > 0 && st.def.max < 2e9) meta += std::string(" · ") + TR("макс. ", "max ") + fmt_num(st.def.max);
        if (prot) meta += std::string(" · ") + TR("защищено игрой", "protected by the game");
        std::string bad = changed ? stat_edit_error(st, ed->second) : std::string();
        if (!bad.empty()) meta = bad;
        text_clipped(dl, F_REG, kBase * 0.8f, ImVec2(q0.x + S(12), q0.y + S(26)), w - vw - S(40), bad.empty() ? P.text3 : P.danger, meta);
        char buf[64];
        if (changed) snprintf(buf, sizeof(buf), "%s", ed->second.c_str());
        else if (st.def.type == stats::StatInt) snprintf(buf, sizeof(buf), "%lld", (long long)st.value);
        else snprintf(buf, sizeof(buf), "%.7g", st.value);
        ImGui::SetCursorScreenPos(ImVec2(q0.x + w - vw - S(10), q0.y + S(9)));
        ImGui::SetNextItemWidth(vw);
        ImGui::PushID(st.def.id.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(7)));
        ImGui::BeginDisabled(prot);
        if (ImGui::InputText("##v", buf, sizeof(buf), st.def.type == stats::StatInt ? ImGuiInputTextFlags_CharsDecimal : ImGuiInputTextFlags_CharsScientific)) {
            std::string now = buf;
            char orig[64];
            if (st.def.type == stats::StatInt) snprintf(orig, sizeof(orig), "%lld", (long long)st.value);
            else snprintf(orig, sizeof(orig), "%.7g", st.value);
            if (now == orig) stat_edits_.erase(st.def.id);
            else stat_edits_[st.def.id] = now;
        }
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(q0.x, q0.y + rh));
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    (void)h;
}

// ================================================================ farm
void App::draw_farm(float x, float y, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    FarmView fv = tools_->farm_view();
    float px = x + S(28), py = S(26);
    dl->AddText(F_SEMI, S(27), ImVec2(px, py - S(2)), P.text, TR("Фарм", "Farm"));
    std::string acc = store_.settings.streamer_mode ? std::string("•••") : current_steam_login();
    std::string sub = std::string(TR("Часы и карточки · аккаунт в Steam: ", "Hours and cards · Steam account: ")) + (acc.empty() ? "—" : acc);
    if (fv.active) sub += std::string(" · ") + TR("идёт ", "running ") + hms(unix_now() - fv.started);
    dl->AddText(F_REG, S(kBase * 0.95f), ImVec2(px, py + S(34)), P.text2, sub.c_str());
    ImGui::SetCursorScreenPos(ImVec2(px, y + S(94)));
    const char* tabs[] = {TR("Карточки", "Trading cards"), TR("Часы", "Hours")};
    segmented("##farmtab", &farm_tab_, tabs, 2);
    float top = y + S(146);
    ImGui::SetCursorScreenPos(ImVec2(x, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(4)));
    ImGui::BeginChild("##farmbody", ImVec2(w, h - (top - y)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    float cw = std::min(ImGui::GetContentRegionAvail().x, S(820));
    if (farm_tab_ == 0) draw_farm_cards(cw);
    else draw_farm_hours(cw, h - (top - y));
    ImGui::EndChild();
}

static void farm_running_list(ImDrawList* dl, SteamTools* tools, const FarmView& fv, float w) {
    for (auto& r : fv.running) {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float rh = S(50);
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + rh - S(6)), P.surface, S(12));
        game_icon(dl, tools->icon_for(r.appid), r.name, r.appid, ImVec2(p0.x + S(10), p0.y + S(7)), S(30));
        text_clipped(dl, F_MED, kBase * 0.92f, ImVec2(p0.x + S(50), p0.y + S(6)), w - S(180), P.text, r.name);
        std::string meta = "AppID " + std::to_string(r.appid);
        for (auto& b : fv.badges)
            if (b.appid == r.appid) meta += " · " + hours_text(b.hours) + " · " + std::string(TR("карточек: ", "drops: ")) + std::to_string(b.drops);
        text_clipped(dl, F_REG, kBase * 0.8f, ImVec2(p0.x + S(50), p0.y + S(24)), w - S(180), P.text3, meta);
        std::string t = r.ok ? hms(unix_now() - r.started) : std::string(TR("не запустилась", "not running"));
        ImVec2 ts = F_SEMI->CalcTextSizeA(S(kBase), FLT_MAX, 0, t.c_str());
        dl->AddText(F_SEMI, S(kBase), ImVec2(p0.x + w - ts.x - S(14), p0.y + S(14)), r.ok ? P.ok : P.danger, t.c_str());
        ImGui::Dummy(ImVec2(w, rh));
    }
}

void App::draw_farm_cards(float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    FarmView fv = tools_->farm_view();
    bool cards_active = fv.active && fv.kind == FarmKind::Cards;
    uint64_t sid = current_steam_id();
    bool session = sid && guard_->has_web_session(sid);
    ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(TR("Как Idle Master: свитчер «запускает» игры без самих игр, и Steam выдаёт карточки. Игры, где наиграно меньше 2 часов, идут вместе "
                              "(карточки в них ещё не падают), затем — по одной, пока не выпадут все. Steam должен быть запущен.",
                              "Like Idle Master: the switcher “runs” games without the games themselves, and Steam drops the cards. Games with under 2 hours run "
                              "together (they don't drop yet), then one at a time until all drops are received. Steam must be running."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(10)));
    if (!sid) {
        info_line(TR("Сначала войдите в аккаунт Steam — фарм идёт на том аккаунте, который сейчас открыт в Steam.",
                     "Sign in to Steam first — the farm runs on the account that is open in Steam."),
                  P.warn);
        return;
    }
    if (!session && !cards_active) {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        const char* why = TR("Сколько карточек осталось, Steam показывает только владельцу. Пароль нигде не сохраняется.",
                             "Steam shows the remaining drops only to the owner. The password is not stored.");
        float tw = w - S(70);
        float th = F_REG->CalcTextSizeA(S(kBase * 0.9f), FLT_MAX, tw, why).y;
        float ph = S(38) + th + S(14) + S(36) + S(18);
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + ph), with_alpha(P.surface, 0.7f), S(14));
        dl->AddRect(p0, ImVec2(p0.x + w, p0.y + ph), P.border, S(14), S(1));
        dl->AddText(F_REG, S(kBase * 1.2f), ImVec2(p0.x + S(18), p0.y + S(16)), P.text2, ICON_LOG_IN);
        dl->AddText(F_SEMI, S(kBase), ImVec2(p0.x + S(50), p0.y + S(16)), P.text, TR("Нужен вход в аккаунт", "Sign-in needed"));
        dl->AddText(F_REG, S(kBase * 0.9f), ImVec2(p0.x + S(50), p0.y + S(38)), P.text2, why, nullptr, tw);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + S(50), p0.y + S(38) + th + S(14)));
        std::string lbl = std::string(TR("Войти как ", "Sign in as ")) + (store_.settings.streamer_mode ? std::string("•••") : current_steam_login());
        if (pill_button(lbl.c_str(), true, ImVec2(0, S(36)))) open_session_login(sid, current_steam_login());
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + ph + S(12)));
        ImGui::Dummy(ImVec2(0, 0));
        return;
    }
    if (!cards_active) {
        ImGui::PushFont(F_MED, kBase * 0.92f);
        ImGui::TextUnformatted(TR("Порядок", "Order"));
        ImGui::PopFont();
        const char* modes[] = {TR("Авто (как Idle Master)", "Auto (like Idle Master)"), TR("По одной игре", "One game at a time"),
                               TR("Все сразу", "All at once")};
        segmented("##farmmode", &farm_mode_, modes, 3);
        ImGui::Dummy(ImVec2(0, S(10)));
        bool other = fv.active && fv.kind == FarmKind::Hours;
        if (pill_button(TR("Начать фарм карточек", "Start farming cards"), true, ImVec2(S(240), S(40)), true, ICON_PLAY)) {
            tools_->start_cards(farm_mode_ == 1 ? cards::Mode::Sequential : farm_mode_ == 2 ? cards::Mode::Simultaneous : cards::Mode::Auto);
        }
        if (other) {
            ImGui::SameLine(0, S(10));
            ImGui::AlignTextToFramePadding();
            info_line(TR("остановит фарм часов", "stops the hours farm"), P.text3);
        }
        ImGui::Dummy(ImVec2(0, S(6)));
        if (!fv.status.empty() && fv.kind == FarmKind::Cards) info_line(fv.status, P.text2);
        if (!fv.error.empty() && fv.kind == FarmKind::Cards) info_line(fv.error, P.danger);
        if (fv.need_login && fv.kind == FarmKind::Cards) {
            if (pill_button(TR("Войти", "Sign in"), false, ImVec2(S(140), S(36)))) open_session_login(sid, current_steam_login());
        }
        if (session && guard_->key_for_steamid(sid).empty()) {  // a session from the Steam Guard page is managed there
            ImGui::Dummy(ImVec2(0, S(10)));
            if (pill_button(TR("Выйти из аккаунта", "Sign out"), false, ImVec2(0, S(32)))) {
                guard_->forget_web_session(sid);
                toast(TR("Сессия удалена", "Session removed"));
            }
            tooltip(TR("Удаляет сохранённую для фарма сессию этого аккаунта", "Removes the session saved for the farm"));
        }
        return;
    }
    // ---- running
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float ph = S(84);
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + ph), P.surface, S(16));
        dl->AddRect(p0, ImVec2(p0.x + w, p0.y + ph), P.border, S(16), S(1));
        auto big = [&](float x, const std::string& v, const char* label) {
            dl->AddText(F_SEMI, S(26), ImVec2(x, p0.y + S(14)), P.text, v.c_str());
            dl->AddText(F_REG, S(kBase * 0.82f), ImVec2(x, p0.y + S(50)), P.text3, label);
        };
        int games_left = 0;
        for (auto& b : fv.badges) games_left += b.drops > 0;
        bool checked = fv.next_check != 0;  // set after the first successful look at the badges
        big(p0.x + S(20), checked ? std::to_string(fv.drops_left) : std::string("—"), TR("карточек осталось", "drops left"));
        big(p0.x + S(20) + w * 0.25f, checked ? std::to_string(games_left) : std::string("—"), TR("игр с карточками", "games with drops"));
        big(p0.x + S(20) + w * 0.5f, std::to_string(fv.dropped), TR("получено сейчас", "received now"));
        std::string nc = fv.next_check ? hms(fv.next_check - unix_now()) : std::string("—");
        big(p0.x + S(20) + w * 0.75f, nc, TR("до проверки", "next check"));
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + ph + S(12)));
    }
    if (pill_button(TR("Остановить", "Stop"), true, ImVec2(S(160), S(38)), true, ICON_POWER)) tools_->stop_farm();
    ImGui::SameLine(0, S(8));
    if (pill_button(TR("Проверить сейчас", "Check now"), false, ImVec2(0, S(38)), true, ICON_REFRESH_CW)) tools_->check_now();
    ImGui::Dummy(ImVec2(0, S(6)));
    if (!fv.status.empty()) info_line(fv.status, P.text2);
    if (!fv.error.empty()) info_line(fv.error, P.danger);
    ImGui::Dummy(ImVec2(0, S(8)));
    if (!fv.running.empty()) {
        section_label(TR("СЕЙЧАС ИДУТ", "RUNNING NOW"));
        ImGui::Dummy(ImVec2(0, S(4)));
        farm_running_list(dl, tools_.get(), fv, w);
    }
    if (!fv.badges.empty()) {
        ImGui::Dummy(ImVec2(0, S(8)));
        section_label(TR("ОСТАЛОСЬ", "LEFT"));
        ImGui::Dummy(ImVec2(0, S(4)));
        for (auto& b : fv.badges) {
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            text_clipped(dl, F_REG, kBase * 0.92f, p0, w - S(180), P.text, b.name.empty() ? "AppID " + std::to_string(b.appid) : b.name);
            std::string r = hours_text(b.hours) + "   ·   " + std::to_string(b.drops);
            ImVec2 ts = F_MED->CalcTextSizeA(S(kBase * 0.92f), FLT_MAX, 0, r.c_str());
            dl->AddText(F_MED, S(kBase * 0.92f), ImVec2(p0.x + w - ts.x, p0.y), P.text2, r.c_str());
            ImGui::Dummy(ImVec2(w, S(24)));
        }
    }
    ImGui::Dummy(ImVec2(0, S(12)));
}

void App::draw_farm_hours(float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    FarmView fv = tools_->farm_view();
    bool hours_active = fv.active && fv.kind == FarmKind::Hours;
    if (hours_active) {
        std::string t = std::string(TR("Идут игры: ", "Running: ")) + std::to_string(fv.running.size());
        if (fv.stop_at) t += std::string(" · ") + TR("остановится через ", "stops in ") + hms(fv.stop_at - unix_now());
        ImGui::PushFont(F_SEMI, kBase * 1.05f);
        ImGui::TextUnformatted(t.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, S(6)));
        if (pill_button(TR("Остановить", "Stop"), true, ImVec2(S(160), S(38)), true, ICON_POWER)) tools_->stop_farm();
        ImGui::Dummy(ImVec2(0, S(8)));
        if (!fv.error.empty()) info_line(fv.error, P.danger);
        farm_running_list(dl, tools_.get(), fv, w);
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(TR("Отметьте игры — Steam будет считать, что вы в них играете (до 32 одновременно). Друзья увидят вас «в игре».",
                              "Tick the games — Steam counts them as played (up to 32 at once). Friends will see you as in-game."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(8)));
    LibraryView lib = tools_->library();
    if (!lib.loaded && !lib.loading && lib.error.empty()) tools_->load_library(false);
    // controls: [search] [time limit] [start] [x] — one row, or two in a narrow window
    {
        ImVec2 r0 = ImGui::GetCursorScreenPos();
        float rh = S(38);
        float cw = S(170), bw = S(140), xw = S(34);
        float row2 = cw + S(10) + bw + S(8) + xw;
        bool wide = w >= S(240) + S(10) + row2;
        float sw = wide ? std::min(S(280), w - row2 - S(10)) : std::min(w, S(360));
        ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + (rh - S(36)) * 0.5f));
        search_box("##farmsearch", farm_search_, sizeof(farm_search_), TR("Игра или AppID", "Game or AppID"), sw);
        float cx = wide ? r0.x + sw + S(10) : r0.x;
        float cy = wide ? r0.y : r0.y + rh + S(8);
        const char* it[] = {TR("Без ограничения", "No limit"), TR("1 час", "1 hour"), TR("2 часа", "2 hours"), TR("5 часов", "5 hours"),
                            TR("10 часов", "10 hours"), TR("24 часа", "24 hours")};
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), (S(36) - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(18));
        ImGui::SetCursorScreenPos(ImVec2(cx, cy + (rh - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(cw);
        if (ImGui::BeginCombo("##hlimit", it[std::clamp(hours_limit_, 0, 5)])) {
            for (int i = 0; i < 6; ++i)
                if (ImGui::Selectable(it[i], hours_limit_ == i)) hours_limit_ = i;
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("Остановить фарм через…", "Stop the farm after…"));
        ImGui::SetCursorScreenPos(ImVec2(cx + cw + S(10), cy));
        std::string st = std::string(TR("Начать", "Start")) + " (" + std::to_string(hours_sel_.size()) + ")";
        if (pill_button(st.c_str(), true, ImVec2(bw, rh), !hours_sel_.empty(), ICON_PLAY)) {
            const int mins[] = {0, 60, 120, 300, 600, 1440};
            tools_->start_hours(std::vector<uint32_t>(hours_sel_.begin(), hours_sel_.end()), mins[std::clamp(hours_limit_, 0, 5)]);
        }
        if (!hours_sel_.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(cx + cw + S(10) + bw + S(8), cy + (rh - S(34)) * 0.5f));
            if (icon_button("##hclear", ICON_X, TR("Снять выбор", "Clear the selection"))) hours_sel_.clear();
        }
        ImGui::SetCursorScreenPos(ImVec2(r0.x, cy + rh + S(8)));
        ImGui::Dummy(ImVec2(0, 0));
    }
    if (fv.kind == FarmKind::Hours && !fv.status.empty()) info_line(fv.status, P.text2);
    if (fv.kind == FarmKind::Hours && !fv.error.empty()) info_line(fv.error, P.danger);
    ImGui::Dummy(ImVec2(0, S(6)));
    if (lib.loading && lib.games.empty()) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        spinner(dl, ImVec2(p.x + S(14), p.y + S(14)), S(9), P.text2, S(2.2f));
        dl->AddText(F_REG, S(kBase), ImVec2(p.x + S(34), p.y + S(5)), P.text2, TR("Собираю библиотеку игр…", "Collecting the game library…"));
        ImGui::Dummy(ImVec2(0, S(40)));
        return;
    }
    if (!lib.error.empty()) {
        info_line(lib.error, P.danger);
        if (pill_button(TR("Повторить", "Retry"), false, ImVec2(S(140), S(36)))) tools_->load_library(true);
        return;
    }
    uint64_t typed = 0;
    if (parse_u64(trim(farm_search_), typed) && typed > 0 && typed < 0xFFFFFFFFu &&
        std::none_of(lib.games.begin(), lib.games.end(), [&](const ToolGame& g) { return g.appid == typed; })) {
        std::string l = std::string(TR("Добавить AppID ", "Add AppID ")) + std::to_string(typed);
        if (pill_button(l.c_str(), false, ImVec2(0, S(34)), hours_sel_.size() < 32)) hours_sel_.insert((uint32_t)typed);
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    int shown = 0;
    // the order never changes on click (rows must not jump under the cursor); the picked games are listed above
    if (!hours_sel_.empty()) {
        std::string names;
        for (uint32_t id : hours_sel_) {
            std::string n = tools_->game_name(id);
            names += (names.empty() ? "" : ", ") + (n.empty() ? "AppID " + std::to_string(id) : n);
        }
        ImVec2 p = ImGui::GetCursorScreenPos();
        text_clipped(dl, F_REG, kBase * 0.9f, p, w, P.text2, std::string(TR("Выбрано: ", "Selected: ")) + names);
        ImGui::Dummy(ImVec2(w, S(22)));
    }
    for (auto& g : lib.games) {
        if (farm_search_[0] && !icontains(g.name + " " + std::to_string(g.appid), farm_search_)) continue;
        if (++shown > 600) break;
        bool sel = hours_sel_.count(g.appid) > 0;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float rh = S(44);
        ImGui::PushID((int)g.appid);
        bool clicked = ImGui::InvisibleButton("##h", ImVec2(w, rh));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov || sel) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + rh - S(4)), sel ? with_alpha(P.accent_soft, 0.9f) : with_alpha(P.surface, 0.7f), S(10));
        ImVec2 cb(p0.x + S(12), p0.y + (rh - S(4) - S(18)) * 0.5f);
        dl->AddRectFilled(cb, ImVec2(cb.x + S(18), cb.y + S(18)), sel ? P.accent : P.bg, S(5));
        dl->AddRect(cb, ImVec2(cb.x + S(18), cb.y + S(18)), sel ? P.accent : P.border_strong, S(5), S(1.3f));
        if (sel) {
            ImVec2 ts = F_REG->CalcTextSizeA(S(12), FLT_MAX, 0, ICON_CHECK);
            dl->AddText(F_REG, S(12), ImVec2(cb.x + (S(18) - ts.x) * 0.5f, cb.y + (S(18) - ts.y) * 0.5f), P.accent_text, ICON_CHECK);
        }
        game_icon(dl, tools_->icon_for(g.appid), g.name, g.appid, ImVec2(p0.x + S(42), p0.y + S(6)), S(28));
        text_clipped(dl, F_MED, kBase * 0.92f, ImVec2(p0.x + S(80), p0.y + S(5)), w - S(200), P.text, g.name.empty() ? "AppID " + std::to_string(g.appid) : g.name);
        text_clipped(dl, F_REG, kBase * 0.78f, ImVec2(p0.x + S(80), p0.y + S(22)), w - S(200), P.text3, "AppID " + std::to_string(g.appid));
        if (g.minutes >= 0) {
            std::string ht = hours_text(g.minutes / 60.0);
            ImVec2 ts = F_REG->CalcTextSizeA(S(kBase * 0.85f), FLT_MAX, 0, ht.c_str());
            dl->AddText(F_REG, S(kBase * 0.85f), ImVec2(p0.x + w - ts.x - S(14), p0.y + S(12)), P.text3, ht.c_str());
        }
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked) {
            if (sel) hours_sel_.erase(g.appid);
            else if (hours_sel_.size() < 32) hours_sel_.insert(g.appid);
            else toast(TR("Не больше 32 игр одновременно — так считает Steam", "No more than 32 games at once — Steam's limit"), true);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rh));
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    (void)h;
}

// ================================================================ dialogs
void App::draw_tools_dialogs() {
    if (gdlg_ != GDlg::ResetStats) return;
    if (gdlg_open_req_) {
        ImGui::OpenPopup("##td_reset");
        gdlg_open_req_ = false;
    }
    bool close = false;
    if (begin_dialog("##td_reset", TR("Сбросить всю статистику?", "Reset all stats?"), 480)) {
        std::string name = tools_->game_name(ach_app_);
        dialog_text((name.empty() ? "AppID " + std::to_string(ach_app_) : name) + "\n\n" +
                    TR("Steam обнулит всю статистику этой игры на аккаунте. Отменить это нельзя.",
                       "Steam zeroes all of this game's stats on the account. This can't be undone."));
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::Checkbox(TR("Вместе с достижениями", "Achievements too"), &g_check_);
        int r = dialog_buttons(TR("Сбросить", "Reset"), TR("Отмена", "Cancel"), true, true);
        if (r == 1) {
            stats::Changes c;
            c.reset_all = true;
            c.reset_with_achievements = g_check_;
            tools_->apply(ach_app_, c);
            ach_changes_.clear();
            stat_edits_.clear();
            close = true;
        }
        if (r == 2) close = true;
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    } else {
        close = true;
    }
    if (close) gdlg_ = GDlg::None;
}

}  // namespace vs
