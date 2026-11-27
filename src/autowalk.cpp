#include "autowalk.h"
#include "autowalk_hold.h"
#include "autowalk_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace {
constexpr DWORD kMagic = 0x4157324F;
constexpr ULONGLONG kHeartbeatTimeout = 750;
struct Config {
    LONG enabled;
    LONG count;
    int keys[hotkeys::kMaxKeys];
};
struct Report {
    LONG ready, walking, maskVk, externalDown, maskPending;
    char status[256], forward[64], backward[64];
};
struct Shared {
    DWORD magic, version, parentPid, gamePid;
    alignas(8) volatile LONG64 heartbeat;
    volatile LONG configSequence;
    Config config;
    volatile LONG reportSequence;
    Report report;
};
LONG atomic_read(volatile LONG* value) { return InterlockedCompareExchange(value, 0, 0); }
template<class T> bool snapshot(volatile LONG* sequence, const T* source, T& result) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        LONG before = atomic_read(sequence);
        if (before & 1) continue;
        std::memcpy(&result, source, sizeof(T));
        MemoryBarrier();
        if (before == atomic_read(sequence)) return true;
    }
    return false;
}
void write_config(Shared* shared, const autowalk::Settings& settings) {
    Config config{};
    config.enabled = settings.enabled;
    config.count = static_cast<LONG>((std::min)(settings.toggle.keys.size(), static_cast<size_t>(hotkeys::kMaxKeys)));
    std::copy_n(settings.toggle.keys.begin(), config.count, config.keys);
    InterlockedIncrement(&shared->configSequence);
    shared->config = config;
    InterlockedIncrement(&shared->configSequence);
}
int generic_key(int key) {
    if (key == VK_LCONTROL || key == VK_RCONTROL) return VK_CONTROL;
    if (key == VK_LMENU || key == VK_RMENU) return VK_MENU;
    if (key == VK_LSHIFT || key == VK_RSHIFT) return VK_SHIFT;
    return key;
}
int scan_vk(int scan, HKL layout) {
    return static_cast<int>(MapVirtualKeyExW((scan & 0x7F) | ((scan & 0x80) ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX, layout));
}
std::string key_names(const std::vector<autowalk::Key>& keys) {
    std::string result;
    for (const auto& key : keys) { if (!result.empty()) result += " / "; result += key.name; }
    return result.empty() ? "Unbound" : result;
}
bool contains(const std::vector<autowalk::Key>& keys, int scan) {
    return std::any_of(keys.begin(), keys.end(), [scan](const auto& key) { return key.scan == scan; });
}

class Worker;
Worker* g_worker = nullptr;
LRESULT CALLBACK keyboard_hook(int code, WPARAM message, LPARAM data);

class Worker {
public:
    Worker(HANDLE parent, HANDLE game, HANDLE stop, Shared* shared)
        : parent_(parent), game_(game), stop_(stop), shared_(shared), reader_(game, shared->gamePid),
          marker_(0x4F50324100000000ULL | GetCurrentProcessId()) {}

    int run() {
        g_worker = this;
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        bool quitting = false;
        int result = 0;
        for (;;) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) quitting = true;
                else { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if (WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT ||
                WaitForSingleObject(game_, 0) != WAIT_TIMEOUT ||
                WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT) quitting = true;
            if (quitting) {
                stop(result == 10 ? status_ : "Autowalk stopped.");
                publish(false);
                if (!held_.owned() && !maskVk_) break;
            } else {
                tick();
                if (reader_.failed()) { quitting = true; result = 10; }
            }
            retry_release();
            HANDLE waits[] = {parent_, game_, stop_};
            // Continue pumping the hook even during cleanup; no GUI thread owns it.
            MsgWaitForMultipleObjects(quitting ? 0 : 3, waits, FALSE, 10, QS_ALLINPUT);
        }
        if (hook_) UnhookWindowsHookEx(hook_);
        g_worker = nullptr;
        return result;
    }

    LRESULT event(int code, WPARAM message, const KBDLLHOOKSTRUCT* event) {
        if (code != HC_ACTION) return CallNextHookEx(hook_, code, message, reinterpret_cast<LPARAM>(event));
        bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
        if (!down && !up) return CallNextHookEx(hook_, code, message, reinterpret_cast<LPARAM>(event));
        if (event->dwExtraInfo == marker_) {
            if (down) { maskVk_ = filterVk_ = static_cast<int>(event->vkCode); publish(state_.allowed); }
        } else {
            const int scan = static_cast<int>(event->scanCode & 0x7F) | ((event->flags & LLKHF_EXTENDED) ? 0x80 : 0);
            if (scan > 0 && scan < 256) physical_[scan] = down;
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
            if (walking_ && foregroundPid == shared_->gamePid) {
                if (contains(state_.forward, scan) || contains(state_.backward, scan))
                    stop("Stopped by forward/back input.");
                else if (down && std::find(state_.contextKeys.begin(), state_.contextKeys.end(), scan) != state_.contextKeys.end())
                    stop("Stopped by a menu/pause/console key.");
            }
            if (scan == filterScan_ || down) publish(state_.allowed);
        }
        // Observe only: all user events retain their normal game/Windows behavior.
        return CallNextHookEx(hook_, code, message, reinterpret_cast<LPARAM>(event));
    }

private:
    bool emit(int scan, bool down) {
        if (down) {
            DWORD owner = 0;
            HWND window = GetForegroundWindow();
            wchar_t windowClass[128]{};
            GetWindowThreadProcessId(window, &owner);
            GetClassNameW(window, windowClass, 128);
            if (owner != shared_->gamePid || std::wcscmp(windowClass, L"_XRAY_") ||
                !IsWindowVisible(window) || IsIconic(window) ||
                WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT) return false;
        }
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = static_cast<WORD>(scan & 0x7F);
        input.ki.dwFlags = KEYEVENTF_SCANCODE | ((scan & 0x80) ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
        input.ki.dwExtraInfo = marker_;
        return SendInput(1, &input, sizeof(input)) == 1;
    }
    void stop(const std::string& reason) {
        const bool takeover = held_.owned() && physical_[held_.scan()];
        walking_ = false;
        held_.release(takeover, [this](int scan, bool down) { return emit(scan, down); });
        if (takeover) maskVk_ = 0;
        status_ = reason;
    }
    void retry_release() {
        if (!walking_ && held_.owned())
            held_.release(physical_[held_.scan()], [this](int scan, bool down) { return emit(scan, down); });
        if (!held_.owned() && maskVk_) {
            if (physical_[held_.scan()] || !(GetAsyncKeyState(maskVk_) & 0x8000)) maskVk_ = 0;
            else emit(held_.scan(), false);
        }
    }
    void publish(bool ready) {
        Report report{};
        report.ready = ready;
        report.walking = walking_;
        report.maskVk = filterVk_;
        report.externalDown = filterVk_ && physical_[filterScan_];
        report.maskPending = held_.owned() || maskVk_ != 0;
        std::snprintf(report.status, sizeof(report.status), "%s", status_.c_str());
        std::snprintf(report.forward, sizeof(report.forward), "%s", key_names(state_.forward).c_str());
        std::snprintf(report.backward, sizeof(report.backward), "%s", key_names(state_.backward).c_str());
        InterlockedIncrement(&shared_->reportSequence);
        shared_->report = report;
        InterlockedIncrement(&shared_->reportSequence);
    }
    void tick() {
        Config config{};
        if (!snapshot(&shared_->configSequence, &shared_->config, config) || config.count < 0 || config.count > hotkeys::kMaxKeys) {
            stop("Invalid controller settings."); matcher_.reset(); publish(false); return;
        }
        hotkeys::Binding binding;
        binding.keys.assign(config.keys, config.keys + config.count);
        if (!hotkeys::normalize(binding)) { stop("Invalid Autowalk hotkey."); matcher_.reset(); publish(false); return; }
        const ULONGLONG heartbeat = static_cast<ULONGLONG>(InterlockedCompareExchange64(&shared_->heartbeat, 0, 0));
        if (!config.enabled || binding.keys.empty() || GetTickCount64() - heartbeat > kHeartbeatTimeout) {
            stop("Disabled, unbound, or launcher heartbeat lost."); matcher_.reset(); publish(false); return;
        }
        if (!hotkeys::equal(binding, binding_)) { stop("Hotkey changed; Autowalk is off."); binding_ = binding; matcher_.reset(); }
        auto next = reader_.sample();
        if (!hook_ && !next.forward.empty()) {
            hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, GetModuleHandleW(nullptr), 0);
            if (!hook_) { stop("Cannot monitor manual input; Autowalk is disabled."); publish(false); return; }
            DWORD tid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            HKL layout = GetKeyboardLayout(tid);
            for (int scan = 1; scan < 256; ++scan) {
                int vk = scan_vk(scan, layout);
                physical_[scan] = vk && (GetAsyncKeyState(vk) & 0x8000);
            }
        }
        const bool changed = state_.level && (next.level != state_.level || next.actor != state_.actor ||
            key_names(next.forward) != key_names(state_.forward) || key_names(next.backward) != key_names(state_.backward));
        const bool regained = !state_.allowed && next.allowed;
        state_ = std::move(next);
        if (!state_.allowed || !hook_) { stop(state_.reason); matcher_.reset(); publish(false); return; }
        if (changed) { stop("Scene or movement bindings changed; Autowalk is off."); matcher_.reset(); }
        else if (regained && !walking_) status_ = state_.reason;
        DWORD tid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        HKL layout = GetKeyboardLayout(tid);
        if (walking_ && layout != layout_) stop("Keyboard layout changed; Autowalk is off.");
        if (!walking_ && !held_.owned() && !maskVk_ && layout != layout_) {
            filterVk_ = filterScan_ = 0;
            layout_ = layout;
        }
        bool conflict = false;
        for (const auto* keys : {&state_.forward, &state_.backward}) for (const auto& key : *keys) {
            int vk = generic_key(scan_vk(key.scan, layout));
            if (std::find(binding.keys.begin(), binding.keys.end(), vk) != binding.keys.end()) conflict = true;
        }
        if (conflict) { stop("Autowalk hotkey cannot include a forward/back key."); matcher_.reset(); publish(false); return; }
        for (int scan : state_.contextKeys)
            if (std::find(binding.keys.begin(), binding.keys.end(), generic_key(scan_vk(scan, layout))) != binding.keys.end()) conflict = true;
        if (conflict) { stop("Autowalk hotkey cannot include a menu/pause/console key."); matcher_.reset(); publish(false); return; }
        hotkeys::State input = hotkeys::sample();
        if (maskVk_) {
            input[maskVk_] = physical_[held_.scan()];
            int generic = generic_key(maskVk_);
            if (generic != maskVk_) {
                int other = maskVk_ == VK_LSHIFT ? VK_RSHIFT : maskVk_ == VK_RSHIFT ? VK_LSHIFT :
                            maskVk_ == VK_LCONTROL ? VK_RCONTROL : maskVk_ == VK_RCONTROL ? VK_LCONTROL :
                            maskVk_ == VK_LMENU ? VK_RMENU : VK_LMENU;
                input[generic] = input[maskVk_] || (GetAsyncKeyState(other) & 0x8000) != 0;
            }
        }
        hotkeys::Bindings bindings{};
        bindings[0] = binding;
        if (matcher_.poll(true, bindings, input) == 0) {
            if (walking_) stop("Stopped by Autowalk hotkey.");
            else if (!held_.owned() && !maskVk_) {
                bool moving = false;
                for (const auto* keys : {&state_.forward, &state_.backward}) for (const auto& key : *keys)
                    moving = moving || physical_[key.scan] || (GetAsyncKeyState(scan_vk(key.scan, layout)) & 0x8000);
                if (moving) status_ = "Release forward/back before enabling Autowalk.";
                else {
                    auto fresh = reader_.sample();
                    if (!fresh.allowed || fresh.level != state_.level || fresh.actor != state_.actor) {
                        stop(fresh.reason); matcher_.reset(); publish(false); return;
                    }
                    layout_ = layout;
                    int scan = state_.forward.front().scan;
                    maskVk_ = scan_vk(scan, layout);
                    filterVk_ = maskVk_;
                    filterScan_ = scan;
                    // Publish the exclusion before SendInput can make the global
                    // key state visible to the GUI thread's FOV hotkey sampler.
                    publish(true);
                    walking_ = maskVk_ && held_.press(scan, [this](int key, bool down) { return emit(key, down); });
                    if (!walking_) maskVk_ = 0;
                    status_ = walking_ ? "Autowalk active." : "Windows rejected the key press; Autowalk is off.";
                    if (walking_) {
                        for (const auto* keys : {&state_.forward, &state_.backward}) for (const auto& key : *keys)
                            if (physical_[key.scan]) stop("Stopped by forward/back input.");
                        for (int key : state_.contextKeys) if (physical_[key]) stop("Stopped by a context key.");
                    }
                }
            }
        }
        if (!walking_ && status_.empty()) status_ = state_.reason;
        publish(true);
    }
    HANDLE parent_, game_, stop_;
    Shared* shared_;
    autowalk::StateReader reader_;
    HHOOK hook_ = nullptr;
    HKL layout_ = nullptr;
    ULONG_PTR marker_;
    std::array<bool, 256> physical_{};
    autowalk::GameState state_;
    autowalk::HeldKey held_;
    hotkeys::Matcher matcher_;
    hotkeys::Binding binding_;
    bool walking_ = false;
    int maskVk_ = 0;
    int filterVk_ = 0, filterScan_ = 0;
    std::string status_;
};
LRESULT CALLBACK keyboard_hook(int code, WPARAM message, LPARAM data) {
    return g_worker ? g_worker->event(code, message, reinterpret_cast<const KBDLLHOOKSTRUCT*>(data)) : CallNextHookEx(nullptr, code, message, data);
}
}

