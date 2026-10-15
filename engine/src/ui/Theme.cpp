#include "ui/Theme.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace glue::ui {
namespace {

ImVec4 rgb(unsigned hex, float a = 1.0f) {
    return {((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a};
}
ImVec4 withAlpha(ImVec4 c, float a) { c.w = a; return c; }

Palette makePalette() {
    Palette p;
    // Cool near-black surfaces, a periwinkle accent, soft status colours.
    p.bg0 = rgb(0x0E1018);  p.bg1 = rgb(0x141722);  p.bg2 = rgb(0x1B1F2E);  p.bg3 = rgb(0x252A3D);
    p.border = rgb(0x2A2F44);
    p.text = rgb(0xE6E9F5); p.textDim = rgb(0x9097B0); p.textFaint = rgb(0x5C637C);
    p.accent = rgb(0x7C9CFF); p.accentHover = rgb(0x93AEFF); p.accentActive = rgb(0x6A8AF0);
    p.accentSoft = rgb(0x7C9CFF, 0.18f);
    p.success = rgb(0x5EE6A8); p.warning = rgb(0xFFCF6B); p.danger = rgb(0xFF6B81);
    return p;
}

Palette g_pal = makePalette();
Fonts g_fonts;

float animate(ImGuiID id, float target, float speed = 14.0f) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float cur = st->GetFloat(id, target);
    cur += (target - cur) * std::min(1.0f, speed * ImGui::GetIO().DeltaTime);
    if (std::fabs(cur - target) < 0.002f) cur = target;
    st->SetFloat(id, cur);
    return cur;
}

} // namespace

const Palette& palette() { return g_pal; }
Fonts& fonts() { return g_fonts; }

void install(const std::string& assetsDir, float scale) {
    if (const char* e = std::getenv("GLUE_UI_SCALE")) scale = (float)std::atof(e) > 0 ? (float)std::atof(e) : scale;
    ImGuiIO& io = ImGui::GetIO();
    namespace fs = std::filesystem;

    // ---- fonts ----------------------------------------------------------------------------
    auto load = [&](const char* file, float px) -> ImFont* {
        fs::path p = fs::path(assetsDir) / "fonts" / file;
        if (!fs::exists(p)) return nullptr;
        return io.Fonts->AddFontFromFileTTF(p.string().c_str(), px * scale);
    };
    g_fonts.body = load("Inter-Regular.ttf", 16.0f);
    g_fonts.strong = load("Inter-SemiBold.ttf", 16.0f);
    g_fonts.code = load("JetBrainsMono-Regular.ttf", 15.0f);
    if (!g_fonts.body) g_fonts.body = io.Fonts->AddFontDefault();
    if (!g_fonts.strong) g_fonts.strong = g_fonts.body;
    if (!g_fonts.code) g_fonts.code = g_fonts.body;
    io.FontDefault = g_fonts.body;

    // ---- geometry: generous padding, big radii, no borders on controls ------------------------
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = {14, 12};   s.FramePadding = {10, 6};   s.CellPadding = {8, 5};
    s.ItemSpacing = {10, 8};      s.ItemInnerSpacing = {8, 6}; s.IndentSpacing = 18;
    s.ScrollbarSize = 11;         s.GrabMinSize = 14;
    s.WindowBorderSize = 1;       s.ChildBorderSize = 0;       s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;        s.TabBorderSize = 0;         s.TabBarBorderSize = 1;
    s.WindowRounding = 10;        s.ChildRounding = 8;         s.FrameRounding = 7;
    s.PopupRounding = 10;         s.ScrollbarRounding = 12;    s.GrabRounding = 7;
    s.TabRounding = 7;
    s.WindowTitleAlign = {0.0f, 0.5f};
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = {10, 4};
    s.DockingSeparatorSize = 2;
    s.AntiAliasedLines = true; s.AntiAliasedFill = true;
    s.ScaleAllSizes(scale);

    // ---- colours -------------------------------------------------------------------------
    const Palette& p = g_pal;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = p.text;                 c[ImGuiCol_TextDisabled] = p.textFaint;
    c[ImGuiCol_WindowBg] = p.bg1;              c[ImGuiCol_ChildBg] = withAlpha(p.bg0, 0.0f);
    c[ImGuiCol_PopupBg] = rgb(0x171A27, 0.98f);
    c[ImGuiCol_Border] = p.border;             c[ImGuiCol_BorderShadow] = rgb(0, 0.0f);
    c[ImGuiCol_FrameBg] = p.bg2;               c[ImGuiCol_FrameBgHovered] = p.bg3;
    c[ImGuiCol_FrameBgActive] = rgb(0x2E3450);
    c[ImGuiCol_TitleBg] = p.bg0;               c[ImGuiCol_TitleBgActive] = p.bg0;
    c[ImGuiCol_TitleBgCollapsed] = p.bg0;
    c[ImGuiCol_MenuBarBg] = p.bg0;
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x30364F); c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x424A6B);
    c[ImGuiCol_ScrollbarGrabActive] = p.accent;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;         c[ImGuiCol_SliderGrabActive] = p.accentHover;
    c[ImGuiCol_Button] = p.bg3;                c[ImGuiCol_ButtonHovered] = rgb(0x30375A);
    c[ImGuiCol_ButtonActive] = rgb(0x3A4270);
    c[ImGuiCol_Header] = p.accentSoft;         c[ImGuiCol_HeaderHovered] = withAlpha(p.accent, 0.28f);
    c[ImGuiCol_HeaderActive] = withAlpha(p.accent, 0.38f);
    c[ImGuiCol_Separator] = p.border;          c[ImGuiCol_SeparatorHovered] = p.accent;
    c[ImGuiCol_SeparatorActive] = p.accentHover;
    c[ImGuiCol_ResizeGrip] = withAlpha(p.accent, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(p.accent, 0.55f);
    c[ImGuiCol_ResizeGripActive] = p.accent;
    c[ImGuiCol_Tab] = p.bg0;                   c[ImGuiCol_TabHovered] = withAlpha(p.accent, 0.30f);
    c[ImGuiCol_TabSelected] = p.bg2;           c[ImGuiCol_TabSelectedOverline] = p.accent;
    c[ImGuiCol_TabDimmed] = p.bg0;             c[ImGuiCol_TabDimmedSelected] = p.bg1;
    c[ImGuiCol_TabDimmedSelectedOverline] = p.textFaint;
    c[ImGuiCol_DockingPreview] = withAlpha(p.accent, 0.35f);
    c[ImGuiCol_DockingEmptyBg] = p.bg0;
    c[ImGuiCol_PlotLines] = p.accent;          c[ImGuiCol_PlotHistogram] = p.accent;
    c[ImGuiCol_TableHeaderBg] = p.bg2;         c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight] = withAlpha(p.border, 0.5f);
    c[ImGuiCol_TableRowBg] = rgb(0, 0.0f);     c[ImGuiCol_TableRowBgAlt] = rgb(0xFFFFFF, 0.025f);
    c[ImGuiCol_TextSelectedBg] = withAlpha(p.accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = p.accent;
    c[ImGuiCol_NavCursor] = p.accent;
    c[ImGuiCol_NavWindowingHighlight] = rgb(0xFFFFFF, 0.6f);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, 0.45f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.5f);
}

