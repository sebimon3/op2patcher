#include "autowalk_state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <tlhelp32.h>
#include <wincrypt.h>

namespace {
bool locked_hash(const std::wstring& path, const char* expected, HANDLE& held) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    unsigned char bytes[65536];
    while (ok) {
        DWORD count = 0;
        if (!ReadFile(file, bytes, sizeof(bytes), &count, nullptr)) { ok = false; break; }
        if (!count) break;
        ok = CryptHashData(hash, bytes, count, 0) != FALSE;
    }
    unsigned char digest[32]{};
    DWORD count = sizeof(digest);
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &count, 0) && count == 32;
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    char hex[65]{};
    for (unsigned i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02X", digest[i]);
    if (!ok || std::strcmp(hex, expected)) { CloseHandle(file); return false; }
    held = file;
    return true;
}
}

namespace autowalk {
StateReader::~StateReader() { for (HANDLE file : files_) CloseHandle(file); }

bool StateReader::read(std::uint64_t address, void* data, size_t size) const {
    SIZE_T count = 0;
    return address >= 0x10000 && address < 0x0000800000000000ULL &&
        ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(address), data, size, &count) && count == size;
}
bool StateReader::pointer(std::uint64_t address, std::uint64_t& out) const {
    return read(address, &out, sizeof(out)) && out >= 0x10000 && out < 0x0000800000000000ULL;
}
bool StateReader::text(std::uint64_t address, std::string& out) const {
    out.clear();
    unsigned char bytes[48]{};
    if (!read(address, bytes, sizeof(bytes))) return false;
    for (unsigned char c : bytes) {
        if (!c) return !out.empty();
        if (c < 32 || c > 126) return false;
        out += static_cast<char>(c);
    }
    return false;
}

bool StateReader::discover() {
    if (GetTickCount64() < nextDiscovery_) return false;
    nextDiscovery_ = GetTickCount64() + 250;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    struct Image { std::uint64_t base = 0; std::wstring path; unsigned count = 0; } exe, game, core;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool first = true;
    if (Module32FirstW(snapshot, &me)) do {
        Image* image = nullptr;
        if (first) image = &exe;
        else if (!_wcsicmp(me.szModule, L"xrGame.dll")) image = &game;
        else if (!_wcsicmp(me.szModule, L"xrCore.dll")) image = &core;
        if (image) { image->base = reinterpret_cast<std::uint64_t>(me.modBaseAddr); image->path = me.szExePath; ++image->count; }
        first = false;
    } while (Module32NextW(snapshot, &me));
    CloseHandle(snapshot);
    if (!exe.count || !game.count || !core.count) return false;
    const char* hashes[] = {
        "9F7F9424E6773D329D739E540876FDDA6313B41C5BFE75495CB5DAAAD4BFDE72",
        "9AE0AD19A48AEAB09E9F78B96DAFE72CBBE4543BDA2F2221C3F7A30DBFC5FE4D",
        "F2F92E1771960B2BAD083A0DF338CF2D5E6C45A68C25045A3BFF2879F6D071CC"};
    Image* images[] = {&exe, &game, &core};
    for (unsigned i = 0; i < 3; ++i) {
        HANDLE file = INVALID_HANDLE_VALUE;
        if (images[i]->count != 1 || !locked_hash(images[i]->path, hashes[i], file)) {
            failed_ = true;
            error_ = "Unsupported/unreadable engine build; Autowalk is disabled.";
            return false;
        }
        files_.push_back(file);
    }
    exe_ = exe.base; game_ = game.base; core_ = core.base;
    for (int action = 0; action < 99; ++action) {
        std::uint64_t namePointer = 0;
        std::string name;
        if (!pointer(game_ + 0xD16400 + action * 16, namePointer) || !text(namePointer, name)) {
            failed_ = true; error_ = "Cannot verify engine action table."; return false;
        }
        if (name == "pause" || name == "quit" || name == "console" || name == "inventory" ||
            name == "active_jobs" || name == "map" || name == "contacts" || name == "chat" ||
            name == "chat_team" || name == "quick_load") contextActions_.push_back(action);
    }
    return true;
}

