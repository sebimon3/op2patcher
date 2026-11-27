#pragma once

namespace autowalk {
// Ownership is separate from the user's physical key state. Never release a
// key taken over by a real forward press, and retain failed releases for retry.
class HeldKey {
public:
    template<class Emit> bool press(int scan, Emit emit) {
        if (owned_ || scan <= 0 || scan >= 256) return false;
        if (!emit(scan, true)) return false;
        scan_ = scan;
        owned_ = true;
        return true;
    }
    template<class Emit> bool release(bool physicalTakeover, Emit emit) {
        if (!owned_) return true;
        if (!physicalTakeover && !emit(scan_, false)) return false;
        owned_ = false;
        return true;
    }
    bool owned() const { return owned_; }
    int scan() const { return scan_; }
private:
    bool owned_ = false;
    int scan_ = 0;
};
} // namespace autowalk
