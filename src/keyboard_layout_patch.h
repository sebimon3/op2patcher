#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace keyboardlayout {
struct Site {
    bool engine;
    uint64_t rva;
    std::vector<unsigned char> expected;
    std::vector<unsigned char> replacement;
};
// Public deterministic builders are also used by the image-only test oracle.
std::vector<unsigned char> make_veneer(uint64_t iatAddress);
bool make_sites(uint64_t engine, uint64_t game, uint64_t veneers,
                std::vector<Site>& sites, std::string& error);
// Caller guarantees all target threads are stopped (LOAD_DLL_DEBUG_EVENT).
// On a write failure the caller MUST terminate the child before resuming it.
bool install_engine(HANDLE process, uint64_t engine, std::string& error);
bool install_game(HANDLE process, uint64_t engine, uint64_t game,
                  std::string& error, uint64_t* veneers = nullptr);
}
