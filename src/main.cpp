#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>
#include <dwmapi.h>
#include <d3d9.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"

#include "mini_json.h"
#include "patcher.h"
#include "fov_controller.h"
#include "autowalk.h"
#include "style.h"
#include "game_paths.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static const char* WINDOW_TITLE = "OP2.2 Patch Manager";
static const int WINDOW_W = 800;
static const int WINDOW_H = 600;
static const float PATCH_ROW_HEIGHT = 22.0f;
// One gap value used between all top-section blocks and info lines.
static const float TOP_SECTION_GAP = 4.0f;
static const float INFO_LINE_STEP = 18.0f;

static HWND g_hwnd = nullptr;
static IDirect3D9* g_d3d = nullptr;
static IDirect3DDevice9* g_device = nullptr;
static D3DPRESENT_PARAMETERS g_pp{};

struct AppState {
    std::vector<JsonValue> definitions;
    std::vector<bool> enabled;
    std::vector<LogLine> log;
    char exePath[MAX_PATH] = "";
    char workingDir[MAX_PATH] = "";
    std::string status = "Ready.";
    std::string fovWarning;
    bool launchFailed = false;
};
static AppState g_app;

// One controller for the whole app; its lifetime is independent of tab selection
// and of the ImGui/D3D render state.
static fov::Controller g_fov;
// Owns the launched child handle independently of Dynamic FOV and keeps the
// single-game session guard active even when Autowalk itself is disabled.
static autowalk::Controller g_autowalk;

static fov::Settings g_fovOpts;
static autowalk::Settings g_autowalkOpts;
static std::string g_autowalkWarning;
static hotkeys::Capture g_keyCapture;
enum class CaptureTarget { None, FovAction, AutowalkToggle };
static CaptureTarget g_captureTarget = CaptureTarget::None;
static int g_captureAction = -1;
static bool g_openCapture = false;
static std::string g_captureMessage;

// Which path field is currently being edited ("exe" / "cwd"), plus a one-frame
// request to focus the InputText as soon as it appears.
static std::string g_editField;
static bool g_editFocusPending = false;

static const char* fov_action_name(int action);

static std::string app_dir() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? s : s.substr(0, slash);
}

static std::string join_path(const std::string& dir, const std::string& name) {
    return dir + "\\" + name;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f << data;
}

static void load_definitions() {
    std::string text = read_file(join_path(app_dir(), "patches.json"));
    JsonValue root;
    std::string err;
    if (text.empty() || !json_parse(text, root, err) || root.type != JsonValue::Array) {
        g_app.log.push_back({"Could not load patches.json: " + (err.empty() ? "file missing or invalid" : err), true});
        return;
    }
    g_app.definitions.clear();
    for (const JsonValue& def : root.array) {
        const std::string id = def.str("id");
        if (id != "dynamic_fov" && id != "wide_fov_render") g_app.definitions.push_back(def);
    }
    g_app.enabled.assign(g_app.definitions.size(), false);
}

static void autodetect_paths() {
    const game_paths::Paths paths = game_paths::autodetect(app_dir());
    std::snprintf(g_app.exePath, MAX_PATH, "%s", paths.gameExe.c_str());
    std::snprintf(g_app.workingDir, MAX_PATH, "%s", paths.workingDir.c_str());
}

static bool valid_binding_value(const JsonValue* value) {
    if (!value || value->type != JsonValue::Array || value->array.size() > hotkeys::kMaxKeys) return false;
    hotkeys::Binding candidate;
    for (const JsonValue& key : value->array) {
        if (key.type != JsonValue::Number || !std::isfinite(key.number) ||
            key.number < 8 || key.number > 254 || (double)(int)key.number != key.number) return false;
        candidate.keys.push_back((int)key.number);
    }
    return hotkeys::normalize(candidate);
}

static bool valid_autowalk_config(const JsonValue* value) {
    if (!value || value->type != JsonValue::Object) return false;
    if (const JsonValue* enabled = value->find("enabled"))
        if (enabled->type != JsonValue::Bool) return false;
    const JsonValue* toggle = value->find("toggleBinding");
    return valid_binding_value(toggle);
}

static void load_autowalk_config(const JsonValue* value) {
    g_autowalkOpts = autowalk::Settings{};
    g_autowalkWarning.clear();
    if (!value) return;
    if (!valid_autowalk_config(value)) {
        g_autowalkWarning = "Invalid Autowalk settings were disabled and unbound.";
        return;
    }

    autowalk::Settings loaded;
    autowalk::load_settings(value, loaded);
    if (!loaded.toggle.keys.empty()) {
        for (int i = 0; i < hotkeys::kActionCount; ++i) {
            if (g_fovOpts.bindings[(size_t)i].keys.empty()) continue;
            if (autowalk::conflicts(loaded.toggle, g_fovOpts.bindings[(size_t)i])) {
                g_autowalkWarning = std::string("Autowalk toggle conflicts with ") +
                    fov_action_name(i) + "; Autowalk was disabled and unbound.";
                return;
            }
        }
    }
    g_autowalkOpts = loaded;
}

