#pragma once

#include <string>

namespace game_paths {

struct Paths {
    std::string gameExe;
    std::string workingDir;
};

// appDir must be the absolute directory containing the launcher.
Paths autodetect(const std::string& appDir);
void apply_saved(Paths& paths, const std::string& appDir,
                 const std::string& gameExe, const std::string& workingDir);

} // namespace game_paths
