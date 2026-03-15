<p align="center">
  <h1 align="center">VibeEngine</h1>
  <p align="center"><strong>A game engine fully driven and developed by AI</strong></p>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue?logo=cplusplus" alt="C++17">
  <img src="https://img.shields.io/badge/OpenGL-4.6-green?logo=opengl" alt="OpenGL 4.6">
  <img src="https://img.shields.io/badge/Vulkan-1.3-red?logo=vulkan" alt="Vulkan 1.3">
  <img src="https://img.shields.io/badge/Platform-Windows-lightgrey?logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/Build-CMake-064F8C?logo=cmake" alt="CMake">
  <img src="https://img.shields.io/badge/License-MIT-yellow" alt="MIT License">
</p>

---

![VibeEngine Editor](docs/screenshot.png)

VibeEngine is an experimental game engine where **every line of code is authored by AI**. It explores how far AI-driven development can push the boundaries of game engine architecture, performance, and usability -- spanning a full renderer abstraction with dual OpenGL/Vulkan backends, an ECS-based scene system, integrated physics, audio, scripting, and a complete docking editor.

---

## Features

### Rendering

- [x] **Dual Backend** -- runtime-switchable OpenGL 4.6 and Vulkan 1.3
- [x] **PBR Pipeline** -- Cook-Torrance BRDF with metallic/roughness workflow
- [x] **HDR & Tonemapping** -- ACES, Reinhard, and Uncharted2 tone mappers
- [x] **Screen-Space Reflections (SSR)** -- real-time reflective surfaces
- [x] **Screen-Space Ambient Occlusion (SSAO)** -- contact shadows and ambient darkening
- [x] **Bloom** -- multi-pass threshold bloom with intensity control
- [x] **Depth of Field** -- bokeh-style depth blur
- [x] **Motion Blur** -- per-object velocity-based blur
- [x] **Shadow Mapping** -- cascaded shadow maps (3-cascade CSM) with PCF filtering
- [x] **Decals** -- projected decal rendering with dedicated shader
- [x] **Particle System** -- billboard particles with emission, lifetime, color/size over time
- [x] **Animated Water** -- shader-driven waves with Fresnel and specular highlights
- [x] **Sky Rendering** -- gradient and equirectangular texture skysphere
- [x] **Volumetric Fog** -- ray-marched fog with Henyey-Greenstein phase scattering
- [x] **Anti-Aliasing** -- MSAA (2x/4x/8x), FXAA, and TAA
- [x] **Instanced Rendering** -- automatic mesh+material batching for draw call reduction
- [x] **LOD System** -- distance-based mesh switching with configurable LOD levels and cull distance
- [x] **Reflection Probes** -- environment cubemap capture and blending
- [x] **Occlusion Culling** -- frustum and occlusion-based visibility testing
- [x] **Light Probes** -- irradiance sampling for indirect lighting
- [x] **Lightmapper** -- baked lightmap generation
- [x] **Sprite Batch Renderer** -- up to 10,000 quads per draw call with 16 texture slots
- [x] **ShaderLab System** -- Unity-style `.shader` file format with parser and compiler
- [x] **Material System** -- PBR materials with shader + property overrides, `.vmat` YAML files
- [x] **Render Graph** -- frame render pass organization
- [x] **Framebuffers** -- off-screen render targets and post-processing pipeline

### Editor

- [x] **ImGui Docking UI** -- full docking workspace (Dear ImGui docking branch)
- [x] **Transform Gizmos** -- Translate, Rotate, and Scale with Ctrl+Drag snap alignment
- [x] **Undo / Redo** -- complete coverage for all Inspector property changes
- [x] **Multi-Select** -- select and manipulate multiple entities
- [x] **Copy / Paste** -- duplicate entities in the scene
- [x] **Material Editor** -- visual material and shader property editing
- [x] **Content Browser** -- asset grid with thumbnails, folder tree, drag-and-drop
- [x] **Profiler** -- runtime performance profiler panel
- [x] **Console** -- log output panel with filtering
- [x] **WASD Camera** -- orbit, pan, and fly modes (2D orthographic + 3D perspective)
- [x] **Scene Manager** -- multi-scene loading and switching
- [x] **Auto-Save** -- periodic scene auto-save
- [x] **Play / Stop Mode** -- scene snapshot on play, full restore on stop
- [x] **Render Pipeline Panel** -- sky, shadows, bloom, vignette, color grading, tonemapping, AA settings
- [x] **Build Export** -- standalone game runtime packaging (.exe + assets)
- [x] **Grid Rendering** -- XY grid (2D) and XZ grid (3D) overlays
- [x] **Click Selection** -- screen-space entity picking with radius test

