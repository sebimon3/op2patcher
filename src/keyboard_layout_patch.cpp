#include "keyboard_layout_patch.h"
#include <cstring>
#include <limits>

namespace {
using Bytes = std::vector<unsigned char>;
bool read(HANDLE p, uint64_t a, void* data, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(p, reinterpret_cast<LPCVOID>(a), data, n, &got) && got == n;
}
bool image(HANDLE p, uint64_t base, uint64_t address, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQueryEx(p, reinterpret_cast<LPCVOID>(address), &m, sizeof(m)) != sizeof(m)) return false;
    const auto start = reinterpret_cast<uint64_t>(m.BaseAddress);
    return m.AllocationBase == reinterpret_cast<void*>(base) && m.Type == MEM_IMAGE &&
           m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           address >= start && n <= m.RegionSize && address - start <= m.RegionSize - n;
}
bool matches(HANDLE p, uint64_t address, const Bytes& wanted) {
    Bytes actual(wanted.size());
    return read(p, address, actual.data(), actual.size()) && actual == wanted;
}
bool rel32(uint64_t next, uint64_t target, unsigned char* dest) {
    if (next > INT64_MAX || target > INT64_MAX) return false;
    const int64_t d = static_cast<int64_t>(target) - static_cast<int64_t>(next);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    const auto v = static_cast<int32_t>(d);
    std::memcpy(dest, &v, 4);
    return true;
}
bool check_sites(HANDLE p, uint64_t engine, uint64_t game,
                 const std::vector<keyboardlayout::Site>& sites, bool engineSites, std::string& error) {
    for (const auto& site : sites) {
        if (site.engine != engineSites) continue;
        const uint64_t base = site.engine ? engine : game;
        if (!image(p, base, base + site.rva, site.expected.size()) ||
            !matches(p, base + site.rva, site.expected)) {
            error = "Keyboard layout: image mapping or instruction preimage mismatch at RVA " + std::to_string(site.rva);
            return false;
        }
    }
    return true;
}
bool write_sites(HANDLE p, uint64_t engine, uint64_t game,
                 const std::vector<keyboardlayout::Site>& sites, bool engineSites, std::string& error) {
    for (const auto& site : sites) {
        if (site.engine != engineSites) continue;
        const auto address = reinterpret_cast<void*>((site.engine ? engine : game) + site.rva);
        DWORD old = 0, ignored = 0;
        if (!VirtualProtectEx(p, address, site.replacement.size(), PAGE_EXECUTE_READWRITE, &old)) {
            error = "Keyboard layout: cannot make an instruction site writable."; return false;
        }
        SIZE_T done = 0;
        bool ok = WriteProcessMemory(p, address, site.replacement.data(), site.replacement.size(), &done) &&
                  done == site.replacement.size() &&
                  matches(p, reinterpret_cast<uint64_t>(address), site.replacement) &&
                  FlushInstructionCache(p, address, site.replacement.size());
        ok = VirtualProtectEx(p, address, site.replacement.size(), old, &ignored) && ok;
        if (!ok) { error = "Keyboard layout: instruction write/readback/protection restoration failed; child must terminate."; return false; }
    }
    return true;
}
}

