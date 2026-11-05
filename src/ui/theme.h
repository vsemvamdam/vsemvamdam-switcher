// vsemvamdam-switcher — palette, fonts and ImGui style
#pragma once
#include "imgui.h"

namespace vs::ui {

struct Palette {
    ImU32 bg, sidebar, surface, surface_hover, surface_active, border, border_strong;
    ImU32 text, text2, text3;
    ImU32 accent, accent_hover, accent_text, accent_soft;
    ImU32 ok, ok_soft, danger, danger_soft, warn, warn_soft;
    ImU32 shadow, overlay, selection;
};

extern Palette P;
extern bool g_dark;
extern float g_scale;  // DPI scale
extern ImFont* F_REG;
extern ImFont* F_MED;
extern ImFont* F_SEMI;

constexpr float kBase = 14.5f;   // body size (unscaled)

void load_fonts();
void apply_theme(bool dark, float dpi_scale);
void clear_color(float out[4]);
inline float S(float v) { return v * g_scale; }   // scale a pixel size

ImU32 with_alpha(ImU32 c, float a);
ImU32 mix(ImU32 a, ImU32 b, float t);

}  // namespace vs::ui