### Physics

- [x] **Jolt Physics** -- full 3D rigid body simulation via Jolt Physics v5.2.0
- [x] **Rigidbody Component** -- Static, Kinematic, and Dynamic body types with mass, damping, restitution, friction
- [x] **Colliders** -- Box, Sphere, Capsule, and Mesh shapes with size and offset
- [x] **Raycasting** -- physics ray queries
- [x] **Fixed Timestep** -- 60 Hz accumulator-based physics update

### Animation

- [x] **Skeletal Animation** -- bone hierarchy with skinned mesh playback
- [x] **Animation Clips** -- keyframe animation data
- [x] **State Machine** -- animation state graph with transitions
- [x] **Crossfade Blending** -- smooth transitions between animation states

### Audio

- [x] **miniaudio Backend** -- cross-format playback (WAV, MP3, OGG, FLAC)
- [x] **3D Spatial Audio** -- distance-based attenuation and positioning
- [x] **AudioSource Component** -- per-entity audio with volume, pitch, loop, play-on-awake
- [x] **AudioListener Component** -- camera-relative audio listener
- [x] **Script API** -- Play, Stop, SetVolume, SetMasterVolume from C++ scripts

### Scripting

- [x] **C++ Native Scripting** -- DLL-based gameplay scripts with no link-time dependency
- [x] **Hot-Reload** -- automatic DLL reload on recompile (timestamp monitoring)
- [x] **Property Reflection** -- `VE_PROPERTIES()` macros expose Float, Int, Bool, Vec3, String to Inspector
- [x] **ScriptAPI** -- function pointer table for Log, Input, Transform, Entity, Audio access
- [x] **Multi-Script DLL** -- `REGISTER_SCRIPT()` macro with auto-generated registry
- [x] **Editor Integration** -- New Script button, auto-compile, build status panel

### Assets

- [x] **FBX Import** -- mesh + skeleton + animation importing via ufbx with vertex deduplication
- [x] **glTF Import** -- mesh importing via cgltf
- [x] **AssetDatabase** -- UUID-based asset tracking with `.meta` files
- [x] **File Watcher** -- live directory monitoring for asset changes
- [x] **Thumbnail Cache** -- lazy-loaded image previews for Content Browser
- [x] **Drag-and-Drop** -- drop assets from Content Browser to viewport to create entities

### Scene & ECS

- [x] **Entity Component System** -- built on entt v3.14.0
- [x] **Parent-Child Hierarchy** -- entity parenting with world transform propagation
- [x] **Multi-Scene** -- scene manager with loading and switching
- [x] **Prefabs** -- reusable entity templates
- [x] **Scene Serialization** -- YAML-based `.vscene` files via yaml-cpp
- [x] **UUID** -- unique entity identification across sessions
- [x] **Components** -- Tag, Transform, MeshRenderer, DirectionalLight, PointLight, Rigidbody, Collider, Camera, Script, Animator, AudioSource, AudioListener, SpriteRenderer, SpriteAnimator, ParticleSystem, LODGroup, Terrain, UICanvas, UIRectTransform, UIText, UIImage, UIButton

### UI System

- [x] **Runtime UI Renderer** -- batched screen-space quad rendering (independent of ImGui)
- [x] **Font Atlas** -- stb_truetype TTF loading + embedded bitmap font fallback
- [x] **UI Components** -- Canvas, RectTransform (9-point anchoring), Text, Image, Button
- [x] **Button Interaction** -- hover/press/click detection with embedded label text

### Navigation

- [x] **A\* Pathfinding** -- grid-based navigation via NavGrid

### Terrain

