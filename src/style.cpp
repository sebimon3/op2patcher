#include "style.h"
#include "imgui_internal.h"
#include <algorithm>

namespace {
    const ImVec4 TOOLTIP_FG(0.910f, 0.914f, 0.922f, 1.0f);
    const ImVec4 TOOLTIP_BG(0.192f, 0.212f, 0.220f, 1.0f);
    const ImVec4 WARN_FG(1.0f, 1.0f, 0.0f, 1.0f);

    void tooltip(const char* desc, const char* warning) {
        ImGui::PushStyleColor(ImGuiCol_Border, TOOLTIP_BG);
        ImGui::PushStyleColor(ImGuiCol_BorderShadow, TOOLTIP_BG);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, TOOLTIP_BG);
        ImGui::PushStyleColor(ImGuiCol_Text, TOOLTIP_FG);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        if (desc && desc[0]) {
            ImGui::TextUnformatted(desc);
            if (warning && warning[0]) ImGui::TextUnformatted("");
        }
        if (warning && warning[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, WARN_FG);
            ImGui::TextUnformatted("WARNING:");
            ImGui::TextUnformatted(warning);
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleColor(4);
    }

    ImVec2 padded_tab_frame_padding() {
        ImVec2 base = ImGui::GetStyle().FramePadding;
        return ImVec2(base.x + 6.0f, base.y + 1.0f);
    }
}

