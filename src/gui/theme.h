#pragma once

#include <imgui.h>

// Slate/"forge" dark theme + small widgets used to match the dashboard mockup.
namespace openitc::gui::theme {

inline const ImVec4 bg     {0.008f, 0.024f, 0.090f, 1.0f};  // #020617 slate-950
inline const ImVec4 panel  {0.059f, 0.090f, 0.165f, 1.0f};  // #0f172a slate-900
inline const ImVec4 slot   {0.086f, 0.125f, 0.196f, 1.0f};  // #16203-2 slate-800
inline const ImVec4 term   {0.035f, 0.035f, 0.043f, 1.0f};  // #09090b
inline const ImVec4 border {0.118f, 0.161f, 0.231f, 1.0f};  // #1e293b slate-800
inline const ImVec4 text   {0.796f, 0.835f, 0.882f, 1.0f};  // #cbd5e1 slate-300
inline const ImVec4 dim    {0.392f, 0.455f, 0.545f, 1.0f};  // #64748b slate-500
inline const ImVec4 faint  {0.216f, 0.255f, 0.318f, 1.0f};  // #374151
inline const ImVec4 cyan   {0.133f, 0.827f, 0.933f, 1.0f};  // #22d3ee
inline const ImVec4 blue   {0.231f, 0.510f, 0.965f, 1.0f};  // #3b82f6
inline const ImVec4 purple {0.753f, 0.518f, 0.988f, 1.0f};  // #c084fc
inline const ImVec4 orange {0.992f, 0.729f, 0.455f, 1.0f};  // #fdba74
inline const ImVec4 green  {0.290f, 0.870f, 0.502f, 1.0f};  // #4ade80
inline const ImVec4 yellow {0.992f, 0.945f, 0.541f, 1.0f};  // #fde68a
inline const ImVec4 pink   {0.976f, 0.658f, 0.831f, 1.0f};  // #f9a8d4
inline const ImVec4 red    {0.973f, 0.443f, 0.443f, 1.0f};  // #f87171
inline const ImVec4 amber  {0.984f, 0.749f, 0.141f, 1.0f};  // #fbbf24
inline const ImVec4 slate  {0.580f, 0.639f, 0.722f, 1.0f};  // #94a3b8 slate-400

inline ImU32 u(const ImVec4& c) { return ImGui::GetColorU32(c); }

inline ImVec4 type_color(int t) {
    switch (t) {
        case 0x02: return green;
        case 0x05: return slate;
        case 0x06: return yellow;
        case 0x07: return pink;
        case 0x70: return purple;
        case 0x33:
        case 0x34: return cyan;
        default: return text;
    }
}

inline void apply() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 9.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = dim;
    c[ImGuiCol_FrameBg] = slot;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.118f, 0.161f, 0.231f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.153f, 0.204f, 0.286f, 1.0f);
    c[ImGuiCol_Button] = slot;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.23f, 0.31f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.13f, 0.20f, 0.34f, 1.0f);
    c[ImGuiCol_Header] = slot;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.22f, 0.32f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.13f, 0.20f, 0.34f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = bg;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.09f, 0.12f, 0.18f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.25f, 0.33f, 1.0f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_CheckMark] = cyan;
    c[ImGuiCol_SliderGrab] = cyan;
    c[ImGuiCol_PlotLines] = cyan;
    c[ImGuiCol_PlotHistogram] = cyan;
    c[ImGuiCol_TitleBg] = panel;
    c[ImGuiCol_TitleBgActive] = panel;
}

// A rounded "card" region with an optional gradient top strip and border.
inline bool begin_card(const char* id, const ImVec2& size, bool grad = true,
                       bool scroll = false) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    ImGuiWindowFlags f = scroll ? 0 : ImGuiWindowFlags_NoScrollbar;
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_None, f);
    if (open && grad) {
        const ImVec2 p = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowWidth();
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            p, ImVec2(p.x + w, p.y + 3), u(blue), u(cyan), u(cyan), u(blue));
    }
    return open;
}

inline void end_card() {
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 z = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + z.x, p.y + z.y), u(border), 8.0f, 0,
                                        1.0f);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Rounded pill. Leaves the cursor on the same line.
inline void chip(const char* txt, const ImVec4& fg, const ImVec4& bgc) {
    const ImVec2 pad(8, 3);
    const ImVec2 tsz = ImGui::CalcTextSize(txt);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pmax(p.x + tsz.x + pad.x * 2, p.y + tsz.y + pad.y * 2);
    dl->AddRectFilled(p, pmax, u(bgc), 6.0f);
    dl->AddRect(p, pmax, u(border), 6.0f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), u(fg), txt);
    ImGui::Dummy(ImVec2(tsz.x + pad.x * 2, tsz.y + pad.y * 2));
    ImGui::SameLine(0, 8);
}

inline void right_text(const ImVec4& c, const char* t) {
    const float w = ImGui::CalcTextSize(t).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float x = ImGui::GetCursorPosX() + (avail > w ? avail - w : 0.0f);
    ImGui::SetCursorPosX(x);
    ImGui::TextColored(c, "%s", t);
}

}  // namespace openitc::gui::theme
