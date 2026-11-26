#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace startup {
using OnLibrary = std::function<bool(HANDLE file, uint64_t base, std::string& error)>;
// Child must have been created DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED.
// The callback runs at its LOAD_DLL event, before the loader initializes that
// DLL. Every target thread is stopped by Windows. On success we detach; on
// any failure we terminate this child before releasing the debug stop.
bool install_at_load(PROCESS_INFORMATION& child, const wchar_t* library,
                     const OnLibrary& install, std::string& error, DWORD timeoutMs = 20000);
}
