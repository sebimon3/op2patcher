#pragma once

#include <string>
#include <windows.h>
#include "hotkeys.h"
#include "mini_json.h"

namespace autowalk {
struct Settings {
    bool enabled = false;
    hotkeys::Binding toggle;
};

void load_settings(const JsonValue* value, Settings& settings);
std::string settings_json(const Settings& settings);
// Nested chords with identical modifiers are ambiguous across feature matchers.
bool conflicts(const hotkeys::Binding& a, const hotkeys::Binding& b);

class Controller {
public:
    Controller() = default;
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    // Owns the specific child handle, even when Autowalk is disabled. Never kills it.
    void attach(HANDLE process, DWORD pid);
    void detach();
    void tick(const Settings& settings);
    bool attached() const { return process_ != nullptr; }
    DWORD pid() const { return pid_; }
    bool ready() const { return ready_; }
    bool walking() const { return walking_; }
    const std::string& status() const { return status_; }
    const std::string& forward_key() const { return forward_; }
    const std::string& backward_key() const { return backward_; }

private:
    bool start_worker(const Settings& settings);
    void stop_worker();
    HANDLE process_ = nullptr;
    HANDLE worker_ = nullptr;
    HANDLE mapping_ = nullptr;
    HANDLE stop_ = nullptr;
    void* shared_ = nullptr;
    DWORD pid_ = 0;
    bool failed_ = false;
    bool ready_ = false;
    bool walking_ = false;
    Settings sent_;
    std::string status_ = "Launch the game to use Autowalk.";
    std::string forward_, backward_;
};

// Returns -1 for normal GUI startup, otherwise the headless watchdog exit code.
int worker_command(const char* commandLine);
} // namespace autowalk
