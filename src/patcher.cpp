#include "patcher.h"
#include "wide_fov_patch.h"
#include "keyboard_layout_patch.h"
#include "startup_debugger.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <windows.h>
#include <tlhelp32.h>
#include <wincrypt.h>

namespace {
    void log_line(std::vector<LogLine>& log, const std::string& text, bool error = false) {
        log.push_back({text, error});
    }

    struct HeldFile {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~HeldFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        HeldFile() = default;
        HeldFile(const HeldFile&) = delete;
        HeldFile& operator=(const HeldFile&) = delete;
    };

    // Hold a read-only lock so the exact verified file cannot be replaced or
    // modified between hashing and installing the process-only patch.
    bool verify_file(const std::wstring& path, const char* expected, HeldFile& held, std::string& error) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "Cannot lock/read game image (Win32 " + std::to_string(GetLastError()) + ")."; return false;
        }
        HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
        bool ok = CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
                  CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
        unsigned char buffer[65536];
        while (ok) {
            DWORD got = 0;
            if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) { ok = false; break; }
            if (!got) break;
            if (!CryptHashData(hash, buffer, got, 0)) { ok = false; break; }
        }
        unsigned char digest[32]{}; DWORD length = sizeof(digest);
        if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest);
        if (hash) CryptDestroyHash(hash);
        if (provider) CryptReleaseContext(provider, 0);
        if (!ok) { error = "Could not compute game image SHA-256."; CloseHandle(file); return false; }
        char actual[65]{};
        for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(actual + i * 2, 3, "%02X", digest[i]);
        if (std::strcmp(actual, expected) != 0) {
            error = "Unsupported game image build (SHA-256 " + std::string(actual) + ").";
            CloseHandle(file); return false;
        }
        held.value = file;
        return true;
    }
    bool verify_file(const std::string& path, const char* expected, HeldFile& held, std::string& error) {
        const int count = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
        if (count <= 0) { error = "Cannot decode game executable path."; return false; }
        std::wstring wide(static_cast<size_t>(count), L'\0');
        if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path.c_str(), -1, &wide[0], count)) {
            error = "Cannot decode game executable path."; return false;
        }
        wide.pop_back();
        return verify_file(wide, expected, held, error);
    }

    std::vector<uint8_t> hex_bytes(const std::string& hex) {
        std::vector<uint8_t> out;
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
            if (hi < 0 || lo < 0) return {};
            out.push_back((uint8_t)((hi << 4) | lo));
        }
        return out;
    }

    uint64_t parse_rva(const JsonValue& v) {
        if (v.type == JsonValue::String) return std::strtoull(v.text.c_str(), nullptr, 0);
        if (v.type == JsonValue::Number) return (uint64_t)v.number;
        return 0;
    }

    // Toolhelp's SNAPMODULE fails with ERROR_PARTIAL_COPY while the initial
    // thread is suspended, so walk the child's PEB loader list instead.
    struct PROCESS_BASIC_INFORMATION {
        LONG ExitStatus;
        uint64_t PebBaseAddress;
        uint64_t AffinityMask;
        LONG BasePriority;
        uint64_t UniqueProcessId;
        uint64_t InheritedFromUniqueProcessId;
    };
    using NtQueryInformationProcessFn = LONG (NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    bool read_mem(HANDLE process, uint64_t address, void* buffer, size_t length);

    bool image_base(HANDLE process, uint64_t& base) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (!ntdll) return false;
        using Fn = LONG (NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        auto query = reinterpret_cast<Fn>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
        if (!query) return false;
        char pbi[48]{}; ULONG length = 0;
        if (query(process, 0, pbi, sizeof(pbi), &length) < 0) return false;
        uint64_t peb = 0;
        std::memcpy(&peb, pbi + 8, 8);
        if (!peb) return false;
        uint64_t result = 0;
        if (!read_mem(process, peb + 0x10, &result, 8) || !result) return false;
        base = result;
        return true;
    }

    // Used only after the process resumes: Toolhelp succeeds then.
    bool module_base_toolhelp(HANDLE process, DWORD pid, const std::string& wanted,
                              uint64_t& base, size_t& size, std::wstring* imagePath = nullptr) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return false;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        bool found = false;
        if (Module32FirstW(snap, &me)) {
            do {
                std::wstring name(me.szModule);
                std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) {
                    return static_cast<wchar_t>(std::towlower(c));
                });
                std::wstring target(wanted.begin(), wanted.end());
                std::transform(target.begin(), target.end(), target.begin(), [](wchar_t c) {
                    return static_cast<wchar_t>(std::towlower(c));
                });
                if (name == target) {
                    base = (uint64_t)me.modBaseAddr;
                    size = me.modBaseSize;
                    if (imagePath) *imagePath = me.szExePath;
                    found = true;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return found;
    }

    bool module_base(HANDLE process, DWORD pid, const std::string& wanted,
                     uint64_t& base, size_t& size) {
        (void)pid;
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (!ntdll) return false;
        auto query = reinterpret_cast<NtQueryInformationProcessFn>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
        if (!query) return false;

        PROCESS_BASIC_INFORMATION pbi{};
        ULONG length = 0;
        if (query(process, 0, &pbi, sizeof(pbi), &length) < 0) return false;
        if (!pbi.PebBaseAddress) return false;

        uint64_t ldr = 0;
        if (!read_mem(process, pbi.PebBaseAddress + 0x18, &ldr, sizeof(ldr)) || !ldr) return false;
        uint64_t head = ldr + 0x10;  // InLoadOrderModuleList.Flink
        uint64_t link = 0;
        if (!read_mem(process, head, &link, sizeof(link))) return false;

        std::string target = wanted;
        std::transform(target.begin(), target.end(), target.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });

        for (int guard = 0; guard < 512 && link && link != head; ++guard) {
            uint64_t entry = link;
            uint64_t dllBase = 0;
            uint32_t imageSize = 0;
            uint16_t nameLength = 0;
            uint64_t nameBuffer = 0;
            if (!read_mem(process, entry + 0x30, &dllBase, sizeof(dllBase))) break;
            if (!read_mem(process, entry + 0x40, &imageSize, sizeof(imageSize))) break;
            if (!read_mem(process, entry + 0x58, &nameLength, sizeof(nameLength))) break;
            if (!read_mem(process, entry + 0x60, &nameBuffer, sizeof(nameBuffer))) break;

            std::wstring wide;
            if (nameLength && nameLength < 512 && nameBuffer) {
                wide.resize(nameLength / 2);
                if (read_mem(process, nameBuffer, wide.data(), nameLength)) {
                    std::string narrow;
                    for (wchar_t wc : wide) {
                        if (wc == 0) break;
                        narrow.push_back((char)(std::tolower((unsigned char)(wc & 0xFF))));
                    }
                    if (narrow == target) {
                        base = dllBase;
                        size = imageSize;
                        return true;
                    }
                }
            }
            if (!read_mem(process, entry, &link, sizeof(link))) break;
        }
        return false;
    }

    bool read_mem(HANDLE process, uint64_t address, void* buffer, size_t length) {
        SIZE_T got = 0;
        return ReadProcessMemory(process, (LPCVOID)address, buffer, length, &got) && got == length;
    }

    bool write_mem(HANDLE process, uint64_t address, const void* buffer, size_t length) {
        DWORD old = 0;
        if (!VirtualProtectEx(process, (LPVOID)address, length, PAGE_READWRITE, &old)) return false;
        BOOL ok = WriteProcessMemory(process, (LPVOID)address, buffer, length, nullptr);
        DWORD ignored = 0;
        VirtualProtectEx(process, (LPVOID)address, length, old, &ignored);
        return ok != FALSE;
    }

    bool equals(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        return a == b;
    }

    bool apply_memory_patch(HANDLE process, const JsonValue& def, uint64_t xrGameBase,
                            std::vector<LogLine>& log) {
        const JsonValue* sites = def.find("patches");
        if (!sites || sites->type != JsonValue::Array) return false;
        for (const JsonValue& site : sites->array) {
            const JsonValue* rvaV = site.find("rva");
            if (!rvaV) return false;
            std::string dll = site.str("dllName", "xrGame.dll");
            uint64_t base = 0, size = 0;
            if (dll == "xrGame.dll") {
                base = xrGameBase;
            } else {
                // resolve other modules lazily if a definition ever needs them
                return false;
            }
            if (!base) return false;
            uint64_t address = base + parse_rva(*rvaV);
            std::vector<uint8_t> expected = hex_bytes(site.str("expected"));
            std::vector<uint8_t> enabled = hex_bytes(site.str("enabled"));
            if (expected.empty() || enabled.empty() || expected.size() != enabled.size()) {
                log_line(log, "  invalid byte definition at RVA " + site.str("rva"), true);
                return false;
            }
            std::vector<uint8_t> current(expected.size());
            if (!read_mem(process, address, current.data(), current.size())) {
                log_line(log, "  cannot read xrGame.dll at RVA " + site.str("rva"), true);
                return false;
            }
            if (!equals(current, expected)) {
                log_line(log, "  byte mismatch at xrGame.dll RVA " + site.str("rva") +
                    " (expected " + site.str("expected") + ", found " + "different bytes)", true);
                return false;
            }
            if (!write_mem(process, address, enabled.data(), enabled.size())) {
                log_line(log, "  cannot write xrGame.dll at RVA " + site.str("rva"), true);
                return false;
            }
            log_line(log, "  xrGame.dll RVA " + site.str("rva") + ": " +
                site.str("expected") + " -> " + site.str("enabled"));
        }
        return true;
    }

    bool apply_cursor_stub(HANDLE process, const JsonValue& def, uint64_t engineBase,
                           std::vector<LogLine>& log) {
        uint64_t hookRva = parse_rva(*def.find("hookRva"));
        uint64_t hookAddress = engineBase + hookRva;
        std::vector<uint8_t> expected = hex_bytes(def.str("expected"));
        if (expected.size() != 6) { log_line(log, "  invalid MouseUpdate prologue definition", true); return false; }
        std::vector<uint8_t> current(6);
        if (!read_mem(process, hookAddress, current.data(), 6) || !equals(current, expected)) {
            log_line(log, "  byte mismatch at XR_3DA.exe RVA " + def.str("hookRva"), true);
            return false;
        }

        std::vector<uint8_t> stub = hex_bytes(def.str("stub"));
        if (stub.size() < 200) { log_line(log, "  invalid stub template", true); return false; }

        const JsonValue* imms = def.find("immOffsets");
        const JsonValue* strings = def.find("stringOffsets");
        const JsonValue* iats = def.find("iatRvas");
        if (!imms || imms->type != JsonValue::Array || imms->array.size() != 8 ||
            !strings || strings->type != JsonValue::Array || strings->array.size() != 2 ||
            !iats || iats->type != JsonValue::Object) {
            log_line(log, "  invalid stub metadata", true);
            return false;
        }

        // E8 rel32 can only reach +/-2 GB. Allocate the stub near the game
        // image instead of letting the system pick an arbitrary far address.
        void* alloc = nullptr;
        const uint64_t offsets[] = {0x10000000, 0x20000000, 0x30000000, 0x40000000,
                                    0x50000000, 0x60000000, 0x70000000};
        for (uint64_t off : offsets) {
            alloc = VirtualAllocEx(process, (LPVOID)(engineBase + off), 512,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (alloc) break;
        }
        if (!alloc) { log_line(log, "  VirtualAllocEx near game image failed", true); return false; }
        uint64_t stubBase = (uint64_t)alloc;

        auto write_u64 = [&](size_t offset, uint64_t value) {
            for (int k = 0; k < 8; ++k) stub[offset + k] = (uint8_t)(value >> (k * 8));
        };

        uint64_t values[8] = {
            engineBase + parse_rva(*def.find("hwndRva")),
            engineBase + parse_rva(*iats->find("GetWindowRect")),
            engineBase + parse_rva(*def.find("activeRva")),
            stubBase + (uint64_t)parse_rva(strings->array[0]),
            engineBase + parse_rva(*iats->find("GetModuleHandleA")),
            stubBase + (uint64_t)parse_rva(strings->array[1]),
            engineBase + parse_rva(*iats->find("GetProcAddress")),
            hookAddress + 6,
        };
        for (size_t k = 0; k < 8; ++k) {
            uint64_t offset = parse_rva(imms->array[k]);
            if (offset + 8 > stub.size()) { log_line(log, "  stub imm offset out of range", true); return false; }
            write_u64((size_t)offset, values[k]);
        }

        // strings live at fixed offsets and are already in the template.
        if (!write_mem(process, stubBase, stub.data(), stub.size())) {
            log_line(log, "  cannot write cursor stub", true);
            return false;
        }

        int64_t relative = (int64_t)stubBase - (int64_t)(hookAddress + 5);
        if (relative < INT32_MIN || relative > INT32_MAX) {
            log_line(log, "  stub is outside rel32 reach", true);
            return false;
        }
        uint8_t hook[6] = {
            0xE8,
            (uint8_t)(relative), (uint8_t)(relative >> 8),
            (uint8_t)(relative >> 16), (uint8_t)(relative >> 24),
            0x90,
        };
        if (!write_mem(process, hookAddress, hook, sizeof(hook))) {
            log_line(log, "  cannot hook MouseUpdate", true);
            return false;
        }
        log_line(log, "  XR_3DA.exe RVA " + def.str("hookRva") + ": hooked -> stub at " +
            [&]() { char b[32]; std::snprintf(b, sizeof(b), "0x%llx", (unsigned long long)stubBase); return std::string(b); }());
        return true;
    }

    enum class FovInstall { Installed, Unavailable, Unsafe };

    FovInstall install_dynamic_fov(HANDLE process, DWORD pid, uint64_t engineBase,
                                  uint64_t expectedGameBase, std::string& error) {
        uint64_t gameBase = 0; size_t imageSize = 0; std::wstring path;
        if (!module_base_toolhelp(process, pid, "xrGame.dll", gameBase, imageSize, &path) ||
            gameBase != expectedGameBase || imageSize != 0xE04000 || path.empty()) {
            error = "Cannot independently identify the loaded xrGame.dll image."; return FovInstall::Unavailable;
        }
        HeldFile gameFile;
        if (!verify_file(path, widefov::kGameHash, gameFile, error)) {
            return FovInstall::Unavailable;
        }
        using Suspend = LONG (NTAPI*)(HANDLE);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        auto suspend = ntdll ? reinterpret_cast<Suspend>(GetProcAddress(ntdll, "NtSuspendProcess")) : nullptr;
        auto resume = ntdll ? reinterpret_cast<Suspend>(GetProcAddress(ntdll, "NtResumeProcess")) : nullptr;
        if (!suspend || !resume || suspend(process) < 0) {
            error = "Could not suspend the child for Dynamic FOV installation."; return FovInstall::Unavailable;
        }
        // Both changes are made while every game thread is suspended. On a
        // recoverable failure, restore bytes AND page protections before
        // resuming, so an unavailable optional feature does not kill the game.
        bool skyAttempted = false, hookAttempted = false;
        bool ok = widefov::apply_sky(process, engineBase, error, &skyAttempted);
        if (ok) ok = widefov::install_near(process, gameBase, engineBase, error, nullptr, &hookAttempted);
        if (ok && (!widefov::verify_sky(process, engineBase) || !widefov::verify_near(process, gameBase, engineBase))) {
            error = "Dynamic FOV post-installation verification failed."; ok = false;
        }
        if (!ok) {
            const bool nearRestored = !hookAttempted || widefov::restore_near(process, gameBase);
            const bool skyRestored = !skyAttempted || widefov::restore_sky(process, engineBase);
            if (!nearRestored || !skyRestored) {
                error += " Could not safely roll back the incomplete process-memory changes.";
                // Do not resume potentially corrupted code; the caller terminates this child.
                return FovInstall::Unsafe;
            }
        }
        if (resume(process) < 0) {
            error = "Could not resume the child after Dynamic FOV installation."; return FovInstall::Unsafe;
        }
        return ok ? FovInstall::Installed : FovInstall::Unavailable;
    }
}