static void load_config() {
    std::string text = read_file(join_path(app_dir(), "config.json"));
    JsonValue root;
    std::string err;
    if (text.empty() || !json_parse(text, root, err) || root.type != JsonValue::Object) return;

    // A saved path is only honored when it still exists. Invalid/stale entries
    // leave the autodetected (or empty) values in place.
    game_paths::Paths paths{g_app.exePath, g_app.workingDir};
    game_paths::apply_saved(paths, app_dir(), root.str("gameExe"), root.str("workingDir"));
    std::snprintf(g_app.exePath, MAX_PATH, "%s", paths.gameExe.c_str());
    std::snprintf(g_app.workingDir, MAX_PATH, "%s", paths.workingDir.c_str());
    const JsonValue* enabled = root.find("enabled");
    if (enabled && enabled->type == JsonValue::Array) {
        for (const JsonValue& v : enabled->array) {
            if (v.type != JsonValue::String) continue;
            for (size_t k = 0; k < g_app.definitions.size(); ++k) {
                if (g_app.definitions[k].str("id") == v.text) g_app.enabled[k] = true;
            }
        }
    }

    // Old Ctrl+Alt bindings migrate without changing their meaning.
    const JsonValue* fovSettings = root.find("fov");
    fov::load_settings(fovSettings, g_fovOpts);
    const JsonValue* fovEnabled = fovSettings ? fovSettings->find("enabled") : nullptr;
    if (fovEnabled) {
        // Explicit malformed values must not fall back to the legacy selection.
        g_fovOpts.enabledForNextLaunch = fovEnabled->type == JsonValue::Bool && fovEnabled->boolean;
    } else {
        g_fovOpts.enabledForNextLaunch = false;
        // A malformed FOV object also fails closed rather than reviving an old selection.
        if ((!fovSettings || fovSettings->type == JsonValue::Object) && enabled && enabled->type == JsonValue::Array) {
            for (const JsonValue& v : enabled->array) {
                if (v.type == JsonValue::String && v.text == "dynamic_fov") {
                    g_fovOpts.enabledForNextLaunch = true;
                    break;
                }
            }
        }
    }
    load_autowalk_config(root.find("autowalk"));
}

static void save_config() {
    std::string out = "{\n";
    out += "  \"gameExe\": " + json_quote(g_app.exePath) + ",\n";
    out += "  \"workingDir\": " + json_quote(g_app.workingDir) + ",\n";
    out += "  \"enabled\": [";
    bool first = true;
    for (size_t k = 0; k < g_app.definitions.size(); ++k) {
        if (!g_app.enabled[k]) continue;
        if (!first) out += ", ";
        out += json_quote(g_app.definitions[k].str("id"));
        first = false;
    }
    out += "],\n";
    out += "  \"autowalk\": " + autowalk::settings_json(g_autowalkOpts) + ",\n";
    out += "  \"fov\": " + fov::settings_json(g_fovOpts) + "\n}\n";
    write_file(join_path(app_dir(), "config.json"), out);
}

static POINT center_on_primary(int w, int h) {
    POINT pos{0, 0};
    HMONITOR mon = MonitorFromPoint(pos, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        pos.x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        pos.y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    }
    return pos;
}

static bool create_device() {
    g_pp = {};
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = FALSE;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_pp.BackBufferWidth = (UINT)(rc.right - rc.left);
    g_pp.BackBufferHeight = (UINT)(rc.bottom - rc.top);

    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;
    HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &g_pp, &g_device);
    if (FAILED(hr)) {
        hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, g_hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &g_device);
    }
    return SUCCEEDED(hr);
}