namespace style {
    void apply() {
        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 0.0f;
        s.FrameRounding = 0.0f;
        s.GrabRounding = 0.0f;
        s.TabRounding = 0.0f;
        s.ScrollbarRounding = 0.0f;
        s.WindowPadding = ImVec2(14, 12);
        s.FramePadding = ImVec2(8, 5);
        s.ItemSpacing = ImVec2(8, 2);

        // Dark red application palette.
        ImVec4* c = s.Colors;
        c[ImGuiCol_Text]                  = ImVec4(1.00f, 0.90f, 0.90f, 1.00f);
        c[ImGuiCol_TextDisabled]          = ImVec4(0.35f, 0.26f, 0.26f, 1.00f);
        c[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.07f, 0.07f, 1.00f);
        c[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.09f, 0.09f, 1.00f);
        c[ImGuiCol_PopupBg]               = ImVec4(0.08f, 0.07f, 0.07f, 0.96f);
        c[ImGuiCol_Border]                = ImVec4(0.25f, 0.15f, 0.15f, 0.80f);
        c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_FrameBg]               = ImVec4(0.15f, 0.10f, 0.10f, 1.00f);
        c[ImGuiCol_FrameBgHovered]        = ImVec4(0.25f, 0.15f, 0.15f, 1.00f);
        c[ImGuiCol_FrameBgActive]         = ImVec4(0.35f, 0.20f, 0.20f, 1.00f);
        c[ImGuiCol_TitleBg]               = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
        c[ImGuiCol_TitleBgActive]         = ImVec4(0.25f, 0.10f, 0.10f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
        c[ImGuiCol_MenuBarBg]             = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
        c[ImGuiCol_ScrollbarBg]           = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
        c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.25f, 0.12f, 0.12f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.35f, 0.15f, 0.15f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.45f, 0.20f, 0.20f, 1.00f);
        c[ImGuiCol_CheckMark]             = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);
        c[ImGuiCol_SliderGrab]            = ImVec4(0.60f, 0.12f, 0.12f, 1.00f);
        c[ImGuiCol_SliderGrabActive]      = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);
        c[ImGuiCol_Button]                = ImVec4(0.30f, 0.12f, 0.12f, 1.00f);
        c[ImGuiCol_ButtonHovered]         = ImVec4(0.50f, 0.18f, 0.18f, 1.00f);
        c[ImGuiCol_ButtonActive]          = ImVec4(0.70f, 0.25f, 0.25f, 1.00f);
        c[ImGuiCol_Header]                = ImVec4(0.30f, 0.12f, 0.12f, 1.00f);
        c[ImGuiCol_HeaderHovered]         = ImVec4(0.50f, 0.18f, 0.18f, 1.00f);
        c[ImGuiCol_HeaderActive]          = ImVec4(0.70f, 0.25f, 0.25f, 1.00f);
        c[ImGuiCol_Tab]                   = ImVec4(0.15f, 0.10f, 0.10f, 1.00f);
        c[ImGuiCol_TabHovered]            = ImVec4(0.50f, 0.18f, 0.18f, 1.00f);
        c[ImGuiCol_TabActive]             = ImVec4(0.35f, 0.12f, 0.12f, 1.00f);
        c[ImGuiCol_PlotLines]             = ImVec4(0.85f, 0.20f, 0.20f, 1.00f);
        c[ImGuiCol_TextSelectedBg]        = ImVec4(0.85f, 0.15f, 0.15f, 0.35f);
        c[ImGuiCol_NavHighlight]          = ImVec4(0.85f, 0.15f, 0.15f, 1.00f);
        c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.025f);
        c[ImGuiCol_Separator]             = ImVec4(0.32f, 0.22f, 0.22f, 1.00f);
        c[ImGuiCol_SeparatorHovered]      = ImVec4(0.42f, 0.22f, 0.22f, 1.00f);
        c[ImGuiCol_SeparatorActive]       = ImVec4(0.52f, 0.22f, 0.22f, 1.00f);
    }

    void help_marker(const char* desc) {
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) tooltip(desc, nullptr);
    }

    void warn_marker(const char* desc, const char* warning) {
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, WARN_FG);
        ImGui::TextDisabled("(!)");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) tooltip(desc, warning);
    }

    bool colored_button(const char* label, const ImVec4& base, const ImVec2& size) {
        auto brighten = [](const ImVec4& c, float d) {
            return ImVec4((std::min)(c.x + d, 1.0f), (std::min)(c.y + d, 1.0f),
                          (std::min)(c.z + d, 1.0f), c.w);
        };
        ImGui::PushStyleColor(ImGuiCol_Button, base);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, brighten(base, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, brighten(base, 0.22f));
        bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        return clicked;
    }

    bool begin_padded_tab_bar(const char* id, float reservedRight) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padded_tab_frame_padding());
        // ImGui draws its tab-bar border from (bottom - 1) to bottom, which
        // leaves the active tab box touching the line. Disable that built-in
        // border and draw our own line a couple of pixels lower instead.
        ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float barBottom = origin.y + ImGui::GetFontSize() +
            ImGui::GetStyle().FramePadding.y * 2.0f;
        // BeginTabBar takes its width from WorkRect, not SetNextItemWidth.
        // Reserve only the header width; restore full-width tab contents.
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const float fullRight = window->WorkRect.Max.x;
        window->WorkRect.Max.x = (std::max)(window->DC.CursorPos.x, fullRight - reservedRight);
        bool opened = ImGui::BeginTabBar(id);
        window->WorkRect.Max.x = fullRight;
        const float lineY = barBottom + 2.0f;
        const float pad = ImGui::GetStyle().WindowPadding.x * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(origin.x - pad, lineY),
            ImVec2(origin.x + ImGui::GetContentRegionAvail().x + pad, lineY + 1.0f),
            ImGui::GetColorU32(ImGuiCol_TabSelected));
        // BeginTabBar() left the cursor just below the strip; move it past our
        // separator too so the tab contents don't collide with the line.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        return opened;
    }

    bool begin_padded_tab_item(const char* label) {
        const float uniform = 76.0f;
        const float space = ImGui::CalcTextSize(" ").x;
        const float width = ImGui::CalcTextSize(label).x;
        int pad = (space > 0.0f) ? (int)((uniform - width) * 0.5f / space) : 0;
        if (pad < 0) pad = 0;
        std::string padded;
        for (int k = 0; k < pad; ++k) padded.push_back(' ');
        padded += label;
        for (int k = 0; k < pad; ++k) padded.push_back(' ');
        padded += "##";
        padded += label;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padded_tab_frame_padding());
        bool open = ImGui::BeginTabItem(padded.c_str());
        ImGui::PopStyleVar();
        return open;
    }

    void end_padded_tab_bar() {
        ImGui::EndTabBar();
        ImGui::PopStyleVar(); // TabBarBorderSize
        ImGui::PopStyleVar(); // FramePadding
    }
    void end_padded_tab_item() { ImGui::EndTabItem(); }
}
