#include "fov_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <tlhelp32.h>
#include <wincrypt.h>

namespace {
constexpr unsigned long long kFovRva = 0xD16A40;
constexpr unsigned long long kGetterRva = 0x7F24DF;
constexpr unsigned long long kZoomRva = 0x7F24BF;
constexpr DWORD kImageSize = 0xE04000;
constexpr unsigned char kGetter[] = {0xF3,0x0F,0x10,0x05,0x59,0x45,0x52,0x00};
constexpr unsigned char kZoom[] = {0xF3,0x0F,0x10,0x80,0x5C,0x06,0x00,0x00};
const char* kDllHash = "9AE0AD19A48AEAB09E9F78B96DAFE72CBBE4543BDA2F2221C3F7A30DBFC5FE4D";
#ifndef FOV_TESTING
const char* kExeHash = "9F7F9424E6773D329D739E540876FDDA6313B41C5BFE75495CB5DAAAD4BFDE72";
#endif

bool read_exact(HANDLE process, unsigned long long address, void* data, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), data, size, &read) && read == size;
}

bool matches(HANDLE process, unsigned long long address, const unsigned char* expected, size_t size) {
    unsigned char buffer[16]{};
    return size <= sizeof(buffer) && read_exact(process, address, buffer, size)
        && std::memcmp(buffer, expected, size) == 0;
}

// Keep the read-only handle open: while the controller is active this denies
// replacing or writing the verified on-disk file. It never writes game files.
bool open_and_hash(const std::wstring& path, const char* expected, HANDLE& held, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot lock/read game file (Win32 " + std::to_string(GetLastError()) + ").";
        return false;
    }
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    unsigned char buffer[65536];
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        if (!CryptHashData(hash, buffer, got, 0)) { ok = false; break; }
    }
    unsigned char digest[32]{};
    DWORD length = sizeof(digest);
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (!ok) {
        error = "Could not hash game file (Win32 " + std::to_string(GetLastError()) + ").";
        CloseHandle(file);
        return false;
    }
    char actual[65]{};
    for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(actual + i * 2, 3, "%02X", digest[i]);
    if (std::strcmp(actual, expected) != 0) {
        error = "Unsupported game build: SHA-256 mismatch (found " + std::string(actual) + ").";
        CloseHandle(file);
        return false;
    }
    held = file;
    return true;
}

bool game_foreground(DWORD pid) {
    HWND window = GetForegroundWindow();
    DWORD owner = 0;
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return false;
    GetWindowThreadProcessId(window, &owner);
    return owner == pid;
}

struct Module {
    unsigned long long base = 0;
    DWORD size = 0;
    std::wstring path;
    int count = 0;
};

bool find_module(DWORD pid, Module& result) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false; // retried on the next tick
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snapshot, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"xrGame.dll") != 0) continue;
            ++result.count;
            result.base = reinterpret_cast<unsigned long long>(me.modBaseAddr);
            result.size = me.modBaseSize;
            result.path = me.szExePath;
        } while (Module32NextW(snapshot, &me));
    }
    CloseHandle(snapshot);
    return true;
}
}