namespace keyboardlayout {
std::vector<unsigned char> make_veneer(uint64_t iatAddress) {
    // Stack-neutral Win64 tail transfer: the extra Ex HKL argument is ignored
    // by the ordinary API. All five ToAscii arguments, including the stack
    // argument, stay exactly where the caller put them. RAX is volatile.
    Bytes code = {0x48,0xb8,0,0,0,0,0,0,0,0, 0xff,0x20};
    std::memcpy(code.data() + 2, &iatAddress, 8);
    return code;
}
bool make_sites(uint64_t engine, uint64_t game, uint64_t veneers,
                std::vector<Site>& sites, std::string& error) {
    sites = {
        {true, 0xe223d, {0xff,0x15,0xe5,0xb0,0,0}, Bytes(6,0x90)},
        {true, 0xe2893, {0x48,0xff,0x25,0x8e,0xaa,0,0}, {0xc3,0x90,0x90,0x90,0x90,0x90,0x90}},
        {false,0xb19432,{0xff,0x15,0xd0,0xe4,6,0},Bytes(6,0x90)},
        {false,0xb1a4c9,{0xff,0x15,0x39,0xd4,6,0},Bytes(6,0x90)},
        {false,0xb1b7f7,{0xff,0x15,0x0b,0xc1,6,0},Bytes(6,0x90)},
        {false,0xb1c2ba,{0xff,0x15,0x48,0xb6,6,0},Bytes(6,0x90)},
        // Disable the separate widget language selector, including writes of
        // input_language to ceb.settings. Its callers ignore the void result.
        {false,0xb19300,{0x48,0x89,0x4c,0x24,8},{0xc3,0x90,0x90,0x90,0x90}},
        // Preserve the normal registry/string construction and destruction;
        // bypass only label filtering. Every enumerated handle is retained
        // with the honest label "OS", not a stale EN/RU/UK preference.
        {false,0xb1a5ea,{0x0f,0xbe,0x44,0x24,0x20,0x83,0xf8,0x55,0x75,0x44,
                         0x0f,0xbe,0x44,0x24,0x21,0x83,0xf8,0x53,0x75,0x3a},
                        {0x66,0xc7,0x44,0x24,0x20,0x4f,0x53,0xe9,0x86,0,0,0,
                         0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}},
        {false,0xb19e04,{0xff,0x15,0xc6,0xda,6,0},{0xe8,0,0,0,0,0x90}},
        {false,0xb19e57,{0xff,0x15,0x6b,0xda,6,0},{0xe8,0,0,0,0,0x90}}
    };
    (void)engine;
    if (!rel32(game + 0xb19e04 + 5, veneers, sites[8].replacement.data()+1) ||
        !rel32(game + 0xb19e57 + 5, veneers + 16, sites[9].replacement.data()+1)) {
        error = "Keyboard layout: translation veneers are outside rel32 reach."; sites.clear(); return false;
    }
    return true;
}
bool install_engine(HANDLE process, uint64_t engine, std::string& error) {
    std::vector<Site> sites;
    if (!make_sites(engine, engine, engine, sites, error)) return false;
    return check_sites(process, engine, 0, sites, true, error) &&
           write_sites(process, engine, 0, sites, true, error);
}
bool install_game(HANDLE process, uint64_t engine, uint64_t game,
                  std::string& error, uint64_t* veneers) {
    if (veneers) *veneers = 0;
    std::vector<Site> sites;
    if (!make_sites(engine, game, game, sites, error) ||
        !check_sites(process, engine, game, sites, false, error)) return false;
    if (!image(process, engine, engine+0xed3b8, 16)) {
        error = "Keyboard layout: engine translation import slots are not mapped in the verified image."; return false;
    }
    void* allocation = nullptr;
    for (uint64_t offset : {0x04000000ULL,0x08000000ULL,0x10000000ULL,0x20000000ULL,
                            0x30000000ULL,0x40000000ULL,0x50000000ULL,0x60000000ULL,0x70000000ULL}) {
        if (game > UINT64_MAX-offset) continue;
        allocation = VirtualAllocEx(process, reinterpret_cast<void*>(game+offset),4096,
                                    MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if (allocation) break;
    }
    if (!allocation) { error = "Keyboard layout: cannot allocate translation veneers near xrGame.dll."; return false; }
    const auto address = reinterpret_cast<uint64_t>(allocation);
    Bytes code(32,0x90);
    auto map = make_veneer(engine+0xed3b8);
    auto ascii = make_veneer(engine+0xed3c0);
    std::memcpy(code.data(),map.data(),map.size());
    std::memcpy(code.data()+16,ascii.data(),ascii.size());
    SIZE_T done = 0; DWORD old = 0;
    if (!make_sites(engine,game,address,sites,error) ||
        !WriteProcessMemory(process,allocation,code.data(),code.size(),&done) || done != code.size() ||
        !matches(process,address,code) ||
        !VirtualProtectEx(process,allocation,4096,PAGE_EXECUTE_READ,&old) ||
        !FlushInstructionCache(process,allocation,code.size())) {
        VirtualFreeEx(process,allocation,0,MEM_RELEASE);
        error = "Keyboard layout: cannot construct/verify executable veneers."; return false;
    }
    if (!write_sites(process,engine,game,sites,false,error)) return false;
    if (veneers) *veneers = address;
    return true;
}
}