static void cleanup_device() {
    if (g_device) { g_device->Release(); g_device = nullptr; }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // While recording a chord, keys are sampled directly; don't let them also
    // activate ImGui navigation or trigger the launcher's Alt+F4 handling.
    if (g_captureTarget != CaptureTarget::None && (msg == WM_KEYDOWN || msg == WM_KEYUP ||
        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR || msg == WM_SYSCHAR)) return 0;
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
    switch (msg) {
        case WM_GETMINMAXINFO: {
            RECT rc{0, 0, 540, 300};
            DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
            DWORD ex = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            if (AdjustWindowRectEx(&rc, style, FALSE, ex)) {
                MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
                mmi->ptMinTrackSize.x = rc.right - rc.left;
                mmi->ptMinTrackSize.y = rc.bottom - rc.top;
            }
            return 0;
        }
        case WM_SIZE:
            if (g_device && wParam != SIZE_MINIMIZED) {
                g_pp.BackBufferWidth = (UINT)LOWORD(lParam);
                g_pp.BackBufferHeight = (UINT)HIWORD(lParam);
                ImGui_ImplDX9_InvalidateDeviceObjects();
                HRESULT hr = g_device->Reset(&g_pp);
                if (SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
            }
            return 0;
        case WM_DESTROY:
            // Detach runtime controllers before the window/render teardown. Both
            // controllers close only their handles; neither terminates the game.
            g_autowalk.detach();
            g_fov.detach();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

static void render_copy_button(const char* value) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 1));
    if (ImGui::Button("Copy")) {
        ImGui::SetClipboardText(value);
        g_app.status = "Copied to clipboard.";
    }
    ImGui::PopStyleVar();
}

