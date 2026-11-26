#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

// Supported stock OP2.2 build only. Callers must independently verify both
// image hashes and must suspend the target before installing the code hook.
namespace widefov {
constexpr float kThreshold = 120.0f;
constexpr const char* kEngineHash = "9F7F9424E6773D329D739E540876FDDA6313B41C5BFE75495CB5DAAAD4BFDE72";
constexpr const char* kGameHash = "9AE0AD19A48AEAB09E9F78B96DAFE72CBBE4543BDA2F2221C3F7A30DBFC5FE4D";
bool make_near_stub(uint64_t stub, uint64_t normal, uint64_t close,
                    uint64_t continuation, uint64_t cameraFov, std::vector<unsigned char>& bytes,
                    std::string& error);
bool apply_sky(HANDLE process, uint64_t engineBase, std::string& error, bool* writeAttempted = nullptr);
bool verify_sky(HANDLE process, uint64_t engineBase);
bool verify_stock_sky(HANDLE process, uint64_t engineBase);
bool restore_sky(HANDLE process, uint64_t engineBase);
bool install_near(HANDLE process, uint64_t gameBase, uint64_t engineBase, std::string& error,
                  uint64_t* stubAddress = nullptr, bool* hookWriteAttempted = nullptr);
bool verify_near(HANDLE process, uint64_t gameBase, uint64_t engineBase);
bool verify_stock_near(HANDLE process, uint64_t gameBase);
bool restore_near(HANDLE process, uint64_t gameBase);
}