bool StateReader::bindings(int action, const char* name, std::vector<Key>& out) const {
    const std::uint64_t record = game_ + 0xD44A70 + action * 24;
    std::uint64_t row[3]{}, namePointer = 0;
    int id = -1;
    std::string actual;
    if (!read(record, row, sizeof(row)) || row[0] != game_ + 0xD16400 + action * 16 ||
        !pointer(row[0], namePointer) || !text(namePointer, actual) || actual != name ||
        !read(row[0] + 8, &id, sizeof(id)) || id != action) return false;
    for (unsigned i = 1; i <= 2; ++i) {
        if (!row[i]) continue;
        Key key;
        if (!pointer(row[i], namePointer) || !text(namePointer, key.name) ||
            !read(row[i] + 8, &key.scan, sizeof(key.scan))) return false;
        if (key.scan > 0 && key.scan < 256 && key.scan != 0xC5 && key.scan != 0xB7)
            out.push_back(key); // Mouse-only and special multi-part scan codes are not synthesized.
    }
    return true;
}

GameState StateReader::sample() {
    GameState state;
    state.reason = "Waiting for a loaded game.";
    if (failed_) { state.reason = error_; return state; }
    if (!game_ && !discover()) { if (failed_) state.reason = error_; return state; }
    if (!bindings(9, "forward", state.forward) || !bindings(10, "back", state.backward)) {
        state.reason = "Waiting for valid movement bindings."; return state;
    }
    if (state.forward.empty()) { state.reason = "Forward needs a supported keyboard binding."; return state; }
    // Names were verified once; bindings remain live and may change in-game.
    for (int action : contextActions_) {
        std::uint64_t namePointer = 0;
        std::string name;
        if (!pointer(game_ + 0xD16400 + action * 16, namePointer) || !text(namePointer, name)) continue;
        std::vector<Key> keys;
        if (!bindings(action, name.c_str(), keys)) { state.reason = "Cannot read context bindings."; return state; }
        for (const Key& key : keys) state.contextKeys.push_back(key.scan);
    }
    HWND window = GetForegroundWindow();
    DWORD owner = 0;
    if (window) GetWindowThreadProcessId(window, &owner);
    wchar_t windowClass[128]{};
    if (window) GetClassNameW(window, windowClass, 128);
    if (owner != pid_ || !IsWindowVisible(window) || IsIconic(window)) {
        state.reason = "Game is not focused."; return state;
    }
    if (std::wcscmp(windowClass, L"_XRAY_")) {
        state.reason = "The game renderer window is not focused."; return state;
    }
    int paused = -1, loaded = 0;
    unsigned char disabled = 1;
    if (!read(core_ + 0x82628, &paused, sizeof(paused)) || paused != 0) {
        state.reason = "Game is paused or pause state is unavailable."; return state;
    }
    if (!read(exe_ + 0x14AD44, &loaded, sizeof(loaded)) || loaded != 1 ||
        !read(game_ + 0xD43A0F, &disabled, 1) || disabled ||
        !pointer(exe_ + 0x14B3B0, state.level) || !pointer(game_ + 0xD453C0, state.actor)) return state;
    std::uint64_t entity = 0, input = 0, begin = 0, end = 0, receiver = 0, vtable = 0;
    if (!pointer(state.level + 0x30, entity) || entity != state.actor ||
        !pointer(exe_ + 0x14B388, input) || !read(input + 0x468, &begin, 8) ||
        !read(input + 0x470, &end, 8) || end <= begin || end - begin > 1024 ||
        !pointer(end - 8, receiver) || receiver != state.level + 0x10 ||
        !pointer(receiver, vtable) || vtable != game_ + 0xBD4F70) {
        state.reason = "Game input is captured or the scene is changing."; return state;
    }
    std::uint64_t hud = 0, ui = 0;
    if (!pointer(state.level + 0x1F8, hud) || !pointer(hud, vtable) ||
        vtable != game_ + 0xC51AD0 || !pointer(hud + 0x18, ui) ||
        !read(ui + 0x18, &begin, 8) || !read(ui + 0x20, &end, 8) || begin != end) {
        state.reason = "A game menu/dialog is open or its state is unavailable."; return state;
    }
    state.allowed = true;
    state.reason = "Ready; press the Autowalk hotkey.";
    return state;
}
} // namespace autowalk
