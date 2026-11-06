#include "widgets.h"

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui_internal.h"
#include "theme.h"

namespace vs::ui {

static std::unordered_map<ImGuiID, float> g_anim;

float anim(ImGuiID id, bool target, float speed) {
    float& v = g_anim[id];
    float t = target ? 1.0f : 0.0f;
    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.1f) dt = 0.1f;
    v += (t - v) * (1.0f - std::exp(-speed * dt));
    if (std::fabs(v - t) < 0.01f) v = t;
    return v;
}

void tooltip(const char* text) {
    if (!text || !*text) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(6)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(8));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, P.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, P.accent_text);
        ImGui::PushStyleColor(ImGuiCol_Border, P.accent);
        ImGui::PushFont(F_REG, kBase * 0.9f);
        ImGui::SetTooltip("%s", text);
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
    }
}

bool icon_button(const char* id, const char* icon, const char* tip, bool active, float size) {
    float s = S(size);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(s, s));
    bool hov = ImGui::IsItemHovered();
    float h = anim(ImGui::GetItemID(), hov || active, 18.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (h > 0.0f) dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), with_alpha(active ? P.accent_soft : P.surface_active, active ? 1.0f : h), S(9));
    ImGui::PushFont(F_REG, kBase * 1.18f);
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(p.x + (s - ts.x) * 0.5f, p.y + (s - ts.y) * 0.5f), active || hov ? P.text : P.text2, icon);
    ImGui::PopFont();
    tooltip(tip);
    return pressed;
}

bool pill_button(const char* label, bool primary, ImVec2 size, bool enabled, const char* icon) {
    ImGui::PushFont(F_MED, kBase);
    std::string text = icon ? std::string(icon) + "  " + label : std::string(label);
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    ImVec2 sz(size.x > 0 ? size.x : ts.x + S(32), size.y > 0 ? size.y : S(38));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool pressed = ImGui::InvisibleButton("##pill", sz, enabled ? 0 : ImGuiButtonFlags_None) && enabled;
    bool hov = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    float h = anim(ImGui::GetItemID(), hov, 16.0f);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = sz.y * 0.5f;
    ImU32 bg, fg, br;
    if (primary) {
        bg = mix(P.accent, P.accent_hover, h);
        fg = P.accent_text;
        br = bg;
    } else {
        bg = mix(P.surface, P.surface_active, h);
        fg = P.text;
        br = P.border;
    }
    if (!enabled) { bg = with_alpha(bg, 0.45f); fg = with_alpha(fg, 0.5f); br = with_alpha(br, 0.5f); }
    if (held) bg = mix(bg, P.text, 0.08f);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, r);
    if (!primary) dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), br, r, S(1));
    dl->AddText(ImVec2(p.x + (sz.x - ts.x) * 0.5f, p.y + (sz.y - ts.y) * 0.5f), fg, text.c_str());
    ImGui::PopFont();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

bool search_box(const char* id, char* buf, size_t n, const char* hint, float width) {
    float h = S(36);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiID fid = ImGui::GetID(id);
    bool focused = ImGui::GetActiveID() == fid || ImGui::GetFocusID() == fid;
    float f = anim(fid + 1, focused, 18.0f);
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), P.surface, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), mix(P.border, P.border_strong, f), h * 0.5f, S(1));
    ImGui::PushFont(F_REG, kBase * 1.05f);
    dl->AddText(ImVec2(p.x + S(13), p.y + (h - ImGui::GetFontSize()) * 0.5f - S(1)), P.text3, "\xee\x85\x91");  // search icon
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(36), p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(width - S(36) - S(30));
    bool changed = ImGui::InputTextWithHint(id, hint, buf, n);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    if (buf[0]) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + width - S(30), p.y + (h - S(24)) * 0.5f));
        ImGui::PushID(id);
        if (ImGui::InvisibleButton("##clr", ImVec2(S(24), S(24)))) { buf[0] = 0; changed = true; }
        bool hv = ImGui::IsItemHovered();
        ImGui::PopID();
        ImGui::PushFont(F_REG, kBase * 0.95f);
        ImVec2 q = ImGui::GetItemRectMin();
        dl->AddText(ImVec2(q.x + S(5), q.y + S(4)), hv ? P.text : P.text3, "\xee\x86\xb2");  // x
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(width, 0));
    return changed;
}

