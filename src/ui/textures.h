// vsemvamdam-switcher — async image loading into D3D11 textures
#pragma once
#include <string>

#include "../core/image.h"
#include "imgui.h"

namespace vs::ui {

// Returns a texture for an image file (decoded off-thread, resized to `px`, corners rounded by `round` fraction).
// Returns 0 while loading or if the file can't be decoded.
ImTextureID tex_file(const std::string& path, int px, float round = 0.0f);
// Icon from an executable (or .ico/.dll); 0 while loading.
ImTextureID tex_exe_icon(const std::string& exe, int px);
// Image embedded in the exe as an RCDATA resource (PNG/JPG), resized to `px`; 0 while loading.
ImTextureID tex_resource(int id, int px);
// Placeholder tile texture by seed
ImTextureID tex_placeholder(unsigned seed, int px, float round, bool dark = false);
void tex_pump();           // UI thread: upload finished decodes
void tex_clear();          // drop everything (e.g. after refresh)
void tex_forget(const std::string& path);
void tex_shutdown();

}  // namespace vs::ui
