#include "hotkeys.h"
#include <algorithm>
#include <cstdio>
#include <windows.h>

namespace hotkeys {
namespace {
constexpr int modifiers[] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN};
int injectedHoldKey = 0;
bool injectedHoldExternallyDown = false;
InjectedHoldReader injectedHoldReader = nullptr;
void* injectedHoldContext = nullptr;

int normalized_state_key(int key) {
    switch (key) {
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    default: return key;
    }
}

bool contains(const Binding& binding, int key) {
    return std::find(binding.keys.begin(), binding.keys.end(), key) != binding.keys.end();
}

bool matches(const Binding& binding, const State& state) {
    if (binding.keys.empty()) return false;
    for (int key : binding.keys) if (!state[(size_t)key]) return false;
    // Extra modifiers must not accidentally activate a less-specific chord.
    // Extra ordinary keys are allowed so the player can move while using it.
    for (int key : modifiers)
        if (state[(size_t)key] && !contains(binding, key)) return false;
    return true;
}

std::string key_name(int key) {
    switch (key) {
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_SHIFT: return "Shift";
    case VK_LWIN: return "Left Win";
    case VK_RWIN: return "Right Win";
    case VK_SPACE: return "Space";
    case VK_ESCAPE: return "Esc";
    default: break;
    }
    UINT scan = MapVirtualKeyW((UINT)key, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(scan << 16);
    switch (key) {
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        lp |= 1 << 24; break;
    default: break;
    }
    wchar_t wide[128]{};
    if (scan && GetKeyNameTextW(lp, wide, 128) > 0) {
        char utf8[512]{};
        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr) > 0)
            return utf8;
    }
    char fallback[16];
    std::snprintf(fallback, sizeof(fallback), "VK 0x%02X", key);
    return fallback;
}
}

bool bindable(int key) {
    // The left/right modifier aliases would duplicate the generic key states.
    return key >= 8 && key <= 254 &&
           key != VK_LSHIFT && key != VK_RSHIFT &&
           key != VK_LCONTROL && key != VK_RCONTROL &&
           key != VK_LMENU && key != VK_RMENU &&
           key != VK_PROCESSKEY && key != VK_PACKET;
}

bool normalize(Binding& binding) {
    if (binding.keys.size() > kMaxKeys) return false;
    for (int key : binding.keys) if (!bindable(key)) return false;
    std::sort(binding.keys.begin(), binding.keys.end());
    if (std::adjacent_find(binding.keys.begin(), binding.keys.end()) != binding.keys.end()) return false;
    return true;
}

bool equal(const Binding& a, const Binding& b) { return a.keys == b.keys; }

std::string describe(const Binding& binding) {
    if (binding.keys.empty()) return "Unbound";
    std::string text;
    auto append = [&](int key) {
        if (!text.empty()) text += " + ";
        text += key_name(key);
    };
    for (int key : modifiers) if (contains(binding, key)) append(key);
    for (int key : binding.keys)
        if (std::find(std::begin(modifiers), std::end(modifiers), key) == std::end(modifiers)) append(key);
    return text;
}

State sample() {
    State state{};
    for (int key = 8; key <= 254; ++key)
        if (bindable(key)) state[(size_t)key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    int holdKey = injectedHoldKey;
    bool externallyDown = injectedHoldExternallyDown;
    if (injectedHoldReader) injectedHoldReader(injectedHoldContext, holdKey, externallyDown);
    if (holdKey > 0 && holdKey < (int)state.size()) {
        int otherSide = 0;
        switch (holdKey) {
        case VK_LSHIFT: otherSide = VK_RSHIFT; break;
        case VK_RSHIFT: otherSide = VK_LSHIFT; break;
        case VK_LCONTROL: otherSide = VK_RCONTROL; break;
        case VK_RCONTROL: otherSide = VK_LCONTROL; break;
        case VK_LMENU: otherSide = VK_RMENU; break;
        case VK_RMENU: otherSide = VK_LMENU; break;
        default: break;
        }
        if (otherSide) externallyDown = externallyDown || (GetAsyncKeyState(otherSide) & 0x8000) != 0;
        state[(size_t)normalized_state_key(holdKey)] = externallyDown;
    }
    return state;
}

void set_injected_hold(int virtualKey, bool externallyDown) {
    if (virtualKey == 0) {
        injectedHoldKey = 0;
        injectedHoldExternallyDown = false;
        return;
    }
    injectedHoldKey = virtualKey > 0 && virtualKey < 256 ? virtualKey : 0;
    injectedHoldExternallyDown = injectedHoldKey != 0 && externallyDown;
}

void set_injected_hold_reader(InjectedHoldReader reader, void* context) {
    injectedHoldReader = reader;
    injectedHoldContext = context;
}

void Matcher::reset() { armed_ = false; previous_ = {}; }

int Matcher::poll(bool active, const Bindings& bindings, const State& state) {
    if (!active) { reset(); return -1; }
    if (!armed_) {
        bool released = true;
        for (const auto& binding : bindings)
            for (int key : binding.keys) if (state[(size_t)key]) released = false;
        previous_ = state;
        if (released) armed_ = true;
        return -1;
    }
    int chosen = -1;
    size_t specificity = 0;
    for (int i = 0; i < kActionCount; ++i) {
        const Binding& binding = bindings[(size_t)i];
        if (!matches(binding, state)) continue;
        bool downEdge = false;
        for (int key : binding.keys)
            if (state[(size_t)key] && !previous_[(size_t)key]) downEdge = true;
        if (downEdge && binding.keys.size() > specificity) {
            chosen = i;
            specificity = binding.keys.size();
        }
    }
    previous_ = state;
    return chosen;
}

void Capture::reset() { waitingRelease_ = true; tooMany_ = false; candidate_.keys.clear(); }

bool Capture::poll(bool foreground, const State& state, Binding& result) {
    if (!foreground) { reset(); return false; }
    Binding held;
    for (int key = 8; key <= 254; ++key)
        if (bindable(key) && state[(size_t)key]) held.keys.push_back(key);
    if (waitingRelease_) {
        if (held.keys.empty()) waitingRelease_ = false;
        return false;
    }
    if (held.keys.size() > kMaxKeys) {
        tooMany_ = true;
        candidate_.keys.clear();
        waitingRelease_ = true;
        return false;
    }
    if (held.keys.size() > candidate_.keys.size()) candidate_ = held;
    if (!held.keys.empty() || candidate_.keys.empty()) return false;
    result = candidate_;
    reset();
    return true;
}
}
