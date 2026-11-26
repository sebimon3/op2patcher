#include "wide_fov_patch.h"
#include <cstring>
#include <limits>

namespace {
constexpr uint64_t kSkyRva = 0x146A90;
constexpr uint64_t kSkyRefRva = 0x80035;
constexpr uint64_t kActorEntryRva = 0x8270B0;
constexpr uint64_t kHookRva = 0x8288A7;
constexpr uint64_t kNormalRva = 0xC52C1C;
constexpr uint64_t kCloseRva = 0xC52BC0;
constexpr uint64_t kCameraFovRva = 0x14B8A0;
const unsigned char kSkyRef[] = {0x48,0x8d,0x15,0x60,0x6a,0x0c,0x00,0x41,0xb8,0x0c,0x00,0x00,0x00};
const unsigned char kActorEntry[] = {
    0xf3,0x0f,0x11,0x54,0x24,0x18, 0xf3,0x0f,0x11,0x4c,0x24,0x10,
    0x48,0x89,0x4c,0x24,0x08, 0x56,0x57,0x48,0x81,0xec,0x78,0x05,0x00,0x00
};
const unsigned char kHook[] = {0xf3,0x0f,0x10,0x05,0x6d,0xa3,0x42,0x00};
const unsigned char kHookFollowing[] = {0xf3,0x0f,0x11,0x84,0x24,0xc0,0x00,0x00,0x00};
const unsigned char kNormal[] = {0xcd,0xcc,0x4c,0x3e};
const unsigned char kClose[] = {0x0a,0xd7,0x23,0x3c};
const float kSky[12][6] = {
    {-1,-1,-1, -1,-1.01f,-1}, { 1,-1,-1,  1,-1.01f,-1},
    {-1,-1, 1, -1,-1.01f, 1}, { 1,-1, 1,  1,-1.01f, 1},
    {-1, 1,-1, -1, 1,-1},     { 1, 1,-1,  1, 1,-1},
    {-1, 1, 1, -1, 1, 1},     { 1, 1, 1,  1, 1, 1},
    {-1, 0,-1, -1,-1,-1},     { 1, 0,-1,  1,-1,-1},
    { 1, 0, 1,  1,-1, 1},     {-1, 0, 1, -1,-1, 1}
};
void scaled_sky(float (&changed)[12][6]) {
    std::memcpy(changed, kSky, sizeof(kSky));
    for (int i = 0; i < 12; ++i) for (int j = 0; j < 3; ++j) changed[i][j] *= 16.0f;
}

bool read_exact(HANDLE p, uint64_t addr, void* data, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(p, reinterpret_cast<LPCVOID>(addr), data, size, &got) && got == size;
}
bool match(HANDLE p, uint64_t addr, const void* expected, size_t size) {
    std::vector<unsigned char> current(size);
    return read_exact(p, addr, current.data(), size) && std::memcmp(current.data(), expected, size) == 0;
}
bool image_mapping(HANDLE p, uint64_t base, uint64_t address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQueryEx(p, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != sizeof(info)) return false;
    uint64_t start = reinterpret_cast<uint64_t>(info.BaseAddress);
    return info.AllocationBase == reinterpret_cast<void*>(base) && info.Type == MEM_IMAGE && info.State == MEM_COMMIT &&
        !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && address >= start &&
        size <= info.RegionSize && address - start <= info.RegionSize - size;
}
bool image_protection(HANDLE p, uint64_t address, DWORD expected) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQueryEx(p, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == sizeof(info) &&
           info.Protect == expected;
}
bool rel32(uint64_t fromNext, uint64_t target, unsigned char* out) {
    // User-mode canonical addresses are far below INT64_MAX on this host.
    if (fromNext > static_cast<uint64_t>(INT64_MAX) || target > static_cast<uint64_t>(INT64_MAX)) return false;
    const int64_t diff = static_cast<int64_t>(target) - static_cast<int64_t>(fromNext);
    if (diff < INT32_MIN || diff > INT32_MAX) return false;
    const uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(diff));
    for (int i = 0; i < 4; ++i) out[i] = static_cast<unsigned char>(v >> (8 * i));
    return true;
}
bool protected_write(HANDLE p, uint64_t address, const void* desired, const void* original,
                     size_t size, DWORD writable, bool code) {
    DWORD old = 0, ignored = 0;
    if (!VirtualProtectEx(p, reinterpret_cast<void*>(address), size, writable, &old)) return false;
    SIZE_T written = 0;
    bool ok = WriteProcessMemory(p, reinterpret_cast<void*>(address), desired, size, &written) &&
              written == size && match(p, address, desired, size) &&
              (!code || FlushInstructionCache(p, reinterpret_cast<LPCVOID>(address), size));
    if (!ok && original) {
        WriteProcessMemory(p, reinterpret_cast<void*>(address), original, size, &written);
        if (code) FlushInstructionCache(p, reinterpret_cast<LPCVOID>(address), size);
    }
    if (!VirtualProtectEx(p, reinterpret_cast<void*>(address), size, old, &ignored)) ok = false;
    return ok;
}
}

