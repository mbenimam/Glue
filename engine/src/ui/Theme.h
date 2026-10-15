#pragma once
#include <imgui.h>
#include <string>

// A modern look for Dear ImGui: one palette, Inter for the interface, JetBrains Mono for code, soft
// rounded geometry, and a few custom controls (switch, slider, section headers, chips). It is a
// style layer only: every widget here is a normal ImGui widget or a few draw-list calls.
namespace glue::ui {

struct Palette {
    ImVec4 bg0, bg1, bg2, bg3;          // window, panel, control, control hover  (dark to light)
    ImVec4 border;                      // hairlines
    ImVec4 text, textDim, textFaint;    // primary, secondary, disabled
    ImVec4 accent, accentHover, accentActive, accentSoft;  // brand colour and tints of it
    ImVec4 success, warning, danger;
};

struct Fonts {
    ImFont* body = nullptr;     // Inter Regular, the interface
    ImFont* strong = nullptr;   // Inter SemiBold, headings and buttons
    ImFont* code = nullptr;     // JetBrains Mono, editors and logs
};

const Palette& palette();
Fonts& fonts();

// Loads the fonts from <assetsDir>/fonts (falls back to ImGui's default font when they are missing),
// and applies the style and colours. Call once, right after ImGui::CreateContext().
// `scale` multiplies sizes and spacing (use 1.5 or 2 on a high-DPI screen; $GLUE_UI_SCALE overrides).
void install(const std::string& assetsDir, float scale = 1.0f);

// ---- controls -------------------------------------------------------------------------------
// iOS-style animated switch with the label on its right. Returns true when toggled.
bool toggle(const char* label, bool* value);
// Label on the left, value on the right, a thin track with a round knob underneath.
bool slider(const char* label, float* value, float lo, float hi, const char* fmt = "%.2f", bool logarithmic = false);
// Integer version of slider(): the value snaps to whole numbers; `fmt` is a printf format for an int.
bool sliderInt(const char* label, int* value, int lo, int hi, const char* fmt = "%d", bool logarithmic = false);
// A filled accent button (the primary action of a panel).
bool accentButton(const char* label, ImVec2 size = {0, 0});
// Small upper-case dim title with a hairline, to group controls.
void sectionHeader(const char* text);
// A rounded pill of text, e.g. "compiled" or "error".
void chip(const char* text, ImVec4 colour);
// A list row with an accent bar on the left when selected. Returns true when clicked.
bool listRow(const char* label, bool selected);

} // namespace glue::ui
