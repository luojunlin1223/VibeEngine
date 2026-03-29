/*
 * EditorPlugin — Base class for hot-reloadable editor UI plugins.
 *
 * Plugins are compiled into a DLL with ImGui linked in.  The engine passes
 * its ImGuiContext* so plugins can draw UI directly via ImGui calls.
 *
 * Usage:
 *   class MyPanel : public VE::EditorPlugin {
 *       void OnEditorUI() override { ImGui::Begin("My Panel"); ... ImGui::End(); }
 *   };
 *   REGISTER_PLUGIN(MyPanel)
 */
#pragma once

#include <cstdint>

// ── DLL export macro ───────────────────────────────────────────────
#ifdef VE_PLUGIN_DLL
  #ifdef _WIN32
    #define VE_PLUGIN_API __declspec(dllexport)
  #else
    #define VE_PLUGIN_API __attribute__((visibility("default")))
  #endif
#else
  #define VE_PLUGIN_API
#endif

namespace VE {

// Viewport info — set by engine each frame, readable by plugins
struct PluginViewport {
    float X = 0, Y = 0, W = 0, H = 0;
};

class EditorPlugin {
public:
    virtual ~EditorPlugin() = default;

    // Engine sets this each frame before calling OnEditorUI
    static PluginViewport Viewport;

    virtual void OnInit()       {}   // Called after DLL load
    virtual void OnShutdown()   {}   // Called before DLL unload
    virtual void OnEditorUI()   {}   // Called every frame — draw ImGui here
};

} // namespace VE

// ── Plugin entry (for registry) ────────────────────────────────────
struct PluginEntry {
    const char* Name;
    VE::EditorPlugin* (*CreateFunc)();
};

// ── Registration macro ─────────────────────────────────────────────
#define REGISTER_PLUGIN(ClassName) \
    VE::EditorPlugin* Create_##ClassName() { return new ClassName(); }