// Read-only looking green text that turns into an editable InputText on click.
static void render_path_line(const char* id, const char* label, const char* help,
                             char* value, bool copyButton) {
    ImGui::PushID(id);
    const ImVec4 green(0.20f, 1.0f, 0.35f, 1.0f);
    const ImVec4 dim(0.48f, 0.40f, 0.40f, 1.0f);

    style::help_marker(help);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    bool editing = g_editField == id;
    if (editing) {
        float width = ImGui::GetContentRegionAvail().x - (copyButton ? 48.0f : 4.0f);
        if (width < 80.0f) width = 80.0f;
        ImGui::SetNextItemWidth(width);
        if (g_editFocusPending) {
            ImGui::SetKeyboardFocusHere();
            g_editFocusPending = false;
        }
        bool committed = ImGui::InputText("##path_edit", value, MAX_PATH,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        bool deactivated = ImGui::IsItemDeactivated();
        if (committed || deactivated) {
            g_editField.clear();
            save_config();
            g_app.status = "Paths updated.";
        }
        if (copyButton) {
            ImGui::SameLine();
            render_copy_button(value);
        }
    } else {
        const char* shown = value[0] ? value : "(click to set path)";
        ImGui::PushStyleColor(ImGuiCol_Text, value[0] ? green : dim);
        ImGui::TextUnformatted(shown);
        bool clicked = ImGui::IsItemClicked(0);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (copyButton) {
            ImGui::SameLine();
            render_copy_button(value);
        }
        if (clicked) {
            g_editField = id;
            g_editFocusPending = true;
            g_app.status = "Editing path - press Enter to save.";
        }
    }
    ImGui::PopID();
}

static void render_info_line(const char* label, const std::string& value, const char* help) {
    style::help_marker(help);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    const ImVec4 green(0.20f, 1.0f, 0.35f, 1.0f);
    ImGui::TextColored(green, "%s", value.c_str());
}

// Larger status box: empty for OFF, bold red check for ON.
// The box itself is a click target, just like the patch-name text.
static void draw_status_cell(int patchIndex, const ImVec4& red, const ImVec4& off) {
    const float boxSize = 14.0f;
    // Center the box/text block vertically inside the 22px patch row.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (PATCH_ROW_HEIGHT - boxSize) * 0.5f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::string id = "##chk" + std::to_string(patchIndex);
    if (ImGui::InvisibleButton(id.c_str(), ImVec2(boxSize, boxSize))) {
        size_t idx = (size_t)patchIndex;
        g_app.enabled[idx] = !g_app.enabled[idx];
        save_config();
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    bool on = g_app.enabled[(size_t)patchIndex];
    ImVec2 boxMin(p.x, p.y);
    ImVec2 boxMax(p.x + boxSize, p.y + boxSize);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImVec4(0.11f, 0.08f, 0.08f, 1.0f)));
    dl->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImVec4(0.42f, 0.34f, 0.34f, 1.0f)),
        0.0f, 0, 1.0f);
    if (on) {
        const float cx = boxMin.x + 2.0f;
        const float cy = boxMin.y + 7.0f;
        dl->AddLine(ImVec2(cx, cy + 0.5f), ImVec2(cx + 4.5f, cy + 5.0f),
            ImGui::GetColorU32(red), 2.4f);
        dl->AddLine(ImVec2(cx + 4.5f, cy + 5.0f), ImVec2(cx + 11.0f, cy - 3.5f),
            ImGui::GetColorU32(red), 2.4f);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + boxSize + 3.0f,
        p.y + (boxSize - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextColored(on ? red : off, "%s", on ? "ON" : "off");
}

static const char* fov_action_name(int action) {
    static const char* names[] = {"Decrease", "Increase", "Restore", "Preset 1", "Preset 2", "Preset 3"};
    return action >= 0 && action < hotkeys::kActionCount ? names[action] : "Unknown";
}

static void binding_control(int action) {
    ImGui::PushID(action);
    const std::string label = hotkeys::describe(g_fovOpts.bindings[(size_t)action]);
    if (ImGui::Button((label + "##binding").c_str(), ImVec2(170, 0))) {
        g_captureTarget = CaptureTarget::FovAction;
        g_captureAction = action;
        g_keyCapture.reset();
        g_captureMessage.clear();
        g_openCapture = true;
        ImGui::GetIO().ClearInputKeys();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to record: %s", label.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        g_fovOpts.bindings[(size_t)action].keys.clear();
        save_config();
    }
    ImGui::PopID();
}

static void autowalk_binding_control() {
    ImGui::PushID("autowalk_toggle_binding");
    const std::string label = hotkeys::describe(g_autowalkOpts.toggle);
    if (ImGui::Button((label + "##binding").c_str(), ImVec2(170, 0))) {
        g_captureTarget = CaptureTarget::AutowalkToggle;
        g_captureAction = -1;
        g_keyCapture.reset();
        g_captureMessage.clear();
        g_openCapture = true;
        ImGui::GetIO().ClearInputKeys();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to record: %s", label.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        g_autowalkOpts.toggle.keys.clear();
        g_autowalkWarning.clear();
        save_config();
    }
    ImGui::PopID();
}

static void render_capture_popup() {
    if (g_openCapture) {
        ImGui::OpenPopup("Record shortcut");
        g_openCapture = false;
    }
    if (!ImGui::BeginPopupModal("Record shortcut", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const bool capturingFov = g_captureTarget == CaptureTarget::FovAction &&
        g_captureAction >= 0 && g_captureAction < hotkeys::kActionCount;
    ImGui::Text("Binding: %s", capturingFov ? fov_action_name(g_captureAction) : "Autowalk toggle");
    ImGui::TextUnformatted("Press one key, or hold a combination, then release all keys.");
    ImGui::TextUnformatted("Up to 8 keyboard keys; click Cancel to keep the old binding.");
    const auto& candidate = g_keyCapture.candidate();
    ImGui::Text("Currently recorded: %s", hotkeys::describe(candidate).c_str());
    if (g_keyCapture.too_many()) ImGui::TextColored(ImVec4(1, .45f, .3f, 1), "Too many keys; release and try again.");
    if (!g_captureMessage.empty()) ImGui::TextColored(ImVec4(1, .45f, .3f, 1), "%s", g_captureMessage.c_str());
    hotkeys::Binding captured;
    if (g_captureTarget != CaptureTarget::None && g_keyCapture.poll(GetForegroundWindow() == g_hwnd, hotkeys::sample(), captured)) {
        int conflict = -1;
        bool conflictsWithAutowalk = false;
        for (int i = 0; i < hotkeys::kActionCount; ++i) {
            if (capturingFov && i == g_captureAction) continue;
            const bool matches = capturingFov
                ? hotkeys::equal(captured, g_fovOpts.bindings[(size_t)i])
                : autowalk::conflicts(captured, g_fovOpts.bindings[(size_t)i]);
            if (matches && !g_fovOpts.bindings[(size_t)i].keys.empty()) { conflict = i; break; }
        }
        if (capturingFov && conflict < 0 && !g_autowalkOpts.toggle.keys.empty() &&
            autowalk::conflicts(captured, g_autowalkOpts.toggle)) conflictsWithAutowalk = true;
        if (conflict >= 0) {
            g_captureMessage = capturingFov
                ? std::string("Already assigned to ") + fov_action_name(conflict) + "; choose another."
                : std::string("Conflicts with ") + fov_action_name(conflict) + "; choose another.";
        } else if (conflictsWithAutowalk) {
            g_captureMessage = "Conflicts with the Autowalk toggle; choose another.";
        } else {
            if (capturingFov) {
                g_fovOpts.bindings[(size_t)g_captureAction] = captured;
            } else {
                g_autowalkOpts.toggle = captured;
                g_autowalkWarning.clear();
            }
            save_config();
            g_captureTarget = CaptureTarget::None;
            g_captureAction = -1;
            ImGui::GetIO().ClearInputKeys();
            ImGui::CloseCurrentPopup();
        }
    }
    if (ImGui::Button("Cancel")) {
        g_captureTarget = CaptureTarget::None;
        g_captureAction = -1;
        g_keyCapture.reset();
        ImGui::GetIO().ClearInputKeys();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void render_fov_tab() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, TOP_SECTION_GAP));
    const ImVec4 green(0.20f, 1.0f, 0.35f, 1.0f);
    ImGui::TextWrapped(
        "Experimental runtime FOV control (10-180 degrees). Keep this manager open. Changes the baseline; zoom "
        "code is unchanged but not yet verified in gameplay.");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .65f, .3f, 1));
    ImGui::TextWrapped("From 120 degrees, reduced depth precision may cause z-fighting. At 180 degrees, projection becomes singular and rendering may break.");
    ImGui::PopStyleColor();

    if (ImGui::Checkbox("Enable Dynamic FOV", &g_fovOpts.enabledForNextLaunch)) save_config();
    ImGui::SameLine();
    style::help_marker("Applies to the next game launch only. Changing this does not affect a running game's controller or rendering hook.");
    ImGui::TextUnformatted("Applies to the next launch only; running games are unchanged.");
    if (!g_app.fovWarning.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .65f, .3f, 1));
        ImGui::TextWrapped("Dynamic FOV: %s", g_app.fovWarning.c_str());
        ImGui::PopStyleColor();
    }

    char current[32] = "n/a";
    char initial[32] = "n/a";
    if (g_fov.has_value()) {
        std::snprintf(current, sizeof(current), "%.2f deg", g_fov.value());
        std::snprintf(initial, sizeof(initial), "%.2f deg", g_fov.initial_value());
    }
    ImGui::TextWrapped("Status: %s", g_fov.status().c_str());
    ImGui::Text("Current: %s    Initial at attach: %s", current, initial);
    if (g_fov.attached()) {
        ImGui::TextColored(green, "Attached to game PID %u | %s | %s", g_fov.pid(),
            g_fov.ready() ? "ready" : "not ready",
            g_fov.foreground() ? "foreground" : "background");
    } else {
        ImGui::TextDisabled("Not attached to a game process.");
    }

    // Edit separate buffers: partial typed values must not be clamped or used
    // by live preset hotkeys before the user commits/leaves the field.
    static bool editInitialized = false;
    static float edits[5]{};
    if (!editInitialized) {
        edits[0] = g_fovOpts.desired; edits[1] = g_fovOpts.step;
        for (int i = 0; i < 3; ++i) edits[2 + i] = g_fovOpts.presets[(size_t)i];
        editInitialized = true;
    }
    auto edit_float = [&](const char* id, int index, float& destination, float low, float high, float fallback, const char* format) {
        ImGui::SetNextItemWidth(82);
        bool enter = ImGui::InputFloat(id, &edits[index], 0, 0, format, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            if (!std::isfinite(edits[index])) edits[index] = fallback;
            edits[index] = (std::max)(low, (std::min)(high, edits[index]));
            destination = edits[index];
            save_config();
        }
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    const bool narrow = ImGui::GetContentRegionAvail().x < 590.0f;
    ImGui::TextUnformatted("Target"); ImGui::SameLine();
    edit_float("##target", 0, g_fovOpts.desired, fov::kMin, fov::kMax, fov::kDefault, "%.1f");
    ImGui::SameLine(); ImGui::TextUnformatted("Step"); ImGui::SameLine();
    edit_float("##step", 1, g_fovOpts.step, 0.01f, 10.0f, 2.5f, "%.2f");
    if (!narrow) ImGui::SameLine();
    if (ImGui::Button("Apply target")) {
        if (g_fov.apply(g_fovOpts.desired)) g_app.status = "FOV target applied.";
        else g_app.status = "FOV apply failed: " + g_fov.status();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore observed at attach")) {
        if (g_fov.restore()) g_app.status = "FOV restored to value observed at attach.";
        else g_app.status = "FOV restore failed: " + g_fov.status();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Presets (degrees and optional shortcut)");
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::Text("Preset %d", i + 1); ImGui::SameLine();
        edit_float("##value", 2 + i, g_fovOpts.presets[(size_t)i], fov::kMin, fov::kMax, fov::kDefault, "%.1f");
        ImGui::SameLine();
        if (ImGui::Button("Apply")) {
            if (g_fov.apply(g_fovOpts.presets[(size_t)i])) g_app.status = "FOV preset applied.";
            else g_app.status = "FOV preset failed: " + g_fov.status();
        }
        ImGui::SameLine(); binding_control(fov::Preset1 + i);
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Other hotkeys");
    for (int action = fov::Decrease; action <= fov::Restore; ++action) {
        ImGui::TextUnformatted(fov_action_name(action)); ImGui::SameLine(90);
        binding_control(action);
    }
    bool suspend = !g_fovOpts.hotkeysEnabled;
    if (ImGui::Checkbox("Suspend FOV hotkeys", &suspend)) {
        g_fovOpts.hotkeysEnabled = !suspend;
        save_config();
    }

    ImGui::TextWrapped("Shortcuts only work while the game is foreground. They are non-consuming and may conflict with game or Windows keys.");
    ImGui::PopStyleVar(); // FramePadding
    ImGui::PopStyleVar(); // ItemSpacing
}

static void render_autowalk_tab() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, TOP_SECTION_GAP));
    ImGui::TextUnformatted("Autowalk");
    ImGui::Separator();
    if (ImGui::Checkbox("Enable Autowalk", &g_autowalkOpts.enabled)) save_config();
    ImGui::TextWrapped(
        "Press the assigned toggle in-game to start or stop walking. Autowalk stops and disarms "
        "when the game loses focus, is paused, or enters a menu/text input; press the toggle again to resume. "
        "Pressing forward or backward cancels it; strafing is allowed. Keep this launcher open while playing.");

    ImGui::TextUnformatted("Toggle hotkey");
    ImGui::SameLine(120);
    autowalk_binding_control();

    if (!g_autowalkWarning.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .65f, .3f, 1));
        ImGui::TextWrapped("Autowalk settings: %s", g_autowalkWarning.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::Text("Status: %s", g_autowalk.status().c_str());
    ImGui::Text("State: %s | %s", g_autowalk.ready() ? "ready" : "not ready",
        g_autowalk.walking() ? "walking" : "off");
    const std::string& forward = g_autowalk.forward_key();
    const std::string& backward = g_autowalk.backward_key();
    ImGui::Text("Forward: %s", forward.empty() ? "unavailable" : forward.c_str());
    ImGui::Text("Backward: %s", backward.empty() ? "unavailable" : backward.c_str());
    ImGui::PopStyleVar();
}

