#include "game_paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace game_paths {
namespace fs = std::filesystem;

static bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

static bool directory_exists(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

Paths autodetect(const std::string& appDir) {
    if (appDir.empty()) return {};
    const fs::path dir = fs::path(appDir).lexically_normal();
    const fs::path nested = dir / "bin" / "XR_3DA.exe";
    if (is_file(nested)) return {nested.string(), dir.string()};

    const fs::path alongside = dir / "XR_3DA.exe";
    if (!is_file(alongside)) return {};
    std::string name = dir.filename().string();
    // A trailing separator gives an empty filename; inspect the directory itself.
    fs::path actualDir = dir;
    if (name.empty()) {
        actualDir = dir.parent_path();
        name = actualDir.filename().string();
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return {alongside.string(), (name == "bin" ? actualDir.parent_path() : dir).string()};
}

static fs::path resolve(const std::string& appDir, const std::string& saved) {
    if (saved.empty()) return {};
    fs::path path(saved);
    if (!path.is_absolute()) {
        // Drive-relative and root-relative Windows paths depend on process state.
        if (path.has_root_name() || path.has_root_directory() || appDir.empty()) return {};
        path = fs::path(appDir) / path;
    }
    return path.lexically_normal();
}

void apply_saved(Paths& paths, const std::string& appDir,
                 const std::string& gameExe, const std::string& workingDir) {
    const fs::path exe = resolve(appDir, gameExe);
    const fs::path cwd = resolve(appDir, workingDir);
    if (!exe.empty() && is_file(exe)) paths.gameExe = exe.string();
    if (!cwd.empty() && directory_exists(cwd)) paths.workingDir = cwd.string();
}

} // namespace game_paths