// ---- controls -----------------------------------------------------------------------------

bool toggle(const char* label, bool* value) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    const Palette& p = g_pal;
    const ImGuiID id = win->GetID(label);
    const float h = ImGui::GetFrameHeight() * 0.78f, w = h * 1.8f;
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, labelEnd);
    const ImVec2 pos = win->DC.CursorPos;
    const float total = w + (labelSize.x > 0 ? ImGui::GetStyle().ItemInnerSpacing.x + labelSize.x : 0);
    const ImRect bb(pos, {pos.x + total, pos.y + ImGui::GetFrameHeight()});
    ImGui::ItemSize(bb, 0);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) *value = !*value;

    const float t = animate(id, *value ? 1.0f : 0.0f);
    const ImVec2 tp = {pos.x, pos.y + (ImGui::GetFrameHeight() - h) * 0.5f};
    ImDrawList* dl = win->DrawList;
    const ImVec4 off = hovered ? p.bg3 : rgb(0x2B3047);
    const ImVec4 mix = {off.x + (p.accent.x - off.x) * t, off.y + (p.accent.y - off.y) * t,
                        off.z + (p.accent.z - off.z) * t, 1.0f};
    dl->AddRectFilled(tp, {tp.x + w, tp.y + h}, ImGui::GetColorU32(mix), h * 0.5f);
    const float r = h * 0.5f - 3.0f;
    const float cx = tp.x + h * 0.5f + t * (w - h);
    dl->AddCircleFilled({cx, tp.y + h * 0.5f + 0.5f}, r + 1.5f, IM_COL32(0, 0, 0, 40), 24);   // soft shadow
    dl->AddCircleFilled({cx, tp.y + h * 0.5f}, r, IM_COL32(255, 255, 255, 255), 24);
    if (labelSize.x > 0)
        ImGui::RenderText({pos.x + w + ImGui::GetStyle().ItemInnerSpacing.x,
                           pos.y + (ImGui::GetFrameHeight() - labelSize.y) * 0.5f}, label, labelEnd);
    return pressed;
}

