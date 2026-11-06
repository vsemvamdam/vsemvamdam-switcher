// vsemvamdam-switcher — custom-drawn widgets
#pragma once
#include <string>

#include "imgui.h"

namespace vs::ui {

bool icon_button(const char* id, const char* icon, const char* tooltip = nullptr, bool active = false, float size = 34.0f);
bool pill_button(const char* label, bool primary, ImVec2 size = ImVec2(0, 0), bool enabled = true, const char* icon = nullptr);
bool search_box(const char* id, char* buf, size_t buf_size, const char* hint, float width);
bool toggle(const char* id, bool* v);
bool segmented(const char* id, int* current, const char* const* labels, int count, float width = 0.0f);
void spinner(ImDrawList* dl, ImVec2 center, float radius, ImU32 col, float thickness);
void text_clipped(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float max_w, ImU32 col, const std::string& s, bool center = false);
std::string ellipsize(ImFont* font, float size, const std::string& s, float max_w);
void soft_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 col, float spread);
void badge(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 fg, ImU32 bg, float size);  // pos = top-left
ImVec2 badge_size(const char* text, float size);
bool menu_item(const char* icon, const char* label, const char* hint = nullptr, bool enabled = true, bool danger = false);
bool begin_menu(const char* icon, const char* label, bool enabled = true);
void tooltip(const char* text);
void section_label(const char* text);
// labelled settings row: left label + description, right control drawn by caller after this returns
void setting_row_begin(const char* title, const char* desc, float control_w);
void setting_row_end();
float anim(ImGuiID id, bool target, float speed = 12.0f);  // smooth 0..1 animation
void countdown_ring(ImDrawList* dl, ImVec2 center, float radius, float remaining_frac, ImU32 col, ImU32 track, float thickness);
// modal dialogs (centered card)
bool begin_dialog(const char* id, const char* title, float width);
void dialog_text(const std::string& t);
// Two buttons right-aligned; returns 1 = primary, 2 = secondary (also Esc)
int dialog_buttons(const char* primary, const char* secondary, bool primary_enabled = true, bool danger = false);
void dialog_input(const char* id, char* buf, size_t n, const char* hint, bool multiline = false, int flags = 0);

}  // namespace vs::ui