- [x] **Heightmap Terrain** -- grid mesh generation from heightmap images
- [x] **Procedural Noise** -- FBM noise with configurable octaves, persistence, lacunarity, seed
- [x] **Multi-Layer Splatting** -- 4-layer height-based texture blending with slope-aware rock blending
- [x] **Terrain Shader** -- world-space UV tiling, Blinn-Phong lighting, point light support

### Video

- [x] **MPEG-1 Playback** -- video rendering via pl_mpeg

### Input

- [x] **Keyboard & Mouse** -- polled and event-based input via GLFW
- [x] **Gamepad** -- controller support
- [x] **Input Actions** -- configurable action mapping with bindable keys/buttons

### Build & Export

- [x] **Standalone Game Export** -- package scenes, assets, and scripts into a runtime executable

---

## Tech Stack

| Category | Library | Version |
|----------|---------|---------|
| Language | C++ | 17 |
| Build System | CMake | 3.20+ |
| Windowing | GLFW | 3.4 |
| Graphics (GL) | glad / OpenGL | 4.6 Core |
| Graphics (VK) | Vulkan SDK | 1.3 |
| Editor UI | Dear ImGui | docking branch |
| ECS | entt | 3.14.0 |
| Physics | Jolt Physics | 5.2.0 |
| Math | GLM | 1.0.1 |
| Serialization | yaml-cpp | 0.8.0 |
| Logging | spdlog | 1.15.0 |
| Image Loading | stb_image | -- |
| Font Loading | stb_truetype | -- |
| FBX Import | ufbx | -- |
| glTF Import | cgltf | -- |
| Audio | miniaudio | -- |
| Video | pl_mpeg | -- |
| Testing | doctest | -- |

---

## Building

### Prerequisites