bool toggle(const char* id, bool* v) {
    float h = S(22), w = S(38);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (pressed) *v = !*v;
    float t = anim(ImGui::GetItemID(), *v, 16.0f);
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 off = g_dark ? P.border_strong : mix(P.border_strong, P.border, 0.3f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), mix(off, P.accent, t), h * 0.5f);
    float r = h * 0.5f - S(3);
    ImVec2 c(p.x + h * 0.5f + (w - h) * t, p.y + h * 0.5f);
    dl->AddCircleFilled(c, r + (hov ? S(0.5f) : 0), g_dark ? mix(P.text, P.accent_text, t) : P.surface, 24);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

bool segmented(const char* id, int* cur, const char* const* labels, int count, float width) {
    ImGui::PushID(id);
    ImGui::PushFont(F_MED, kBase * 0.95f);
    float h = S(34);
    float total = 0;
    std::vector<float> ws(count);
    for (int i = 0; i < count; ++i) { ws[i] = ImGui::CalcTextSize(labels[i]).x + S(28); total += ws[i]; }
    if (width > total) {
        float extra = (width - total) / count;
        for (auto& w : ws) w += extra;
        total = width;
    }
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), g_dark ? P.surface : P.accent_soft, h * 0.5f);
    bool changed = false;
    float x = p.x;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(ws[i], h))) { if (*cur != i) changed = true; *cur = i; }
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (*cur == i) {
            dl->AddRectFilled(ImVec2(x + S(3), p.y + S(3)), ImVec2(x + ws[i] - S(3), p.y + h - S(3)), g_dark ? P.surface_active : P.surface, (h - S(6)) * 0.5f);
        }
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(x + (ws[i] - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), *cur == i ? P.text : (hov ? P.text : P.text2), labels[i]);
        x += ws[i];
    }
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(total, 0));
    ImGui::PopID();
    return changed;
}

void spinner(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float th) {
    float t = (float)ImGui::GetTime();
    float a0 = t * 6.0f;
    float len = 1.6f + 0.9f * std::sin(t * 2.2f);
    dl->PathClear();
    int n = 28;
    for (int i = 0; i <= n; ++i) {
        float a = a0 + len * i / n;
        dl->PathLineTo(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r));
    }
    dl->PathStroke(col, th, 0);
}

std::string ellipsize(ImFont* font, float size, const std::string& s, float max_w) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x <= max_w) return s;
    std::string r = s;
    const char* dots = "…";
    float dw = font->CalcTextSizeA(size, FLT_MAX, 0, dots).x;
    while (!r.empty()) {
        // drop one UTF-8 codepoint
        size_t i = r.size() - 1;
        while (i > 0 && ((unsigned char)r[i] & 0xC0) == 0x80) --i;
        r.erase(i);
        if (font->CalcTextSizeA(size, FLT_MAX, 0, r.c_str()).x + dw <= max_w) break;
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r + dots;
}

void text_clipped(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float max_w, ImU32 col, const std::string& s, bool center) {
    float sz = size * g_scale;
    std::string t = ellipsize(font, sz, s, max_w);
    if (center) {
        float w = font->CalcTextSizeA(sz, FLT_MAX, 0, t.c_str()).x;
        pos.x += (max_w - w) * 0.5f;
    }
    dl->AddText(font, sz, pos, col, t.c_str());
}

void soft_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 col, float spread) {
    int steps = 6;
    float alpha = ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    for (int i = steps; i >= 1; --i) {
        float t = (float)i / steps;
        float g = spread * t;
        ImU32 c = with_alpha((col | IM_COL32_A_MASK), alpha * (1.0f - t) * 0.9f / steps * 3.0f);
        dl->AddRectFilled(ImVec2(a.x - g * 0.6f, a.y - g * 0.2f + spread * 0.25f), ImVec2(b.x + g * 0.6f, b.y + g + spread * 0.25f), c, rounding + g);
    }
}

