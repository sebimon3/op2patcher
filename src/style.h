#pragma once
#include <string>
#include "imgui.h"

namespace style {
    void apply();
    void help_marker(const char* desc);
    void warn_marker(const char* desc, const char* warning);
    bool colored_button(const char* label, const ImVec4& base, const ImVec2& size);
    bool begin_padded_tab_bar(const char* id);
    bool begin_padded_tab_item(const char* label);
    void end_padded_tab_bar();
    void end_padded_tab_item();
}