- **Windows 10 or 11**
- **Visual Studio 2022** (MSVC v143 toolchain)
- **CMake 3.20+**
- **Vulkan SDK 1.3+** ([LunarG](https://vulkan.lunarg.com/))
- **Python 3** (used by glad2 to generate the OpenGL loader)

### Build Steps

```bash
# Configure
cmake -S . -B build

# Build (Release)
cmake --build build --config Release

# Build (Debug)
cmake --build build --config Debug
```

### Run

```bash
# Editor / Sandbox
build/bin/Release/Sandbox.exe

# Standalone Runtime (after exporting a game)
build/bin/Release/VibeRuntime.exe
```

---

## Project Structure

```
VibeEngine/
├── CMakeLists.txt              # Root build -- all deps via FetchContent
├── engine/
│   ├── CMakeLists.txt          # VibeEngineLib static library
│   ├── include/VibeEngine/
│   │   ├── Core/               # Application, Window, Log, UUID, Profiler, Console
│   │   ├── Renderer/           # RendererAPI, Shader, Buffer, Texture, Framebuffer,
│   │   │                       # ShaderLab, PostProcessing, SSAO, SSR, ShadowMap,
│   │   │                       # ParticleSystem, Material, LOD, Occlusion, RenderGraph
│   │   ├── Platform/
│   │   │   ├── OpenGL/         # OpenGL backend implementations
│   │   │   └── Vulkan/         # Vulkan backend implementations
│   │   ├── Scene/              # ECS, Entity, Components, SceneSerializer, SceneManager
│   │   ├── Editor/             # EditorCommand (undo/redo)
│   │   ├── Physics/            # PhysicsWorld (Jolt wrapper, PIMPL)
│   │   ├── Animation/          # Skeleton, AnimationClip, Animator, StateMachine
│   │   ├── Audio/              # AudioEngine (miniaudio wrapper)
│   │   ├── Scripting/          # NativeScript, ScriptEngine, hot-reload
│   │   ├── Asset/              # AssetDatabase, FileWatcher, FBX/glTF importers
│   │   ├── Input/              # Input polling, KeyCodes, InputAction bindings
│   │   ├── Navigation/         # NavGrid (A* pathfinding)
│   │   ├── Terrain/            # Heightmap and procedural terrain
│   │   └── UI/                 # FontAtlas, UIRenderer
│   ├── src/                    # Implementation files (mirrors include/)
│   └── shaders/                # ShaderLab .shader files + Vulkan GLSL sources
├── sandbox/                    # Editor / test application
├── runtime/                    # Standalone game runtime executable
├── tests/                      # Unit tests (doctest, 71 test cases)
├── vendor/                     # Vendored single-header libraries
│   ├── stb/                    # stb_image, stb_truetype
│   ├── ufbx/                   # FBX loader
│   ├── cgltf/                  # glTF loader
│   ├── miniaudio/              # Audio engine
│   ├── pl_mpeg/                # MPEG-1 video decoder
│   └── doctest/                # Testing framework
├── Assets/                     # Project assets directory
└── Scripts/                    # User gameplay scripts (C++ DLL)
```

---

## Architecture

```
┌─────────────┐
│   Sandbox    │  Editor application
├─────────────┤
│   Runtime    │  Standalone game runtime (exported builds)
├─────────────┤
│  ShaderLab  │  .shader file parser + compiler
├─────────────┤
│  Renderer   │  RenderCommand / Material / PostProcessing / RenderGraph
├──────┬──────┤
│OpenGL│Vulkan│  Platform backends (runtime-switchable)
├──────┴──────┤
│ Scene / ECS │  Entity, Components, Terrain, UI, LOD
├─────────────┤
│   Systems   │  Physics, Audio, Scripting, Animation, Input, Navigation
├─────────────┤
│    Core     │  Application, Window, Log, Asset Pipeline
├─────────────┤
│ GLFW / Deps │  Third-party libraries
└─────────────┘
```

### Renderer Abstraction

VibeEngine uses a backend-agnostic rendering architecture. The abstract `RendererAPI` interface is implemented by both `OpenGLRendererAPI` and `VulkanRendererAPI`. A static `RenderCommand` dispatch layer routes all draw calls through the active backend. Switching between OpenGL and Vulkan happens at runtime -- entity GPU resources are automatically saved and restored by mesh index.

### Entity Component System

The scene system is built on **entt**. Each `Scene` owns an `entt::registry` and manages entity lifecycle. The `Entity` wrapper provides a clean template API for adding, getting, and removing components. Built-in components cover transforms, mesh rendering, lighting, physics, audio, animation, scripting, and UI.

### ShaderLab

Inspired by Unity's ShaderLab format, VibeEngine includes a custom `.shader` file parser and compiler. Shader files declare properties, render state (cull, blend, depth), and embed GLSL code in `GLSLPROGRAM ... ENDGLSL` blocks using `#ifdef VERTEX` / `#ifdef FRAGMENT` guards. The engine ships with built-in shaders: Unlit, Lit, PBR, Sky, Water, Terrain, Particle, Decal, Sprite, Outline, and instanced variants.

```glsl
Shader "VibeEngine/MyShader" {
    Properties {
        _MainTex ("Texture", 2D) = "white" {}
        _Color ("Color", Color) = (1, 1, 1, 1)
        _Metallic ("Metallic", Range(0, 1)) = 0.0
    }
    SubShader {
        Pass {
            Cull Back
            ZWrite On
            GLSLPROGRAM
            #version 460 core
            #ifdef VERTEX
            // vertex shader code
            #endif
            #ifdef FRAGMENT
            // fragment shader code
            #endif
            ENDGLSL
        }
    }
    FallBack "VibeEngine/Unlit"
}
```

### Native Scripting

Gameplay logic is written in C++ and compiled into a separate DLL. The engine communicates with scripts through a function pointer table (`ScriptAPI`), avoiding any link-time dependency. Scripts use `REGISTER_SCRIPT()` for registration and `VE_PROPERTIES()` for exposing fields to the Inspector. The engine monitors the DLL timestamp and hot-reloads automatically on recompile.

---

## Testing

VibeEngine includes **71 unit tests** covering core systems that do not require a GPU context: UUID generation, ECS operations, scene serialization, ShaderLab parsing, material properties, frustum culling, and input action mapping.

```bash
# Run all tests
build/bin/Debug/VibeTests.exe

# Run with verbose output
build/bin/Debug/VibeTests.exe -s
```

---

## License

This project is released under the [MIT License](LICENSE).

---

<p align="center">
  <strong>All code in this project is authored by AI</strong><br>
  Built with <a href="https://www.anthropic.com/claude">Claude</a> by Anthropic
</p>
