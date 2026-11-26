#include "startup_debugger.h"
#include <cwchar>
#include <exception>
#include <map>

namespace startup {
bool install_at_load(PROCESS_INFORMATION& child, const wchar_t* library,
                     const OnLibrary& install, std::string& error, DWORD timeoutMs) {
    bool success = false, ended = false, firstBreakpoint = true;
    // Keep the debug-event process/thread handles valid until detach. Windows'
    // debugger bookkeeping can reuse these handles in DebugActiveProcessStop.
    // Closing them at CREATE_PROCESS/CREATE_THREAD caused intermittent E_ACCESSDENIED.
    HANDLE debugProcess = nullptr;
    std::map<DWORD,HANDLE> debugThreads;
    if (ResumeThread(child.hThread) == DWORD(-1)) {
        error = "Cannot resume the debug-stopped child.";
    } else {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            DEBUG_EVENT event{};
            if (!WaitForDebugEvent(&event, 100)) {
                if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
                error = "Cannot receive startup debug event (Win32 " + std::to_string(GetLastError()) + ").";
                break;
            }
            DWORD disposition = DBG_CONTINUE;
            bool callbackFailed = false;
            switch (event.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                if (event.u.CreateProcessInfo.hProcess != child.hProcess) debugProcess = event.u.CreateProcessInfo.hProcess;
                if (event.u.CreateProcessInfo.hThread != child.hThread)
                    debugThreads[event.dwThreadId] = event.u.CreateProcessInfo.hThread;
                break;
            case CREATE_THREAD_DEBUG_EVENT:
                if (event.u.CreateThread.hThread) debugThreads[event.dwThreadId] = event.u.CreateThread.hThread;
                break;
            case EXIT_THREAD_DEBUG_EVENT:
                // Ownership is removed below only after Continue succeeds.
                break;
            case LOAD_DLL_DEBUG_EVENT: {
                HANDLE file = event.u.LoadDll.hFile;
                wchar_t path[32768]{};
                const DWORD n = file ? GetFinalPathNameByHandleW(file,path,32768,FILE_NAME_NORMALIZED) : 0;
                const wchar_t* name = std::wcsrchr(path,L'\\');
                name = name ? name+1 : path;
                if (n && n < 32768 && _wcsicmp(name,library)==0) {
                    try {
                        success = install(file,reinterpret_cast<uint64_t>(event.u.LoadDll.lpBaseOfDll),error);
                    } catch (const std::exception& e) {
                        error = "Startup installation exception: " + std::string(e.what());
                        success = false;
                    } catch (...) {
                        error = "Unknown startup installation exception.";
                        success = false;
                    }
                    callbackFailed = !success;
                    if (callbackFailed && error.empty()) error = "Startup installation callback rejected the target DLL.";
                    if (!success) TerminateProcess(child.hProcess,1); // still debug-stopped
                }
                if (file) CloseHandle(file);
                break;
            }
            case EXCEPTION_DEBUG_EVENT:
                if (firstBreakpoint && event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                    firstBreakpoint = false;
                } else {
                    disposition = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                error = "Child exited before keyboard-layout hooks were installed (code " +
                        std::to_string(event.u.ExitProcess.dwExitCode) + ").";
                break;
            }
            if (!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition)) {
                error = "Cannot continue the startup debug event."; success = false; break;
            }
            // Windows auto-closes exit-event handles only on successful
            // continuation. Keep ownership if continuation failed; never use
            // validity of a recycled numeric handle as evidence of ownership.
            if (event.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT)
                debugThreads.erase(event.dwThreadId);
            if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
                ended = true;
                debugProcess = nullptr;
                debugThreads.clear();
            }
            if (success || callbackFailed || ended) break;
        }
    }
    if (!success && error.empty()) error = "Timed out waiting for the target DLL load event.";
    if (!success) TerminateProcess(child.hProcess,1);
    if (!ended && !DebugActiveProcessStop(child.dwProcessId)) {
        if (success) error = "Cannot detach startup debugger (Win32 " + std::to_string(GetLastError()) + "); terminating the private child.";
        TerminateProcess(child.hProcess,1);
        success = false;
    }
    if (debugProcess) CloseHandle(debugProcess);
    for (const auto& thread : debugThreads) CloseHandle(thread.second);
    if (!success) WaitForSingleObject(child.hProcess,5000);
    return success;
}
}
