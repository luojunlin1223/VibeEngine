/*
 * BuildExporter — Packages the game as a standalone executable.
 *
 * Builds the VibeRuntime target, copies shaders, assets, and script
 * DLLs into a self-contained output directory.
 */
#pragma once

#include <string>
#include <functional>

namespace VE {

struct BuildSettings {
    std::string OutputDir = "Build";           // output folder name
    std::string StartupScene;                  // path to startup .vscene
    std::string GameName = "VibeGame";         // output exe name
    bool ReleaseMode = true;                   // Debug or Release
};

class BuildExporter {
public:
    // Run the full build pipeline (blocking)
    static bool Build(const BuildSettings& settings,
                      const std::string& projectRoot,
                      const std::string& cmakePath,
                      std::string& outLog);

    // Check if a build is in progress
    static bool IsBuilding() { return s_Building; }

    // Get build log
    static const std::string& GetBuildLog() { return s_Log; }

private:
    static bool s_Building;
    static std::string s_Log;
};

} // namespace VE