bool slider(const char* label, float* value, float lo, float hi, const char* fmt, bool logarithmic) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    const Palette& p = g_pal;
    const ImGuiID id = win->GetID(label);
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const float textH = ImGui::GetTextLineHeight();
    const float w = ImGui::CalcItemWidth();
    const ImVec2 pos = win->DC.CursorPos;
    const float height = textH + 18.0f;
    const ImRect bb(pos, {pos.x + w, pos.y + height});
    ImGui::ItemSize(bb, 0);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered, held;
    ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
    bool changed = false;
    const float trackL = pos.x + 7.0f, trackR = pos.x + w - 7.0f, trackY = pos.y + textH + 10.0f;
    if (held && trackR > trackL) {
        float t = std::clamp((ImGui::GetIO().MousePos.x - trackL) / (trackR - trackL), 0.0f, 1.0f);
        const bool lg = logarithmic && lo > 0.0f && hi > lo;
        float v = lg ? lo * std::pow(hi / lo, t) : lo + t * (hi - lo);
        if (std::fabs(v - *value) > 1e-7f) { *value = v; changed = true; }
    }
    const bool lgDraw = logarithmic && lo > 0.0f && hi > lo && *value > 0.0f;
    const float t = hi > lo ? std::clamp(lgDraw ? std::log(*value / lo) / std::log(hi / lo) : (*value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    const float hov = animate(id, (hovered || held) ? 1.0f : 0.0f);
    ImDrawList* dl = win->DrawList;
    ImGui::RenderText(pos, label, labelEnd);
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, *value);
    const ImVec2 vs = ImGui::CalcTextSize(buf);
    dl->AddText({pos.x + w - vs.x, pos.y}, ImGui::GetColorU32(p.textDim), buf);
    const float x = trackL + t * (trackR - trackL);
    dl->AddRectFilled({trackL, trackY - 2.5f}, {trackR, trackY + 2.5f}, ImGui::GetColorU32(rgb(0x2B3047)), 3.0f);
    dl->AddRectFilled({trackL, trackY - 2.5f}, {x, trackY + 2.5f}, ImGui::GetColorU32(p.accent), 3.0f);
    dl->AddCircleFilled({x, trackY}, 6.5f + 1.5f * hov, ImGui::GetColorU32(withAlpha(p.accent, 0.20f * hov)), 24);
    dl->AddCircleFilled({x, trackY}, 6.0f, IM_COL32(255, 255, 255, 255), 24);
    return changed;
}

bool sliderInt(const char* label, int* value, int lo, int hi, const char* fmt, bool logarithmic) {
    // reuse the float slider: show the rounded value with the int format turned into a float one
    std::string f(fmt ? fmt : "%d");
    for (size_t i = f.find("%d"); i != std::string::npos; i = f.find("%d", i + 4)) f.replace(i, 2, "%.0f");
    float v = (float)*value;
    const bool changed = slider(label, &v, (float)lo, (float)hi, f.c_str(), logarithmic);
    const int iv = (int)std::lround(v);
    if (changed && iv != *value) { *value = iv; return true; }
    return false;
}

bool accentButton(const char* label, ImVec2 size) {
    const Palette& p = g_pal;
    ImGui::PushStyleColor(ImGuiCol_Button, p.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.accentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(0x0B0E1A));
    ImGui::PushFont(g_fonts.strong);
    const bool r = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return r;
}

void sectionHeader(const char* text) {
    const Palette& p = g_pal;
    std::string up(text);
    for (char& ch : up) ch = (char)std::toupper((unsigned char)ch);
    ImGui::Dummy({0, 2});
    ImGui::PushStyleColor(ImGuiCol_Text, p.textFaint);
    ImGui::PushFont(g_fonts.strong, ImGui::GetStyle().FontSizeBase * 0.82f);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    dl->AddLine({a.x, a.y - 1}, {a.x + ImGui::GetContentRegionAvail().x, a.y - 1}, ImGui::GetColorU32(p.border));
    ImGui::Dummy({0, 3});
}

void chip(const char* text, ImVec4 colour) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad = {10, 3};
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size = {ts.x + pad.x * 2, ts.y + pad.y * 2};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, ImGui::GetColorU32(withAlpha(colour, 0.16f)), size.y * 0.5f);
    dl->AddCircleFilled({pos.x + 10, pos.y + size.y * 0.5f}, 3.0f, ImGui::GetColorU32(colour));
    dl->AddText({pos.x + pad.x + 8, pos.y + pad.y}, ImGui::GetColorU32(colour), text);
    ImGui::Dummy({size.x + 8, size.y});
}

bool listRow(const char* label, bool selected) {
    const Palette& p = g_pal;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::Selectable(label, selected, 0, {0, ImGui::GetFrameHeight() - 2});
    if (selected) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        (void)pos;
        dl->AddRectFilled({mn.x, mn.y + 5}, {mn.x + 3, mx.y - 5}, ImGui::GetColorU32(p.accent), 2.0f);
    }
    return clicked;
}

} // namespace glue::ui