namespace autowalk {
void load_settings(const JsonValue* value, Settings& settings) {
    settings = {};
    if (!value || value->type != JsonValue::Object) return;
    settings.enabled = value->boolean_value("enabled");
    const JsonValue* binding = value->find("toggleBinding");
    if (!binding || binding->type != JsonValue::Array || binding->array.size() > hotkeys::kMaxKeys) return;
    for (const auto& key : binding->array) {
        if (key.type != JsonValue::Number || !std::isfinite(key.number) || key.number != std::floor(key.number) || key.number < 1 || key.number > 255) {
            settings.toggle.keys.clear(); return;
        }
        settings.toggle.keys.push_back(static_cast<int>(key.number));
    }
    if (!hotkeys::normalize(settings.toggle)) settings.toggle.keys.clear();
}
std::string settings_json(const Settings& settings) {
    std::string out = "{\"enabled\": ";
    out += settings.enabled ? "true" : "false";
    out += ", \"toggleBinding\": [";
    for (size_t i = 0; i < settings.toggle.keys.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(settings.toggle.keys[i]);
    }
    return out + "]}";
}
bool conflicts(const hotkeys::Binding& a, const hotkeys::Binding& b) {
    if (a.keys.empty() || b.keys.empty()) return false;
    for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN}) {
        const bool left = std::find(a.keys.begin(), a.keys.end(), key) != a.keys.end();
        const bool right = std::find(b.keys.begin(), b.keys.end(), key) != b.keys.end();
        if (left != right) return false;
    }
    const auto subset = [](const auto& x, const auto& y) {
        return std::all_of(x.keys.begin(), x.keys.end(), [&y](int key) { return std::find(y.keys.begin(), y.keys.end(), key) != y.keys.end(); });
    };
    return subset(a, b) || subset(b, a);
}

