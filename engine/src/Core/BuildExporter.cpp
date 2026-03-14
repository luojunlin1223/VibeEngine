/*
 * BuildExporter — Builds VibeRuntime and packages game for distribution.
 */
#include "VibeEngine/Core/BuildExporter.h"
#include "VibeEngine/Core/Log.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <array>

namespace fs = std::filesystem;

namespace VE {

bool BuildExporter::s_Building = false;
std::string BuildExporter::s_Log;

static std::string RunCommand(const std::string& cmd) {
    std::string result;
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return "Failed to run command";
    std::array<char, 256> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe))
        result += buffer.data();
    _pclose(pipe);
    return result;
}

bool BuildExporter::Build(const BuildSettings& settings,
                          const std::string& projectRoot,
                          const std::string& cmakePath,
                          std::string& outLog) {
    s_Building = true;
    s_Log.clear();
    outLog.clear();

    auto log = [&](const std::string& msg) {
        s_Log += msg + "\n";
        outLog += msg + "\n";
        VE_INFO("[Build] {}", msg);
    };

    std::string config = settings.ReleaseMode ? "Release" : "Debug";
    fs::path outputDir = fs::path(projectRoot) / settings.OutputDir;
    fs::path buildDir = fs::path(projectRoot) / "build";

    log("=== VibeEngine Build Export ===");
    log("Config: " + config);
    log("Output: " + outputDir.string());

    // ── Step 1: Build VibeRuntime ────────────────────────────────────
    log("Step 1: Building VibeRuntime (" + config + ")...");

    std::string buildCmd = "\"" + cmakePath + "\" --build \"" +
        buildDir.string() + "\" --config " + config + " --target VibeRuntime 2>&1";
    std::string buildOutput = RunCommand(buildCmd);
    s_Log += buildOutput;
    outLog += buildOutput;

    // Check if exe was built
    fs::path exePath = buildDir / "bin" / config / "VibeRuntime.exe";
    if (!fs::exists(exePath)) {
        log("ERROR: VibeRuntime.exe not found at " + exePath.string());
        log("Build failed.");
        s_Building = false;
        return false;
    }
    log("VibeRuntime built successfully");

    // ── Step 2: Create output directory ──────────────────────────────
    log("Step 2: Packaging to " + outputDir.string() + "...");

    fs::create_directories(outputDir);
    fs::create_directories(outputDir / "shaders");
    fs::create_directories(outputDir / "Assets");
    fs::create_directories(outputDir / "ProjectSettings");

    // ── Step 3: Copy executable ─────────────────────────────────────
    fs::path destExe = outputDir / (settings.GameName + ".exe");
    fs::copy_file(exePath, destExe, fs::copy_options::overwrite_existing);
    log("Copied: " + destExe.filename().string());

    // ── Step 4: Copy shaders ────────────────────────────────────────
    fs::path shaderSrc = buildDir / "bin" / config / "shaders";
    if (fs::exists(shaderSrc)) {
        for (auto& entry : fs::directory_iterator(shaderSrc)) {
            if (entry.path().extension() == ".shader") {
                fs::copy_file(entry.path(), outputDir / "shaders" / entry.path().filename(),
                    fs::copy_options::overwrite_existing);
            }
        }
        log("Copied shaders");
    } else {
        log("WARNING: No shaders directory found");
    }

    // ── Step 5: Copy assets ─────────────────────────────────────────
    // Copy from the editor's working Assets directory
    fs::path assetsSrc = fs::path(projectRoot) / "Assets";
    // Also check build directory
    if (!fs::exists(assetsSrc))
        assetsSrc = buildDir / "bin" / config / "Assets";

    if (fs::exists(assetsSrc)) {
        fs::copy(assetsSrc, outputDir / "Assets",
            fs::copy_options::overwrite_existing | fs::copy_options::recursive);
        log("Copied assets");
    }

    // ── Step 6: Write startup scene config ──────────────────────────
    if (!settings.StartupScene.empty()) {
        // Copy startup scene to Assets/startup.vscene
        fs::path startupSrc(settings.StartupScene);
        if (fs::exists(startupSrc)) {
            fs::copy_file(startupSrc, outputDir / "Assets" / "startup.vscene",
                fs::copy_options::overwrite_existing);
            log("Set startup scene: " + startupSrc.filename().string());
        }

        // Also write GameSettings.yaml
        std::ofstream gsOut((outputDir / "ProjectSettings" / "GameSettings.yaml").string());
        gsOut << "StartupScene: Assets/startup.vscene\n";
        gsOut << "GameName: " << settings.GameName << "\n";
        gsOut.close();
    }

    // ── Step 7: Copy script DLL if present ──────────────────────────
    fs::path scriptDLL = buildDir / "bin" / config / "ScriptBuild" / "out" / config / "GameScripts.dll";
    if (fs::exists(scriptDLL)) {
        fs::copy_file(scriptDLL, outputDir / "GameScripts.dll",
            fs::copy_options::overwrite_existing);
        log("Copied script DLL");
    }

    log("=== Build complete! ===");
    log("Output: " + outputDir.string());

    s_Building = false;
    return true;
}

} // namespace VE