ImVec2 badge_size(const char* text, float size) {
    ImVec2 ts = F_SEMI->CalcTextSizeA(size * g_scale, FLT_MAX, 0, text);
    return ImVec2(ts.x + S(12), ts.y + S(4));
}

void badge(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 fg, ImU32 bg, float size) {
    ImVec2 bs = badge_size(text, size);
    dl->AddRectFilled(pos, ImVec2(pos.x + bs.x, pos.y + bs.y), bg, bs.y * 0.5f);
    dl->AddText(F_SEMI, size * g_scale, ImVec2(pos.x + S(6), pos.y + S(2)), fg, text);
}

bool menu_item(const char* icon, const char* label, const char* hint, bool enabled, bool danger) {
    std::string l = std::string(icon ? icon : "    ") + "   " + label;
    if (danger) ImGui::PushStyleColor(ImGuiCol_Text, P.danger);
    bool r = ImGui::MenuItem(l.c_str(), hint, false, enabled);
    if (danger) ImGui::PopStyleColor();
    return r;
}

bool begin_menu(const char* icon, const char* label, bool enabled) {
    std::string l = std::string(icon ? icon : "    ") + "   " + label;
    return ImGui::BeginMenu(l.c_str(), enabled);
}

void section_label(const char* text) {
    ImGui::PushFont(F_SEMI, kBase * 0.78f);
    ImGui::PushStyleColor(ImGuiCol_Text, P.text3);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static float g_row_y = 0, g_row_x = 0, g_row_w = 0, g_ctrl_w = 0;

void setting_row_begin(const char* title, const char* desc, float control_w) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    g_row_x = p.x;
    g_row_y = p.y;
    g_row_w = ImGui::GetContentRegionAvail().x;
    g_ctrl_w = control_w;
    float text_w = g_row_w - control_w - S(24);
    ImGui::PushFont(F_MED, kBase);
    ImGui::PushTextWrapPos(p.x + text_w);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (desc && *desc) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
        ImGui::PushFont(F_REG, kBase * 0.92f);
        ImGui::TextUnformatted(desc);
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    float text_bottom = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(p.x + g_row_w - control_w, p.y + S(2)));
    g_row_y = text_bottom;
}

void setting_row_end() {
    float y = std::max(g_row_y, ImGui::GetItemRectMax().y) + S(14);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(g_row_x, y - S(7)), ImVec2(g_row_x + g_row_w, y - S(7)), P.border, S(1));
    ImGui::SetCursorScreenPos(ImVec2(g_row_x, y));
    ImGui::Dummy(ImVec2(g_row_w, 0));
}

bool begin_dialog(const char* id, const char* title, float width) {
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

void dialog_text(const std::string& t) {
    ImGui::PushStyleColor(ImGuiCol_Text, P.text2);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// Two buttons right-aligned; returns 1 = primary, 2 = secondary
int dialog_buttons(const char* primary, const char* secondary, bool primary_enabled, bool danger) {
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

void dialog_input(const char* id, char* buf, size_t n, const char* hint, bool multiline, int flags) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), S(9)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, g_dark ? P.bg : P.bg);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (multiline) ImGui::InputTextMultiline(id, buf, n, ImVec2(-1, S(110)), flags);
    else ImGui::InputTextWithHint(id, hint, buf, n, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}


// circular countdown: full ring = whole period, the coloured arc shrinks as time runs out
void countdown_ring(ImDrawList* dl, ImVec2 c, float r, float remaining_frac, ImU32 col, ImU32 track, float th) {
    dl->AddCircle(c, r, track, 40, th);
    float f = remaining_frac < 0 ? 0 : remaining_frac > 1 ? 1 : remaining_frac;
    if (f <= 0.001f) return;
    const float pi = 3.14159265f;
    float a0 = -pi * 0.5f, a1 = a0 + 2 * pi * f;
    dl->PathArcTo(c, r, a0, a1, 40);
    dl->PathStroke(col, th);
}

}  // namespace vs::ui