Controller::~Controller() { detach(); }
void Controller::attach(HANDLE process, DWORD pid) {
    detach();
    if (!process || process == INVALID_HANDLE_VALUE || !pid || GetProcessId(process) != pid) {
        if (process && process != INVALID_HANDLE_VALUE) CloseHandle(process);
        status_ = "Invalid game process handle."; return;
    }
    process_ = process; pid_ = pid;
    status_ = "Autowalk is off.";
}
void Controller::stop_worker() {
    hotkeys::set_injected_hold_reader(nullptr, nullptr);
    hotkeys::set_injected_hold(0, false);
    if (stop_) SetEvent(stop_);
    if (worker_) WaitForSingleObject(worker_, 1500); // The independent helper keeps retrying any failed release.
    if (shared_) UnmapViewOfFile(shared_);
    if (mapping_) CloseHandle(mapping_);
    if (stop_) CloseHandle(stop_);
    if (worker_) CloseHandle(worker_);
    shared_ = nullptr; mapping_ = stop_ = worker_ = nullptr;
    ready_ = walking_ = false;
}
void Controller::detach() {
    stop_worker();
    if (process_) CloseHandle(process_);
    process_ = nullptr; pid_ = 0; failed_ = false;
    forward_.clear(); backward_.clear();
    status_ = "Launch the game to use Autowalk.";
}
bool Controller::start_worker(const Settings& settings) {
    HANDLE parent = nullptr, game = nullptr;
    const auto fail = [&]() {
        const DWORD error = GetLastError();
        if (parent) CloseHandle(parent);
        if (game) CloseHandle(game);
        stop_worker();
        failed_ = true;
        status_ = "Could not start the Autowalk watchdog (Win32 " + std::to_string(error) + ").";
        return false;
    };
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0) ||
        !DuplicateHandle(GetCurrentProcess(), process_, GetCurrentProcess(), &game, SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, TRUE, 0)) return fail();
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
    stop_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!mapping_ || !stop_) return fail();
    shared_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!shared_) return fail();
    auto* shared = static_cast<Shared*>(shared_);
    std::memset(shared, 0, sizeof(*shared));
    shared->magic = kMagic; shared->version = 1;
    shared->parentPid = GetCurrentProcessId(); shared->gamePid = pid_;
    InterlockedExchange64(&shared->heartbeat, static_cast<LONG64>(GetTickCount64()));
    write_config(shared, settings);
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return fail();
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --autowalk-worker " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(parent)) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(game)) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(mapping_)) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(stop_));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    auto* list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return fail();
    HANDLE handles[] = {parent, game, mapping_, stop_};
    bool ok = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (ok) ok = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
    DeleteProcThreadAttributeList(list);
    if (!ok) return fail();
    CloseHandle(process.hThread); worker_ = process.hProcess;
    CloseHandle(parent); CloseHandle(game);
    hotkeys::set_injected_hold_reader([](void* context, int& key, bool& down) {
        auto* shared = static_cast<Shared*>(context);
        Report report{};
        if (snapshot(&shared->reportSequence, &shared->report, report)) {
            key = report.maskVk;
            down = report.externalDown != 0;
            DWORD foreground = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
            if (!report.maskPending && foreground != shared->gamePid) key = 0;
        } else {
            // Conservatively suppress a potential synthetic event if a report
            // is being updated. An aligned LONG remains atomically readable.
            key = atomic_read(&shared->report.maskVk);
            down = false;
        }
    }, shared_);
    sent_ = settings;
    status_ = "Waiting for the Autowalk watchdog.";
    return true;
}
void Controller::tick(const Settings& settings) {
    if (!process_) return;
    if (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) { detach(); status_ = "Game exited; Autowalk stopped."; return; }
    if (!settings.enabled || settings.toggle.keys.empty()) {
        if (worker_) stop_worker();
        status_ = settings.enabled ? "Assign an Autowalk hotkey." : "Autowalk is disabled.";
        failed_ = false;
        return;
    }
    if (!worker_ && !failed_ && !start_worker(settings)) return;
    if (!worker_) return;
    auto* shared = static_cast<Shared*>(shared_);
    InterlockedExchange64(&shared->heartbeat, static_cast<LONG64>(GetTickCount64()));
    if (settings.enabled != sent_.enabled || !hotkeys::equal(settings.toggle, sent_.toggle)) { write_config(shared, settings); sent_ = settings; }
    Report report{};
    if (snapshot(&shared->reportSequence, &shared->report, report)) {
        ready_ = report.ready; walking_ = report.walking;
        report.status[sizeof(report.status) - 1] = 0;
        report.forward[sizeof(report.forward) - 1] = report.backward[sizeof(report.backward) - 1] = 0;
        if (report.status[0]) status_ = report.status;
        forward_ = report.forward; backward_ = report.backward;
        hotkeys::set_injected_hold(report.maskVk >= 0 && report.maskVk < 256 ? report.maskVk : 0, report.externalDown != 0);
    }
    if (WaitForSingleObject(worker_, 0) != WAIT_TIMEOUT) {
        DWORD code = 0; GetExitCodeProcess(worker_, &code);
        const std::string message = status_;
        stop_worker(); failed_ = true;
        status_ = code == 10 ? message : "Autowalk watchdog exited; disabled for this session.";
    }
}
int worker_command(const char* commandLine) {
    std::istringstream input(commandLine ? commandLine : "");
    std::string mode;
    input >> mode;
    if (mode != "--autowalk-worker") return -1;
    ULONG_PTR parentValue = 0, gameValue = 0, mappingValue = 0, stopValue = 0;
    std::string extra;
    if (!(input >> parentValue >> gameValue >> mappingValue >> stopValue) || (input >> extra)) return 2;
    HANDLE parent = reinterpret_cast<HANDLE>(parentValue), game = reinterpret_cast<HANDLE>(gameValue);
    HANDLE mapping = reinterpret_cast<HANDLE>(mappingValue), stop = reinterpret_cast<HANDLE>(stopValue);
    DWORD flags = 0;
    if (!GetHandleInformation(parent, &flags) || !GetHandleInformation(game, &flags) ||
        !GetHandleInformation(mapping, &flags) || !GetHandleInformation(stop, &flags)) return 2;
    auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) return 2;
    int result = 2;
    if (shared->magic == kMagic && shared->version == 1 && shared->parentPid == GetProcessId(parent) &&
        shared->gamePid == GetProcessId(game) && shared->parentPid != shared->gamePid && shared->parentPid != GetCurrentProcessId()) {
        Worker worker(parent, game, stop, shared);
        result = worker.run();
    }
    UnmapViewOfFile(shared);
    CloseHandle(parent); CloseHandle(game); CloseHandle(mapping); CloseHandle(stop);
    return result;
}
} // namespace autowalk
