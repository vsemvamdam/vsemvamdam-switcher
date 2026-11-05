#include "theme.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "icons.h"

namespace vs::ui {

Palette P;
bool g_dark = false;
float g_scale = 1.0f;
ImFont* F_REG = nullptr;
ImFont* F_MED = nullptr;
ImFont* F_SEMI = nullptr;

static ImU32 hex(unsigned rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, (int)(a * 255.0f + 0.5f));
}

ImU32 with_alpha(ImU32 c, float a) {
    int al = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)al << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

static bool resource(int id, void*& data, int& size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    data = LockResource(g);
    size = (int)SizeofResource(nullptr, r);
    return data && size > 0;
}

static ImFont* add_font(int id) {
    ImGuiIO& io = ImGui::GetIO();
    void* d = nullptr;
    int sz = 0;
    ImFont* f = nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;
    if (resource(id, d, sz)) f = io.Fonts->AddFontFromMemoryTTF(d, sz, kBase, &cfg);
    if (!f) {
        // fallback: Segoe UI from the system
        if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
            f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", kBase);
        if (!f) f = io.Fonts->AddFontDefault();
    }
    // merge icons
    void* di = nullptr;
    int si = 0;
    if (resource(104, di, si)) {
        ImFontConfig ic;
        ic.FontDataOwnedByAtlas = false;
        ic.MergeMode = true;
        ic.GlyphOffset = ImVec2(0, 2.5f);
        ic.GlyphMinAdvanceX = kBase;
        io.Fonts->AddFontFromMemoryTTF(di, si, kBase, &ic);
    }
    // Symbols fallback for rare glyphs (arrows, emoji-like) from system font
    {
        ImFontConfig sc;
        sc.MergeMode = true;
        if (GetFileAttributesW(L"C:\\Windows\\Fonts\\seguisym.ttf") != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", kBase, &sc);
    }
    return f;
}

void load_fonts() {
    F_REG = add_font(101);
    F_MED = add_font(102);
    F_SEMI = add_font(103);
    ImGui::GetIO().FontDefault = F_REG;
}

static void set_palette(bool dark) {
    if (!dark) {
        P.bg = hex(0xF6F4F0);
        P.sidebar = hex(0xEFECE6);
        P.surface = hex(0xFFFFFF);
        P.surface_hover = hex(0xFFFFFF);
        P.surface_active = hex(0xF2EFEA);
        P.border = hex(0xE4DFD6);
        P.border_strong = hex(0xCFC8BC);
        P.text = hex(0x17171A);
        P.text2 = hex(0x6B665E);
        P.text3 = hex(0xA29C92);
        P.accent = hex(0x17171A);
        P.accent_hover = hex(0x34343A);
        P.accent_text = hex(0xFAF8F5);
        P.accent_soft = hex(0xE8E3DA);
        P.ok = hex(0x2F7D5B);
        P.ok_soft = hex(0xDCEDE3);
        P.danger = hex(0xB3443A);
        P.danger_soft = hex(0xF5DEDA);
        P.warn = hex(0xA87822);
        P.warn_soft = hex(0xF4E9D2);
        P.shadow = hex(0x3A3226, 0.07f);
        P.overlay = hex(0x17171A, 0.28f);
        P.selection = hex(0x17171A);
    } else {
        P.bg = hex(0x141416);
        P.sidebar = hex(0x19191C);
        P.surface = hex(0x1F1F23);
        P.surface_hover = hex(0x26262B);
        P.surface_active = hex(0x2C2C32);
        P.border = hex(0x2B2B31);
        P.border_strong = hex(0x3A3A42);
        P.text = hex(0xEDEBE7);
        P.text2 = hex(0xA19D96);
        P.text3 = hex(0x6E6A64);
        P.accent = hex(0xF1EEE8);
        P.accent_hover = hex(0xFFFFFF);
        P.accent_text = hex(0x141416);
        P.accent_soft = hex(0x2E2D2A);
        P.ok = hex(0x5DBB8E);
        P.ok_soft = hex(0x1F3329);
        P.danger = hex(0xE2766B);
        P.danger_soft = hex(0x3A2422);
        P.warn = hex(0xD9A84B);
        P.warn_soft = hex(0x3A3020);
        P.shadow = hex(0x000000, 0.30f);
        P.overlay = hex(0x000000, 0.45f);
        P.selection = hex(0xF1EEE8);
    }
}

void apply_theme(bool dark, float scale) {
    g_dark = dark;
    g_scale = scale;
    set_palette(dark);
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.WindowRounding = 0;
    st.WindowBorderSize = 0;
    st.ChildRounding = 12;
    st.ChildBorderSize = 0;
    st.PopupRounding = 12;
    st.PopupBorderSize = 1;
    st.FramePadding = ImVec2(12, 8);
    st.FrameRounding = 10;
    st.FrameBorderSize = 1;
    st.ItemSpacing = ImVec2(10, 8);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.ScrollbarSize = 10;
    st.ScrollbarRounding = 8;
    st.GrabMinSize = 10;
    st.GrabRounding = 8;
    st.TabRounding = 8;
    st.SelectableTextAlign = ImVec2(0, 0.5f);
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    st.DisplaySafeAreaPadding = ImVec2(0, 0);
    st.SeparatorTextBorderSize = 1;
    st.ScaleAllSizes(scale);
    st.FontSizeBase = kBase;
    st.FontScaleDpi = scale;
    st.HoverDelayNormal = 0.45f;
    st.HoverDelayShort = 0.25f;
    st.HoverStationaryDelay = 0.2f;

    auto c = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    ImVec4* col = st.Colors;
    col[ImGuiCol_Text] = c(P.text);
    col[ImGuiCol_TextDisabled] = c(P.text3);
    col[ImGuiCol_WindowBg] = c(P.bg);
    col[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_PopupBg] = c(P.surface);
    col[ImGuiCol_Border] = c(P.border);
    col[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_FrameBg] = c(P.surface);
    col[ImGuiCol_FrameBgHovered] = c(dark ? P.surface_hover : mix(P.surface, P.bg, 0.5f));
    col[ImGuiCol_FrameBgActive] = c(P.surface_active);
    col[ImGuiCol_TitleBg] = c(P.sidebar);
    col[ImGuiCol_TitleBgActive] = c(P.sidebar);
    col[ImGuiCol_TitleBgCollapsed] = c(P.sidebar);
    col[ImGuiCol_MenuBarBg] = c(P.sidebar);
    col[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ScrollbarGrab] = c(with_alpha(P.text, 0.16f));
    col[ImGuiCol_ScrollbarGrabHovered] = c(with_alpha(P.text, 0.28f));
    col[ImGuiCol_ScrollbarGrabActive] = c(with_alpha(P.text, 0.38f));
    col[ImGuiCol_CheckMark] = c(P.accent_text);
    col[ImGuiCol_SliderGrab] = c(P.accent);
    col[ImGuiCol_SliderGrabActive] = c(P.accent_hover);
    col[ImGuiCol_Button] = c(P.surface);
    col[ImGuiCol_ButtonHovered] = c(dark ? P.surface_hover : P.surface_active);
    col[ImGuiCol_ButtonActive] = c(P.accent_soft);
    col[ImGuiCol_Header] = c(P.accent_soft);
    col[ImGuiCol_HeaderHovered] = c(with_alpha(P.accent_soft, 0.85f));
    col[ImGuiCol_HeaderActive] = c(P.accent_soft);
    col[ImGuiCol_Separator] = c(P.border);
    col[ImGuiCol_SeparatorHovered] = c(P.border_strong);
    col[ImGuiCol_SeparatorActive] = c(P.border_strong);
    col[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_TextSelectedBg] = c(with_alpha(P.accent, 0.18f));
    col[ImGuiCol_DragDropTarget] = c(P.accent);
    col[ImGuiCol_NavCursor] = c(with_alpha(P.accent, 0.6f));
    col[ImGuiCol_ModalWindowDimBg] = c(P.overlay);
    col[ImGuiCol_InputTextCursor] = c(P.text);
}

void clear_color(float out[4]) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(P.bg);
    out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = 1.0f;
}

}  // namespace vs::ui