namespace widefov {
bool make_near_stub(uint64_t stub, uint64_t normal, uint64_t close, uint64_t continuation,
                    uint64_t cameraFov, std::vector<unsigned char>& bytes, std::string& error) {
    // Actor Update's third parameter is at [rsp+0x5a0]. Also test Device's
    // last rendered camera FOV, so near stays small while a wide camera
    // interpolates back down to a zoomed/narrow target. Preserve rax/flags.
    const unsigned char code[] = {
        0x9c,                                      // pushfq
        0x50,                                      // push rax
        0xf3,0x0f,0x10,0x84,0x24,0xb0,0x05,0x00,0x00, // movss xmm0,[rsp+5b0h]
        0x0f,0x2e,0x05,0x32,0x00,0x00,0x00,      // ucomiss xmm0,[threshold]
        0x73,0x21,                                // jae close
        0x48,0xb8,0,0,0,0,0,0,0,0,               // mov rax, device fov address
        0xf3,0x0f,0x10,0x00,                      // movss xmm0,[rax]
        0x0f,0x2e,0x05,0x1b,0x00,0x00,0x00,      // ucomiss xmm0,[threshold]
        0x73,0x0a,                                // jae close
        0xf3,0x0f,0x10,0x05,0,0,0,0,             // stock: movss xmm0,[0.2]
        0xeb,0x08,                                // jmp restore
        0xf3,0x0f,0x10,0x05,0,0,0,0,             // close: movss xmm0,[0.01]
        0x58,                                      // pop rax
        0x9d,                                      // popfq
        0xe9,0,0,0,0,                             // jmp continuation
        0x00,0x00,0xf0,0x42                      // 120.0f
    };
    bytes.assign(code, code + sizeof(code));
    for (int i = 0; i < 8; ++i) bytes[22 + i] = static_cast<unsigned char>(cameraFov >> (8 * i));
    if (!rel32(stub + 51, normal, &bytes[47]) ||
        !rel32(stub + 61, close, &bytes[57]) ||
        !rel32(stub + 68, continuation, &bytes[64])) {
        bytes.clear(); error = "Near-plane trampoline is outside rel32 range."; return false;
    }
    return true;
}

bool apply_sky(HANDLE process, uint64_t engineBase, std::string& error, bool* writeAttempted) {
    if (writeAttempted) *writeAttempted = false;
    const uint64_t address = engineBase + kSkyRva;
    if (!image_mapping(process, engineBase, address, sizeof(kSky)) ||
        !image_protection(process, address, PAGE_READWRITE) ||
        !match(process, engineBase + kSkyRefRva, kSkyRef, sizeof(kSkyRef)) ||
        !match(process, address, kSky, sizeof(kSky))) {
        error = "XR_3DA.exe sky reference or 288-byte vertex preimage mismatch."; return false;
    }
    float changed[12][6]; scaled_sky(changed);
    if (writeAttempted) *writeAttempted = true;
    if (!protected_write(process, address, changed, kSky, sizeof(changed), PAGE_READWRITE, false)) {
        error = "Could not write, verify, or restore protection on sky data."; return false;
    }
    return true;
}

bool verify_sky(HANDLE process, uint64_t engineBase) {
    float changed[12][6]; scaled_sky(changed);
    return image_mapping(process, engineBase, engineBase + kSkyRva, sizeof(changed)) &&
           image_protection(process, engineBase + kSkyRva, PAGE_READWRITE) &&
           match(process, engineBase + kSkyRefRva, kSkyRef, sizeof(kSkyRef)) &&
           match(process, engineBase + kSkyRva, changed, sizeof(changed));
}

bool verify_stock_sky(HANDLE process, uint64_t engineBase) {
    return image_mapping(process, engineBase, engineBase + kSkyRva, sizeof(kSky)) &&
           image_protection(process, engineBase + kSkyRva, PAGE_READWRITE) &&
           match(process, engineBase + kSkyRefRva, kSkyRef, sizeof(kSkyRef)) &&
           match(process, engineBase + kSkyRva, kSky, sizeof(kSky));
}

bool restore_sky(HANDLE process, uint64_t engineBase) {
    if (verify_stock_sky(process, engineBase)) return true;
    const uint64_t address = engineBase + kSkyRva;
    float changed[12][6]; scaled_sky(changed);
    if (!image_mapping(process, engineBase, address, sizeof(kSky)) ||
        !match(process, engineBase + kSkyRefRva, kSkyRef, sizeof(kSkyRef)) ||
        (!match(process, address, kSky, sizeof(kSky)) && !match(process, address, changed, sizeof(changed)))) return false;
    DWORD old = 0, ignored = 0; SIZE_T written = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(address), sizeof(kSky), PAGE_READWRITE, &old)) return false;
    bool ok = WriteProcessMemory(process, reinterpret_cast<void*>(address), kSky, sizeof(kSky), &written) &&
              written == sizeof(kSky);
    ok = VirtualProtectEx(process, reinterpret_cast<void*>(address), sizeof(kSky), PAGE_READWRITE, &ignored) && ok;
    return ok && verify_stock_sky(process, engineBase);
}

