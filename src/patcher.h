#pragma once
#include <string>
#include <vector>
#include <windows.h>
#include "mini_json.h"

struct LogLine {
    std::string text;
    bool isError = false;
};

namespace patcher {
    bool launch_game(
        const std::string& exePath,
        const std::string& workingDir,
        const std::vector<std::string>& enabledIds,
        const std::vector<JsonValue>& definitions,
        std::vector<LogLine>& log,
        std::string& fatalError,
        HANDLE* childProcess = nullptr,
        DWORD* childPid = nullptr,
        bool enableDynamicFov = false,
        bool* dynamicFovInstalled = nullptr,
        std::string* dynamicFovWarning = nullptr);
}
