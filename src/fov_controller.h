#pragma once

#include <string>
#include <windows.h>
#include "fov_settings.h"

// Runtime access to the existing baseline-FOV float; no code hook or DLL injection.
// All methods are called from the launcher's UI thread. A Controller owns only
// the process handle handed to attach(); detach() never terminates that process.
namespace fov {
class Controller {
public:
    Controller() = default;
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // Takes ownership of the launcher's child handle; a null handle is invalid.
    void attach(HANDLE process, DWORD pid);
    void detach();

    // Call once per UI loop even when minimized. Hotkeys are edge-triggered
    // and require a foreground, visible, non-minimized game window.
    void tick(const Settings& settings);

    bool apply(float degrees);
    bool adjust(float delta);
    bool restore();
    bool attached() const { return process_ != nullptr; }
    bool ready() const { return ready_; }
    bool foreground() const { return foreground_; }
    bool has_value() const { return hasValue_; }
    float value() const { return value_; }
    float initial_value() const { return initial_; }
    DWORD pid() const { return pid_; }
    const std::string& status() const { return status_; }

private:
    bool discover();
    bool validate_module(bool first);
    bool read_value(float& out);
    bool write_value(float value);
    void fail(const std::string& message);

    HANDLE process_ = nullptr;
    HANDLE imageFile_ = INVALID_HANDLE_VALUE;
    HANDLE moduleFile_ = INVALID_HANDLE_VALUE;
    DWORD pid_ = 0;
    unsigned long long base_ = 0;
    unsigned long long nextCheck_ = 0;
    unsigned long long deadline_ = 0;
    std::wstring modulePath_;
    bool ready_ = false;
    bool disabled_ = false;
    bool foreground_ = false;
    hotkeys::Matcher matcher_;
    bool hasValue_ = false;
    float value_ = 0;
    float initial_ = 0;
    std::string status_ = "Not attached. Enable Dynamic FOV, then launch the game.";
};
}