namespace fov {
Controller::~Controller() { detach(); }

void Controller::attach(HANDLE process, DWORD pid) {
    detach();
    if (!process || process == INVALID_HANDLE_VALUE || !pid) {
        status_ = "Invalid game process handle.";
        return;
    }
    process_ = process;
    pid_ = pid;
    deadline_ = GetTickCount64() + 45000;
    nextCheck_ = 0;
    status_ = "Waiting for xrGame.dll; no FOV change has been made.";
}

void Controller::detach() {
    if (moduleFile_ != INVALID_HANDLE_VALUE) CloseHandle(moduleFile_);
    if (imageFile_ != INVALID_HANDLE_VALUE) CloseHandle(imageFile_);
    if (process_) CloseHandle(process_);
    process_ = nullptr;
    moduleFile_ = imageFile_ = INVALID_HANDLE_VALUE;
    pid_ = 0;
    base_ = 0;
    nextCheck_ = deadline_ = 0;
    modulePath_.clear();
    ready_ = disabled_ = foreground_ = hasValue_ = false;
    matcher_.reset();
    value_ = initial_ = 0;
    status_ = "Not attached. Enable Dynamic FOV, then launch the game.";
}

void Controller::fail(const std::string& message) {
    ready_ = false;
    disabled_ = true;
    foreground_ = false;
    matcher_.reset();
    if (moduleFile_ != INVALID_HANDLE_VALUE) CloseHandle(moduleFile_);
    if (imageFile_ != INVALID_HANDLE_VALUE) CloseHandle(imageFile_);
    moduleFile_ = imageFile_ = INVALID_HANDLE_VALUE;
    status_ = message;
}

bool Controller::discover() {
    Module module;
    if (!find_module(pid_, module) || !module.count) return false;
    if (module.count != 1 || module.size != kImageSize || module.path.empty()) {
        fail("Unsupported or ambiguous xrGame.dll image; FOV is disabled.");
        return false;
    }

    std::wstring imagePath(32768, L'\0');
    DWORD chars = static_cast<DWORD>(imagePath.size());
    if (!QueryFullProcessImageNameW(process_, 0, &imagePath[0], &chars)) {
        fail("Cannot identify the game executable (Win32 " + std::to_string(GetLastError()) + ").");
        return false;
    }
    imagePath.resize(chars);
    std::string error;
#ifndef FOV_TESTING
    if (!open_and_hash(imagePath, kExeHash, imageFile_, error)) {
        fail("EXE verification failed. " + error);
        return false;
    }
#endif
    if (!open_and_hash(module.path, kDllHash, moduleFile_, error)) {
        fail("xrGame.dll verification failed. " + error);
        return false;
    }
    base_ = module.base;
    modulePath_ = module.path;
    if (!validate_module(true)) return false;
    float current = 0;
    if (!read_value(current)) {
        fail("Cannot read the verified FOV value.");
        return false;
    }
    value_ = initial_ = current;
    hasValue_ = ready_ = true;
    status_ = "Verified; no change made. Apply a value or use the hotkeys.";
    return true;
}

bool Controller::validate_module(bool first) {
    if (!process_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
        fail("Game exited or the process handle became invalid.");
        return false;
    }
    Module module;
    if (!find_module(pid_, module) || module.count != 1 || module.base != base_ ||
        module.size != kImageSize || _wcsicmp(module.path.c_str(), modulePath_.c_str()) != 0) {
        fail("xrGame.dll changed, unloaded, or could not be verified; FOV disabled.");
        return false;
    }
    if (base_ > std::numeric_limits<unsigned long long>::max() - kImageSize) {
        fail("Invalid module address; FOV disabled.");
        return false;
    }
    if (first) {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!read_exact(process_, base_, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < 0 || static_cast<DWORD>(dos.e_lfanew) > 0x1000 ||
            !read_exact(process_, base_ + dos.e_lfanew, &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage != kImageSize) {
            fail("xrGame.dll has an unexpected in-memory PE image; FOV disabled.");
            return false;
        }
    }
    if (!matches(process_, base_ + kGetterRva, kGetter, sizeof(kGetter)) ||
        !matches(process_, base_ + kZoomRva, kZoom, sizeof(kZoom))) {
        fail("FOV or zoom code signature mismatch; FOV disabled.");
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    const auto address = base_ + kFovRva;
    if (!VirtualQueryEx(process_, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE ||
        reinterpret_cast<unsigned long long>(mbi.AllocationBase) != base_ ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        ((mbi.Protect & 0xff) != PAGE_READWRITE && (mbi.Protect & 0xff) != PAGE_WRITECOPY) ||
        address < reinterpret_cast<unsigned long long>(mbi.BaseAddress) ||
        address + sizeof(float) > reinterpret_cast<unsigned long long>(mbi.BaseAddress) + mbi.RegionSize) {
        fail("FOV memory is not the expected writable image data; FOV disabled.");
        return false;
    }
    return true;
}

bool Controller::read_value(float& out) {
    return base_ && read_exact(process_, base_ + kFovRva, &out, sizeof(out)) && std::isfinite(out);
}

bool Controller::write_value(float target) {
    if (!ready_ || !validate_module(false)) return false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process_, reinterpret_cast<LPVOID>(base_ + kFovRva),
                            &target, sizeof(target), &written) || written != sizeof(target)) {
        fail("FOV write failed or was incomplete; result is uncertain and FOV control is disabled.");
        return false;
    }
    float actual = 0;
    if (!read_value(actual) || std::memcmp(&actual, &target, sizeof(actual)) != 0) {
        fail("FOV readback did not match; result is uncertain and FOV control is disabled.");
        return false;
    }
    value_ = actual;
    hasValue_ = true;
    char message[100];
    std::snprintf(message, sizeof(message), "Applied %.1f degrees (read back).", static_cast<double>(actual));
    status_ = message;
    return true;
}

bool Controller::apply(float target) {
    if (!std::isfinite(target) || target < kMin || target > kMax) {
        status_ = "FOV must be a finite value from 10 to 180 degrees.";
        return false;
    }
    if (!ready_) {
        if (!disabled_) status_ = "FOV controller is not ready; no change was made.";
        return false;
    }
    return write_value(target);
}

bool Controller::adjust(float delta) {
    if (!ready_ || !std::isfinite(delta)) return false;
    float current = 0;
    if (!validate_module(false) || !read_value(current)) {
        if (!disabled_) fail("Cannot read the current FOV; control disabled.");
        return false;
    }
    return apply(std::max(kMin, std::min(kMax, current + delta)));
}

bool Controller::restore() {
    if (!ready_ || !hasValue_) return false;
    return apply(initial_);
}

void Controller::tick(const Settings& settings) {
    if (!process_) return;
    DWORD state = WaitForSingleObject(process_, 0);
    if (state != WAIT_TIMEOUT) {
        detach();
        status_ = state == WAIT_OBJECT_0 ? "Game exited; FOV control detached." : "Game process became inaccessible; FOV control detached.";
        return;
    }
    auto now = GetTickCount64();
    if (!ready_ && !disabled_ && now >= nextCheck_) {
        nextCheck_ = now + 250;
        discover();
        if (!ready_ && !disabled_ && now >= deadline_)
            fail("xrGame.dll did not load within 45 seconds; FOV disabled for this launch.");
    }
    if (ready_ && now >= nextCheck_) {
        nextCheck_ = now + 250;
        float current = 0;
        if (!read_value(current)) fail("Cannot read the current FOV; control disabled.");
        else { value_ = current; hasValue_ = true; }
    }
    foreground_ = ready_ && game_foreground(pid_);
    const bool active = foreground_ && settings.hotkeysEnabled;
    const hotkeys::State keyState = active ? hotkeys::sample() : hotkeys::State{};
    const int action = matcher_.poll(active, settings.bindings, keyState);
    if (action == Decrease || action == Increase) {
        if (std::isfinite(settings.step) && settings.step > 0 && settings.step <= 10)
            adjust(action == Decrease ? -settings.step : settings.step);
    } else if (action == Restore) {
        restore();
    } else if (action >= Preset1 && action <= Preset3) {
        apply(settings.presets[(size_t)(action - Preset1)]);
    }
}
}
