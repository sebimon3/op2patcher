#include "fov_settings.h"
#include "mini_json.h"
#include <cmath>
#include <windows.h>

namespace fov {
namespace {
const char* actionNames[] = {"decrease", "increase", "restore", "preset1", "preset2", "preset3"};
const char* oldNames[] = {"decreaseKey", "increaseKey", "restoreKey"};
bool number(const JsonValue* object, const char* key, double& out) {
    if (!object) return false;
    const JsonValue* v = object->find(key);
    if (!v || v->type != JsonValue::Number || !std::isfinite(v->number)) return false;
    out = v->number;
    return true;
}
bool binding_array(const JsonValue* value, hotkeys::Binding& out) {
    if (!value || value->type != JsonValue::Array || value->array.size() > hotkeys::kMaxKeys) return false;
    hotkeys::Binding candidate;
    for (const JsonValue& entry : value->array) {
        if (entry.type != JsonValue::Number || !std::isfinite(entry.number) ||
            entry.number < 8 || entry.number > 254 || (double)(int)entry.number != entry.number) return false;
        candidate.keys.push_back((int)entry.number);
    }
    if (!hotkeys::normalize(candidate)) return false;
    out = candidate;
    return true;
}
bool old_key(int key) {
    return (key >= VK_F1 && key <= VK_F12) || key == VK_PRIOR || key == VK_NEXT ||
           key == VK_HOME || key == VK_END || key == VK_INSERT || key == VK_DELETE;
}
}

void load_settings(const JsonValue* object, Settings& settings) {
    if (!object || object->type != JsonValue::Object) return;
    if (const JsonValue* v = object->find("enabled"))
        settings.enabledForNextLaunch = v->type == JsonValue::Bool && v->boolean;
    double n = 0;
    if (number(object, "desired", n) && n >= kMin && n <= kMax) settings.desired = (float)n;
    if (number(object, "step", n) && n > 0 && n <= 10) settings.step = (float)n;
    if (const JsonValue* values = object->find("presets")) {
        if (values->type == JsonValue::Array) {
            for (size_t i = 0; i < settings.presets.size() && i < values->array.size(); ++i) {
                const JsonValue& v = values->array[i];
                if (v.type == JsonValue::Number && std::isfinite(v.number) && v.number >= kMin && v.number <= kMax)
                    settings.presets[i] = (float)v.number;
            }
        }
    }
    const JsonValue* bindings = object->find("bindings");
    for (int i = 0; i < hotkeys::kActionCount; ++i) {
        const JsonValue* v = bindings && bindings->type == JsonValue::Object ? bindings->find(actionNames[i]) : nullptr;
        if (bindings) {
            // New-format bindings are authoritative. Missing or malformed
            // entries stay unbound instead of reviving an old shortcut.
            if (v) binding_array(v, settings.bindings[(size_t)i]);
        } else if (i < 3 && number(object, oldNames[i], n) && n >= 0 && n <= 255 && (double)(int)n == n && old_key((int)n)) {
            settings.bindings[(size_t)i].keys = {VK_CONTROL, VK_MENU, (int)n};
            hotkeys::normalize(settings.bindings[(size_t)i]);
        }
    }
    if (const JsonValue* v = object->find("hotkeyEnabled"))
        if (v->type == JsonValue::Bool) settings.hotkeysEnabled = v->boolean;
}

std::string settings_json(const Settings& settings) {
    std::string out = "{\n    \"enabled\": " + std::string(settings.enabledForNextLaunch ? "true" : "false") +
        ",\n    \"desired\": " + std::to_string(settings.desired) +
        ",\n    \"step\": " + std::to_string(settings.step) + ",\n    \"presets\": [";
    for (size_t i = 0; i < settings.presets.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(settings.presets[i]);
    }
    out += "],\n    \"bindings\": {\n";
    for (int i = 0; i < hotkeys::kActionCount; ++i) {
        out += "      \"" + std::string(actionNames[i]) + "\": [";
        const auto& keys = settings.bindings[(size_t)i].keys;
        for (size_t k = 0; k < keys.size(); ++k) {
            if (k) out += ", ";
            out += std::to_string(keys[k]);
        }
        out += i + 1 == hotkeys::kActionCount ? "]\n" : "],\n";
    }
    out += "    },\n    \"hotkeyEnabled\": " + std::string(settings.hotkeysEnabled ? "true" : "false") + "\n  }";
    return out;
}
}