namespace patcher {
    bool launch_game(const std::string& exePath, const std::string& workingDir,
                     const std::vector<std::string>& enabledIds,
                     const std::vector<JsonValue>& definitions,
                     std::vector<LogLine>& log, std::string& fatalError,
                     HANDLE* childProcess, DWORD* childPid, bool enableDynamicFov,
                     bool* dynamicFovInstalled, std::string* dynamicFovWarning) {
        if (childProcess) *childProcess = nullptr;
        if (childPid) *childPid = 0;
        if (dynamicFovInstalled) *dynamicFovInstalled = false;
        if (dynamicFovWarning) dynamicFovWarning->clear();
        if (exePath.empty()) { fatalError = "Game executable path is empty."; return false; }

        bool fovCandidate = enableDynamicFov;
        const bool keyboardCandidate = std::any_of(definitions.begin(), definitions.end(), [&](const JsonValue& def) {
            return def.str("type") == "keyboard_layout" &&
                   std::find(enabledIds.begin(), enabledIds.end(), def.str("id")) != enabledIds.end();
        });
        auto disableFov = [&](const std::string& why) {
            fovCandidate = false;
            if (dynamicFovWarning) *dynamicFovWarning = why;
            log_line(log, "Dynamic FOV unavailable: " + why, true);
        };
        HeldFile engineFile, keyboardGameFile; std::string verificationError;
        if (keyboardCandidate) {
            if (!verify_file(exePath, widefov::kEngineHash, engineFile, verificationError)) {
                fatalError = "Keyboard layout patch: " + verificationError; return false;
            }
            // Use the same Win32 ANSI path semantics as CreateProcessA and
            // verify_file(string), rather than mixing in filesystem UTF-8.
            DWORD length = GetFullPathNameA(exePath.c_str(), 0, nullptr, nullptr);
            std::vector<char> absolute(static_cast<size_t>(length) + 1);
            char* name = nullptr;
            const DWORD copied = length ? GetFullPathNameA(exePath.c_str(),
                static_cast<DWORD>(absolute.size()), absolute.data(), &name) : 0;
            if (!copied || copied >= absolute.size() || !name) {
                fatalError = "Keyboard layout patch: cannot resolve the sibling game DLL."; return false;
            }
            const std::string dll = std::string(absolute.data(), name) + "xrGame.dll";
            if (!verify_file(dll, widefov::kGameHash, keyboardGameFile, verificationError)) {
                fatalError = "Keyboard layout patch: " + verificationError; return false;
            }
        }
        if (fovCandidate && !keyboardCandidate && !verify_file(exePath, widefov::kEngineHash, engineFile, verificationError))
            disableFov(verificationError);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<char> cmdline(exePath.begin(), exePath.end());
        cmdline.push_back('\0');
        const char* cwd = workingDir.empty() ? nullptr : workingDir.c_str();
        if (!CreateProcessA(exePath.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | (keyboardCandidate ? DEBUG_ONLY_THIS_PROCESS : 0), nullptr, cwd, &si, &pi)) {
            fatalError = "CreateProcess failed (Win32 error " + std::to_string(GetLastError()) + ").";
            return false;
        }

        uint64_t gameBase = 0;
        if (!image_base(pi.hProcess, gameBase)) {
            log_line(log, "Cannot read the child's PEB image base", true);
            TerminateProcess(pi.hProcess, 1);
            if (keyboardCandidate) DebugActiveProcessStop(pi.dwProcessId);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            fatalError = "Could not read the suspended process image base.";
            return false;
        }
        log_line(log, "Game process created suspended (pid " + std::to_string(pi.dwProcessId) +
            ", base " + [&]() { char b[32]; std::snprintf(b, sizeof(b), "0x%llx", (unsigned long long)gameBase); return std::string(b); }() + ")");

        bool ok = true;
        std::vector<const JsonValue*> deferred;

        // The cursor hook must be installed before the game loop starts.
        for (const JsonValue& def : definitions) {
            std::string id = def.str("id");
            // Old patch lists may still contain these IDs. Only the explicit
            // next-launch FOV setting can enable the combined feature.
            if (id == "dynamic_fov" || id == "wide_fov_render") continue;
            if (std::find(enabledIds.begin(), enabledIds.end(), id) == enabledIds.end()) continue;
            std::string type = def.str("type");
            if (type == "code_stub") {
                log_line(log, "Applying: " + def.str("name", id));
                if (!apply_cursor_stub(pi.hProcess, def, gameBase, log)) { ok = false; break; }
            } else if (type == "memory") {
                deferred.push_back(&def);
            } else if (type == "keyboard_layout") {
                // Installed below at the loader's debug stop, before any
                // xrGame.dll initializer can create a text widget.
            } else {
                log_line(log, "Unknown patch type: " + type, true);
                ok = false;
                break;
            }
        }

        if (ok && keyboardCandidate) {
            log_line(log, "Keyboard layout: installing console guards before startup.");
            if (!keyboardlayout::install_engine(pi.hProcess, gameBase, fatalError)) ok = false;
        }

        if (!ok) {
            log_line(log, "Patch failed - terminating the game process.", true);
            TerminateProcess(pi.hProcess, 1);
            if (keyboardCandidate) DebugActiveProcessStop(pi.dwProcessId);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (fatalError.empty()) fatalError = "One or more patches failed verification. See the log for details.";
            return false;
        }

        if (keyboardCandidate) {
            log_line(log, "Keyboard layout: waiting for xrGame.dll's pre-initialization load event.");
            const auto install = [&](HANDLE file, uint64_t xrBase, std::string& error) {
                BY_HANDLE_FILE_INFORMATION expected{}, actual{};
                if (!GetFileInformationByHandle(keyboardGameFile.value, &expected) ||
                    !GetFileInformationByHandle(file, &actual) ||
                    expected.dwVolumeSerialNumber != actual.dwVolumeSerialNumber ||
                    expected.nFileIndexHigh != actual.nFileIndexHigh || expected.nFileIndexLow != actual.nFileIndexLow) {
                    error = "Keyboard layout: the loader selected a different game DLL than the verified, locked file.";
                    return false;
                }
                return keyboardlayout::install_game(pi.hProcess, gameBase, xrBase, error);
            };
            if (!startup::install_at_load(pi, L"xrGame.dll", install, fatalError)) {
                log_line(log, "Keyboard layout patch failed: " + fatalError, true);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return false;
            }
            log_line(log, "Keyboard layout: Windows controls text layout; OS label installed; startup debugger detached.");
        } else if (ResumeThread(pi.hThread) == (DWORD)-1) {
            DWORD error = GetLastError();
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            fatalError = "Could not resume the game thread (Win32 error " + std::to_string(error) + ").";
            return false;
        }

        // xrGame.dll is loaded dynamically during engine startup. Keyboard
        // hooks have already been installed at its LOAD_DLL debug stop; the
        // older pause/FOV features retain their existing deferred path.
        if (!deferred.empty() || fovCandidate) {
            uint64_t xrBase = 0, xrSize = 0;
            bool found = false;
            for (int attempt = 0; attempt < 100; ++attempt) {
                if (module_base_toolhelp(pi.hProcess, pi.dwProcessId, "xrGame.dll", xrBase, xrSize) ||
                    module_base(pi.hProcess, pi.dwProcessId, "xrGame.dll", xrBase, xrSize)) {
                    found = true;
                    break;
                }
                Sleep(50);
            }
            if (!found) {
                if (!deferred.empty()) {
                    log_line(log, "xrGame.dll did not load within 5 seconds", true);
                    ok = false;
                }
                if (fovCandidate) disableFov("xrGame.dll did not load within 5 seconds.");
            } else {
                for (const JsonValue* def : deferred) {
                    log_line(log, "Applying: " + def->str("name", def->str("id")));
                    if (!apply_memory_patch(pi.hProcess, *def, xrBase, log)) { ok = false; break; }
                }
                if (ok && fovCandidate) {
                    std::string error;
                    const FovInstall result = install_dynamic_fov(pi.hProcess, pi.dwProcessId, gameBase, xrBase, error);
                    if (result == FovInstall::Installed) {
                        if (dynamicFovInstalled) *dynamicFovInstalled = true;
                        log_line(log, "Dynamic FOV: verified both images; sky and conditional near-plane corrections installed.");
                    } else {
                        disableFov(error);
                        if (result == FovInstall::Unsafe) ok = false;
                    }
                }
            }
        }

        if (!ok) {
            log_line(log, "Patch failed - terminating the game process.", true);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            fatalError = "One or more patches failed verification. See the log for details.";
            return false;
        }

        CloseHandle(pi.hThread);
        if (childProcess) {
            *childProcess = pi.hProcess;
            if (childPid) *childPid = pi.dwProcessId;
        } else {
            CloseHandle(pi.hProcess);
        }
        return true;
    }
}
