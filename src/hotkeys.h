#pragma once

#include <array>
#include <string>
#include <vector>

namespace hotkeys {
constexpr int kActionCount = 6;
constexpr int kMaxKeys = 8;
using State = std::array<bool, 256>;

// Logical Windows virtual keys, with Ctrl/Alt/Shift normalized to either side.
// An empty vector means unbound. Mouse buttons are intentionally excluded.
struct Binding {
    std::vector<int> keys;
};
using Bindings = std::array<Binding, kActionCount>;

bool bindable(int key);
bool normalize(Binding& binding);
bool equal(const Binding& a, const Binding& b);
std::string describe(const Binding& binding);
State sample();
// While a synthetic hold is owned, replace only its sampled key state with
// the corresponding physical state so it cannot retrigger launcher hotkeys.
// Passing virtualKey == 0 clears the override.
void set_injected_hold(int virtualKey, bool externallyDown);
// Optional live provider avoids a cross-process timing gap between the UI tick
// and sampling. Called only by the UI thread; clear before freeing its context.
using InjectedHoldReader = void (*)(void* context, int& virtualKey, bool& externallyDown);
void set_injected_hold_reader(InjectedHoldReader reader, void* context);

// Arming requires all configured keys to be released after focus is gained or
// hotkeys are enabled. A key-down edge, never auto-repeat, triggers an action.
// If multiple chords match at once, the most specific chord wins.
class Matcher {
public:
    int poll(bool active, const Bindings& bindings, const State& state);
    void reset();
private:
    bool armed_ = false;
    State previous_{};
};

// Captures the largest simultaneous set of held keyboard keys, committing only
// after all have been released. A focus loss restarts the release gate.
class Capture {
public:
    void reset();
    bool poll(bool foreground, const State& state, Binding& result);
    const Binding& candidate() const { return candidate_; }
    bool too_many() const { return tooMany_; }
private:
    bool waitingRelease_ = true;
    bool tooMany_ = false;
    Binding candidate_;
};
}