static void render_ui() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
    ImGui::Begin("##Main", nullptr, flags);

    if (style::begin_padded_tab_bar("main_tabs")) {
        if (style::begin_padded_tab_item("Patches")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, TOP_SECTION_GAP));

            std::vector<std::string> selected;
            for (size_t k = 0; k < g_app.definitions.size(); ++k) {
                if (g_app.enabled[k]) selected.push_back(g_app.definitions[k].str("id"));
            }
            // Snap every info line to the same 18px grid so the Copy button's
            // extra height cannot make the first gap bigger than the rest.
            ImVec2 infoPos = ImGui::GetCursorPos();
            render_path_line("exe", "Modules Path: ",
                "Game executable launched by this tool. Click the value to edit.",
                g_app.exePath, true);
            infoPos.y += INFO_LINE_STEP;
            ImGui::SetCursorPos(infoPos);
            render_path_line("cwd", "Working Path: ",
                "Working directory passed to the game process. Click the value to edit.",
                g_app.workingDir, false);
            infoPos.y += INFO_LINE_STEP;
            ImGui::SetCursorPos(infoPos);
            render_info_line("Patches JSON: ", "patches.json",
                "Patch definitions are read from this file next to the launcher.");
            infoPos.y += INFO_LINE_STEP;
            ImGui::SetCursorPos(infoPos);
            render_info_line("Selection: ",
                std::to_string(selected.size()) + " of " + std::to_string(g_app.definitions.size()),
                "Only selected patches are applied at launch.");
            infoPos.y += INFO_LINE_STEP;
            ImGui::SetCursorPos(infoPos);

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
            if (ImGui::Button("Reload")) {
                g_app.log.clear();
                load_definitions();
                g_app.status = "Reloaded patches.json.";
            }
            ImGui::SameLine();
            if (ImGui::Button("Disable all")) {
                for (size_t k = 0; k < g_app.enabled.size(); ++k) g_app.enabled[k] = false;
                g_app.status = "All patches disabled.";
                save_config();
            }
            // Right-align the primary Launch button in the remaining space.
            std::string buttonLabel = "Launch (" + std::to_string(selected.size()) + ")";
            float launchW = ImGui::CalcTextSize(buttonLabel.c_str()).x +
                ImGui::GetStyle().FramePadding.x * 2.0f + 2.0f;
            float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(rightX - launchW);
            if (style::colored_button(buttonLabel.c_str(), ImVec4(0.30f, 0.12f, 0.12f, 1.0f),
                                      ImVec2(launchW, 0))) {
                if (g_autowalk.attached()) {
                    // The Autowalk controller owns the primary child handle
                    // regardless of whether its feature is enabled.
                    g_app.status = "Close the running game before launching again (a game session is attached).";
                } else {
                    g_app.log.clear();
                    g_app.launchFailed = false;
                    g_app.fovWarning.clear();
                    std::string fatal;
                    const bool wantFov = g_fovOpts.enabledForNextLaunch;
                    HANDLE childProcess = nullptr;
                    DWORD childPid = 0;
                    bool fovInstalled = false;
                    bool fovControllerAttached = false;
                    std::string fovWarning;
                    bool ok = patcher::launch_game(g_app.exePath, g_app.workingDir, selected,
                        g_app.definitions, g_app.log, fatal,
                        &childProcess, &childPid, wantFov, &fovInstalled, &fovWarning);
                    HANDLE fovProcess = nullptr;
                    if (ok && childProcess && wantFov && fovInstalled) {
                        if (!DuplicateHandle(GetCurrentProcess(), childProcess, GetCurrentProcess(),
                                             &fovProcess, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            if (!fovWarning.empty()) fovWarning += " ";
                            fovWarning += "FOV support was installed, but its controller handle could not be duplicated (Win32 " +
                                std::to_string(GetLastError()) + ").";
                        }
                    }
                    if (ok && childProcess) {
                        g_fov.detach();
                        g_autowalk.detach();
                        g_autowalk.attach(childProcess, childPid);
                        childProcess = nullptr; // primary handle ownership transferred
                        if (fovProcess) {
                            g_fov.attach(fovProcess, childPid);
                            fovProcess = nullptr; // FOV controller owns the duplicate
                            fovControllerAttached = true;
                        }
                    }
                    if (childProcess) CloseHandle(childProcess);
                    if (fovProcess) CloseHandle(fovProcess);
                    g_app.fovWarning = fovWarning;
                    if (ok && wantFov && !fovInstalled && g_app.fovWarning.empty())
                        g_app.fovWarning = "Support was not installed; the game launched without Dynamic FOV.";
                    if (ok && !g_autowalk.attached()) {
                        if (!g_app.fovWarning.empty()) g_app.fovWarning += " ";
                        g_app.fovWarning += "The game launched, but its process handle could not be retained.";
                    }
                    save_config();
                    if (ok) {
                        g_app.status = "Launched with " + std::to_string(selected.size()) + " patch(es)." +
                            (fovControllerAttached ? " Dynamic FOV control attached." : "");
                    } else {
                        g_app.launchFailed = true;
                        g_app.status = "Launch failed: " + fatal;
                    }
                }
            }
            ImGui::PopStyleVar(); // FramePadding

            static char search[128] = "";
            ImGui::InputTextWithHint("##search", "Type here to search...", search, sizeof(search));

            const ImVec4 green(0.20f, 1.0f, 0.35f, 1.0f);
            const ImVec4 red(1.0f, 0.25f, 0.25f, 1.0f);
            const ImVec4 off(0.55f, 0.50f, 0.50f, 1.0f);
            if (ImGui::BeginTable("patches", 2,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable)) {
                ImGui::TableSetupColumn("Patch", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();
                std::string needle = search;
                std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
                    return (char)std::tolower(c);
                });
                for (size_t k = 0; k < g_app.definitions.size(); ++k) {
                    const JsonValue& def = g_app.definitions[k];
                    std::string name = def.str("name", def.str("id"));
                    std::string lower = name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                        return (char)std::tolower(c);
                    });
                    if (!needle.empty() && lower.find(needle) == std::string::npos) continue;

                    ImGui::PushID((int)k);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    bool selected = g_app.enabled[k];
                    std::string rowLabel = name + (selected ? "  (selected)" : "");
                    const std::string description = def.str("description");
                    if (!description.empty()) rowLabel += "  (?)";
                    ImVec2 cellMin = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##patch_row", ImVec2(-1, PATCH_ROW_HEIGHT))) {
                        g_app.enabled[k] = !g_app.enabled[k];
                        save_config();
                    }
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    if (ImGui::IsItemHovered()) {
                        dl->AddRectFilled(cellMin,
                            ImVec2(cellMin.x + ImGui::GetColumnWidth(), cellMin.y + PATCH_ROW_HEIGHT),
                            ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
                        if (!description.empty()) {
                            ImGui::BeginTooltip();
                            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                            ImGui::TextUnformatted(description.c_str());
                            ImGui::PopTextWrapPos();
                            ImGui::EndTooltip();
                        }
                    }
                    const float lineH = ImGui::GetTextLineHeight();
                    dl->AddText(ImVec2(cellMin.x + 2.0f,
                                       cellMin.y + (PATCH_ROW_HEIGHT - lineH) * 0.5f),
                                ImGui::GetColorU32(green), rowLabel.c_str());
                    ImGui::TableSetColumnIndex(1);
                    draw_status_cell((int)k, red, off);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            if (g_app.launchFailed) ImGui::TextColored(red, "%s", g_app.status.c_str());
            else ImGui::TextDisabled("%s", g_app.status.c_str());
            if (!g_app.fovWarning.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .65f, .3f, 1));
                ImGui::TextWrapped("Dynamic FOV: %s", g_app.fovWarning.c_str());
                ImGui::PopStyleColor();
            }

            if (ImGui::CollapsingHeader("Launch log")) {
                for (const LogLine& line : g_app.log) {
                    if (line.isError) ImGui::TextColored(red, "%s", line.text.c_str());
                    else ImGui::TextUnformatted(line.text.c_str());
                }
            }

            ImGui::PopStyleVar(); // ItemSpacing
            style::end_padded_tab_item();
        }
        if (style::begin_padded_tab_item("FOV")) {
            render_fov_tab();
            style::end_padded_tab_item();
        }
        if (style::begin_padded_tab_item("Others")) {
            render_autowalk_tab();
            style::end_padded_tab_item();
        }
        style::end_padded_tab_bar();
    }
    render_capture_popup();
    ImGui::End();
}

