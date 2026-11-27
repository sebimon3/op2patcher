#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace autowalk {
struct Key {
    int scan = 0;
    std::string name;
};
struct GameState {
    bool allowed = false;
    std::uint64_t level = 0, actor = 0;
    std::vector<Key> forward, backward;
    std::vector<int> contextKeys;
    std::string reason;
};

// Read-only, build-gated access. No game code or memory is modified.
class StateReader {
public:
    StateReader(HANDLE process, DWORD pid) : process_(process), pid_(pid) {}
    ~StateReader();
    StateReader(const StateReader&) = delete;
    StateReader& operator=(const StateReader&) = delete;
    GameState sample();
    bool failed() const { return failed_; }
private:
    bool discover();
    bool read(std::uint64_t address, void* data, size_t size) const;
    bool pointer(std::uint64_t address, std::uint64_t& out) const;
    bool text(std::uint64_t address, std::string& out) const;
    bool bindings(int action, const char* name, std::vector<Key>& out) const;
    HANDLE process_;
    DWORD pid_;
    std::uint64_t exe_ = 0, game_ = 0, core_ = 0;
    std::vector<HANDLE> files_;
    std::vector<int> contextActions_;
    bool failed_ = false;
    ULONGLONG nextDiscovery_ = 0;
    std::string error_;
};
} // namespace autowalk
