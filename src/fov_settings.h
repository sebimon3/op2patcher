#pragma once
#include "hotkeys.h"
#include <array>
#include <string>

struct JsonValue;
namespace fov {
constexpr float kMin = 10.0f;
constexpr float kMax = 180.0f;
constexpr float kDefault = 67.5f;
enum Action { Decrease, Increase, Restore, Preset1, Preset2, Preset3 };

struct Settings {
    bool enabledForNextLaunch = false;
    float desired = kDefault;
    float step = 2.5f;
    std::array<float, 3> presets{{67.5f, 80.0f, 90.0f}};
    hotkeys::Bindings bindings;
    bool hotkeysEnabled = false;
};

void load_settings(const JsonValue* object, Settings& settings);
std::string settings_json(const Settings& settings);
}