// A fullscreen D3D9 game can leave the launcher's device lost for the entire
// session. Keep the previous 50 ms device/message retry cadence while both
// runtime controllers continue polling at roughly 10 ms intervals.
static void wait_for_device() {
    const ULONGLONG until = GetTickCount64() + 50;
    do {
        Sleep(10);
        g_autowalk.tick(g_autowalkOpts);
        g_fov.tick(g_fovOpts);
    } while (GetTickCount64() < until);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR commandLine, int nCmdShow) {
    const int workerExitCode = autowalk::worker_command(commandLine);
    if (workerExitCode >= 0) return workerExitCode;

    // Be DPI-aware so the 800x600 layout stays true on scaled displays.
    if (HMODULE user32 = GetModuleHandleA("user32.dll")) {
        using Fn = BOOL (WINAPI *)(void*);
        bool done = false;
        if (auto setDpi = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            done = setDpi(reinterpret_cast<void*>(-4)) != FALSE; // PER_MONITOR_AWARE_V2
        }
        if (!done) {
            using LegacyFn = BOOL (WINAPI *)();
            if (auto setAware = reinterpret_cast<LegacyFn>(GetProcAddress(user32, "SetProcessDpiAware"))) {
                setAware();
            }
        }
    }

    WNDCLASS wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "OP2LauncherWindow";
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    POINT pos = center_on_primary(WINDOW_W, WINDOW_H);
    g_hwnd = CreateWindowEx(0, wc.lpszClassName, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        pos.x, pos.y, WINDOW_W, WINDOW_H, nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;

    int dark = 1;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    RECT minRect{0, 0, 540, 300};
    AdjustWindowRect(&minRect, WS_OVERLAPPEDWINDOW, FALSE);
    MINMAXINFO* dummy = nullptr; (void)dummy;

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);

    if (!create_device()) {
        MessageBoxA(g_hwnd, "Direct3D 9 device creation failed.", WINDOW_TITLE, MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Use ImGui's small bitmap-style default font.
    ImFontConfig defaultCfg{};
    defaultCfg.SizePixels = 13.0f;
    io.Fonts->AddFontDefault(&defaultCfg);
    style::apply();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(g_device);

    autodetect_paths();
    load_definitions();
    load_config();
    g_app.log.push_back({"Loaded " + std::to_string(g_app.definitions.size()) + " patch definition(s)."});

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        // Runtime controllers and process-exit detection run independently of
        // the active tab and D3D device. Autowalk updates the injected-key
        // filter before FOV samples the shared hotkey state.
        g_autowalk.tick(g_autowalkOpts);
        g_fov.tick(g_fovOpts);

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        render_ui();

        // A fullscreen game exit often changes the display mode and makes our
        // D3D9 device lost. Keep pumping messages and recover automatically so
        // the launcher never appears frozen when the game closes.
        HRESULT coop = g_device ? g_device->TestCooperativeLevel() : D3DERR_DEVICELOST;
        if (coop == D3DERR_DEVICELOST) {
            ImGui::EndFrame();
            wait_for_device();
            continue;
        }
        if (coop == D3DERR_DEVICENOTRESET) {
            ImGui_ImplDX9_InvalidateDeviceObjects();
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            g_pp.BackBufferWidth = (UINT)(rc.right - rc.left);
            g_pp.BackBufferHeight = (UINT)(rc.bottom - rc.top);
            HRESULT hr = g_device->Reset(&g_pp);
            if (FAILED(hr)) {
                ImGui::EndFrame();
                wait_for_device();
                continue;
            }
            ImGui_ImplDX9_CreateDeviceObjects();
        }

        ImGui::Render();
        if (SUCCEEDED(g_device->BeginScene())) {
            g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_RGBA(20, 18, 18, 255), 1.0f, 0);
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_device->EndScene();
        }
        HRESULT present = g_device->Present(nullptr, nullptr, nullptr, nullptr);
        if (present == D3DERR_DEVICELOST) wait_for_device();
        else Sleep(8);
    }

    // Stop Autowalk and release its session handle before tearing down UI or
    // graphics state. detach() is idempotent with WM_DESTROY.
    g_autowalk.detach();
    g_fov.detach();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(g_hwnd);
    UnregisterClass(wc.lpszClassName, hInstance);
    return 0;
}