bool install_near(HANDLE process, uint64_t gameBase, uint64_t engineBase, std::string& error,
                  uint64_t* stubAddress, bool* hookWriteAttempted) {
    if (stubAddress) *stubAddress = 0;
    if (hookWriteAttempted) *hookWriteAttempted = false;
    const uint64_t hook = gameBase + kHookRva;
    if (!image_mapping(process, engineBase, engineBase + kCameraFovRva, sizeof(float)) ||
        !image_mapping(process, gameBase, hook, sizeof(kHook) + sizeof(kHookFollowing)) ||
        !image_protection(process, hook, PAGE_EXECUTE_READ) ||
        !match(process, gameBase + kActorEntryRva, kActorEntry, sizeof(kActorEntry)) ||
        !match(process, hook, kHook, sizeof(kHook)) ||
        !match(process, hook + sizeof(kHook), kHookFollowing, sizeof(kHookFollowing)) ||
        !match(process, gameBase + kNormalRva, kNormal, sizeof(kNormal)) ||
        !match(process, gameBase + kCloseRva, kClose, sizeof(kClose))) {
        error = "xrGame.dll camera prologue, near-plane hook, or constants mismatch."; return false;
    }
    void* alloc = nullptr;
    const uint64_t offsets[] = {0x04000000,0x08000000,0x10000000,0x20000000,
                                0x30000000,0x40000000,0x50000000,0x60000000,0x70000000};
    for (uint64_t off : offsets) {
        if (gameBase > UINT64_MAX - off) continue;
        alloc = VirtualAllocEx(process, reinterpret_cast<void*>(gameBase + off), 4096,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (alloc) break;
    }
    if (!alloc) { error = "Could not allocate a near-plane trampoline near xrGame.dll."; return false; }
    const uint64_t stub = reinterpret_cast<uint64_t>(alloc);
    std::vector<unsigned char> code;
    if (!make_near_stub(stub, gameBase + kNormalRva, gameBase + kCloseRva,
                        hook + sizeof(kHook), engineBase + kCameraFovRva, code, error)) {
        VirtualFreeEx(process, alloc, 0, MEM_RELEASE); return false;
    }
    SIZE_T written = 0;
    DWORD old = 0;
    if (!WriteProcessMemory(process, alloc, code.data(), code.size(), &written) || written != code.size() ||
        !match(process, stub, code.data(), code.size()) ||
        !VirtualProtectEx(process, alloc, 4096, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(process, alloc, code.size())) {
        VirtualFreeEx(process, alloc, 0, MEM_RELEASE);
        error = "Could not write, verify, or protect the near-plane trampoline."; return false;
    }
    unsigned char jump[sizeof(kHook)] = {0xe9,0,0,0,0,0x90,0x90,0x90};
    if (!rel32(hook + 5, stub, &jump[1])) {
        VirtualFreeEx(process, alloc, 0, MEM_RELEASE);
        error = "Near-plane hook is outside rel32 range."; return false;
    }
    // Caller guarantees every target thread is suspended through this write.
    if (hookWriteAttempted) *hookWriteAttempted = true;
    if (!protected_write(process, hook, jump, kHook, sizeof(jump), PAGE_EXECUTE_READWRITE, true)) {
        error = "Could not write, verify, or restore protection on the near-plane hook."; return false;
    }
    if (stubAddress) *stubAddress = stub;
    return true;
}

bool verify_stock_near(HANDLE process, uint64_t gameBase) {
    const uint64_t hook = gameBase + kHookRva;
    return image_mapping(process, gameBase, hook, sizeof(kHook) + sizeof(kHookFollowing)) &&
           image_protection(process, hook, PAGE_EXECUTE_READ) &&
           match(process, hook, kHook, sizeof(kHook)) &&
           match(process, hook + sizeof(kHook), kHookFollowing, sizeof(kHookFollowing));
}

bool restore_near(HANDLE process, uint64_t gameBase) {
    if (verify_stock_near(process, gameBase)) return true;
    const uint64_t hook = gameBase + kHookRva;
    if (!image_mapping(process, gameBase, hook, sizeof(kHook) + sizeof(kHookFollowing)) ||
        !match(process, hook + sizeof(kHook), kHookFollowing, sizeof(kHookFollowing))) return false;
    DWORD old = 0, ignored = 0; SIZE_T written = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(hook), sizeof(kHook), PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = WriteProcessMemory(process, reinterpret_cast<void*>(hook), kHook, sizeof(kHook), &written) &&
              written == sizeof(kHook) && FlushInstructionCache(process, reinterpret_cast<void*>(hook), sizeof(kHook));
    ok = VirtualProtectEx(process, reinterpret_cast<void*>(hook), sizeof(kHook), PAGE_EXECUTE_READ, &ignored) && ok;
    return ok && verify_stock_near(process, gameBase);
}

bool verify_near(HANDLE process, uint64_t gameBase, uint64_t engineBase) {
    const uint64_t hook = gameBase + kHookRva;
    unsigned char jump[sizeof(kHook)]{};
    if (!image_mapping(process, gameBase, hook, sizeof(kHook) + sizeof(kHookFollowing)) ||
        !image_protection(process, hook, PAGE_EXECUTE_READ) ||
        !read_exact(process, hook, jump, sizeof(jump)) || jump[0] != 0xe9 ||
        jump[5] != 0x90 || jump[6] != 0x90 || jump[7] != 0x90 ||
        !match(process, hook + sizeof(kHook), kHookFollowing, sizeof(kHookFollowing))) return false;
    int32_t displacement = 0;
    std::memcpy(&displacement, jump + 1, sizeof(displacement));
    const uint64_t stub = static_cast<uint64_t>(static_cast<int64_t>(hook + 5) + displacement);
    std::vector<unsigned char> expected; std::string error;
    return make_near_stub(stub, gameBase + kNormalRva, gameBase + kCloseRva,
                          hook + sizeof(kHook), engineBase + kCameraFovRva, expected, error) &&
           match(process, stub, expected.data(), expected.size());
}
}
