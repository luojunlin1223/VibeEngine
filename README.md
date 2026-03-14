# VibeEngine

An experimental game engine where **every line of code is written by AI**.

Built with C++17, CMake, and dual OpenGL/Vulkan rendering backends.

## Features

### Rendering
- **Dual Graphics Backend** — runtime-switchable OpenGL 4.6 and Vulkan 1.3
- **PBR Lighting** — Cook-Torrance BRDF with directional + up to 8 point lights
- **Cascaded Shadow Maps** — 3-cascade CSM with configurable bias and PCF filtering
- **GPU Instanced Rendering** — automatic mesh+material batching for draw call reduction
- **Transparent Object Sorting** — back-to-front rendering with per-material blend state
- **Anti-Aliasing** — MSAA (2x/4x/8x), FXAA (post-process edge smoothing), TAA (temporal accumulation)
- **Sky Rendering** — gradient and equirectangular texture sky sphere
- **Texture System** — full texture pipeline for both backends with mipmaps
- **ShaderLab** — Unity-style `.shader` file format with parser, compiler, and runtime loading
- **Post-Processing** — Bloom, Vignette, Color Curves, SMH grading, Tonemapping (ACES/Reinhard/Uncharted2)
- **Frustum Culling** — automatic per-entity visibility testing against camera frustum
- **LOD System** — distance-based mesh switching with configurable LOD levels and cull distance

### Terrain
- **Heightmap Terrain** — grid mesh generation from heightmap images or procedural noise
- **Procedural Noise** — FBM noise with configurable octaves, persistence, lacunarity, seed
- **Multi-Layer Splatting** — 4-layer height-based texture blending with slope-aware rock blending
- **Terrain Shader** — world-space UV tiling, Blinn-Phong lighting, point light support

### Editor
- **ImGui-based Editor** — docking layout with viewport, hierarchy, inspector, content browser
- **Editor Camera** — 2D orthographic and 3D perspective modes with orbit, pan, zoom
- **Gizmo System** — translation gizmos with screen-space hit testing and axis dragging
- **Content Browser** — asset browsing with thumbnails, folder tree, drag-and-drop
- **Scene Serialization** — YAML-based `.vscene` files with save/load/file dialogs
- **Play Mode** — Play/Stop with scene snapshot and restore
- **Render Pipeline Panel** — sky, shadows, bloom, vignette, color grading, tonemapping, AA settings
- **Entity Active Toggle** — enable/disable entities with hierarchy-aware propagation
- **Build Export** — standalone game runtime export (.exe + assets)

### Entity Component System
- **entt ECS** — high-performance entity-component system
- **Components**: Transform, MeshRenderer, DirectionalLight, PointLight, Rigidbody, Collider (Box/Sphere/Capsule/Mesh), Camera, Script, Animator, AudioSource, AudioListener, SpriteRenderer, SpriteAnimator, ParticleSystem, LODGroup, Terrain, UICanvas, UIRectTransform, UIText, UIImage, UIButton
- **Built-in Meshes**: Triangle, Quad, Cube, Sphere
- **Parent-Child Hierarchy** — entity parenting with world transform propagation

### Runtime UI System
- **Independent of ImGui** — batched screen-space quad rendering using Sprite shader
- **Font Rendering** — stb_truetype TTF loading + embedded bitmap font fallback
- **Components**: UICanvas, UIRectTransform (9-point anchoring), UIText, UIImage, UIButton
- **Anchor System** — TopLeft/Center/BottomRight etc. with pixel offset and pivot
- **Button Interaction** — hover/press/click detection with embedded label text

### Physics
- **Jolt Physics** — 3D rigid body simulation (static, kinematic, dynamic)
- Box, Sphere, Capsule, and Mesh colliders
- Fixed 60Hz timestep with accumulator

### Audio
- **miniaudio** — Play/Stop/Volume control, 3D spatial audio
- AudioSource and AudioListener components with play-on-awake

### Scripting
- **C++ Native Scripts** — DLL hot-reload with automatic recompilation
- Multi-script architecture with `REGISTER_SCRIPT()` macro
- Property reflection (Float/Int/Bool/Vec3) with Inspector editing
- Script API: Log, Input, Transform, Entity, Audio functions

### Input System
- **Action Mapping** — configurable input actions with keyboard/mouse/gamepad bindings
- **GLFW Callbacks** — event-driven input with key, mouse button, scroll, gamepad support

### Asset Pipeline
- **Asset Database** — file watching, `.meta` files, UUID tracking
- **FBX Import** — mesh + skeleton + animation importing via ufbx
- **Material System** — PBR materials with shader + property overrides, `.vmat` YAML files
- **Skeletal Animation** — bone transforms, skinned mesh rendering, animation clips

### 2D Rendering
- **Sprite Batch Renderer** — up to 10,000 quads per draw call with 16 texture slots
- **Sprite Animation** — sheet-based frame animation with configurable rows/columns/framerate
- **Particle System** — billboard particles with emission, lifetime, color/size over time

## ShaderLab

VibeEngine uses a Unity-compatible ShaderLab syntax for shader authoring:

```glsl
Shader "VibeEngine/MyShader" {
    Properties {
        _MainTex ("Texture", 2D) = "white" {}
        _Color ("Color", Color) = (1, 1, 1, 1)
        _Metallic ("Metallic", Range(0, 1)) = 0.0
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Geometry" }

        Pass {
            Name "ForwardLit"
            Cull Back
            ZWrite On
            Blend SrcAlpha OneMinusSrcAlpha

            GLSLPROGRAM
            #version 460 core

            #ifdef VERTEX
            // vertex shader
            #endif

            #ifdef FRAGMENT
            // fragment shader
            #endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/Unlit"
}
```

### Supported Property Types
- `Float`, `Int` — scalar values
- `Range(min, max)` — clamped float with slider UI
- `Color` — RGBA color picker
- `Vector` — 4-component vector
- `2D`, `3D`, `Cube` — texture samplers

## Tech Stack

| Component | Technology |
|-----------|-----------|
| Language | C++17 |
| Build | CMake 3.20+ |
| Graphics | OpenGL 4.6, Vulkan 1.3 |
| Windowing | GLFW 3.4 |
| Math | GLM 1.0.1 |
| ECS | entt 3.14.0 |
| Physics | Jolt Physics 5.2.0 |
| Editor UI | Dear ImGui (docking) |
| Audio | miniaudio |
| Font | stb_truetype |
| Serialization | yaml-cpp 0.8.0 |
| Logging | spdlog 1.15 |
| FBX Import | ufbx |

## Project Structure

```
VibeEngine/
├── CMakeLists.txt                    # Root build config + dependency management
├── engine/
│   ├── CMakeLists.txt                # Engine static library
│   ├── include/VibeEngine/
│   │   ├── Core/                     # Application, Window, Log, Input, BuildExporter
│   │   ├── Renderer/                 # Shader, Buffer, Texture, Material, PostProcessing, LOD
│   │   ├── Scene/                    # Scene, Entity, Components, MeshLibrary
│   │   ├── Asset/                    # AssetDatabase, FileWatcher, MeshImporter
│   │   ├── Physics/                  # PhysicsWorld (Jolt)
│   │   ├── Scripting/                # ScriptEngine, NativeScript, ScriptGlue
│   │   ├── Animation/                # Skeleton, AnimationClip, Animator
│   │   ├── Audio/                    # AudioEngine (miniaudio)
│   │   ├── Terrain/                  # Terrain heightmap generation
│   │   ├── UI/                       # FontAtlas, UIRenderer
│   │   ├── Input/                    # InputAction, KeyCodes
│   │   └── Platform/                 # OpenGL/ and Vulkan/ backend implementations
│   ├── src/                          # Implementation files (mirrors include/)
│   └── shaders/                      # ShaderLab .shader files (Lit, Unlit, Sky, Terrain, etc.)
├── sandbox/                          # Editor application
├── runtime/                          # Standalone game runtime
└── vendor/                           # Third-party (ufbx, stb, miniaudio)
```

## Build

### Prerequisites

- CMake 3.20+
- Visual Studio 2022 with C++17 support
- Vulkan SDK 1.3+
- Python 3 (for glad2 generation)

### Build Steps

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
./build/bin/Debug/Sandbox
```

## Architecture

```
┌─────────────┐
│   Sandbox    │  ← Editor application
├─────────────┤
│   Runtime    │  ← Standalone game runtime (exported builds)
├─────────────┤
│  ShaderLab  │  ← .shader file parser + compiler
├─────────────┤
│  Renderer   │  ← RenderCommand / Material / PostProcessing / RenderGraph
├──────┬──────┤
│OpenGL│Vulkan│  ← Platform backends (runtime-switchable)
├──────┴──────┤
│ Scene / ECS │  ← Entity, Components, Terrain, UI, LOD
├─────────────┤
│   Systems   │  ← Physics, Audio, Scripting, Animation, Input
├─────────────┤
│    Core     │  ← Application, Window, Log, Asset Pipeline
├─────────────┤
│ GLFW / Deps │  ← Third-party libraries
└─────────────┘
```

The engine uses a **factory pattern** — abstract interfaces (`RendererAPI`, `Buffer`, `Shader`, etc.) with platform-specific implementations created via static `Create()` methods based on the selected API.

## Roadmap

- [x] CMake build system + FetchContent dependencies
- [x] Window + game loop (GLFW)
- [x] Log system (spdlog)
- [x] Renderer abstraction layer
- [x] OpenGL 4.6 backend
- [x] Vulkan 1.3 backend
- [x] Math library (GLM)
- [x] ECS (entt) with Scene/Entity/Components
- [x] ImGui editor with docking
- [x] Runtime renderer switching (OpenGL ↔ Vulkan)
- [x] Scene serialization (YAML)
- [x] Editor camera (2D/3D)
- [x] Gizmo system (translation, selection)
- [x] 3D PBR renderer with cascaded shadow maps
- [x] Texture system (OpenGL + Vulkan)
- [x] Directional + Point light components
- [x] Sky rendering (gradient + equirectangular)
- [x] Physics system (Jolt Physics)
- [x] Asset management (FileWatcher, AssetDatabase, Content Browser)
- [x] FBX import with skeletal animation
- [x] ShaderLab (.shader file format)
- [x] PBR material system with property overrides
- [x] 2D sprite rendering + batch renderer
- [x] Particle system
- [x] Audio system (miniaudio)
- [x] C++ native scripting with DLL hot-reload
- [x] GPU instanced rendering
- [x] Frustum culling + render statistics
- [x] Input system with action mapping
- [x] Runtime UI system (non-ImGui)
- [x] Anti-aliasing (MSAA / FXAA / TAA)
- [x] LOD system
- [x] Transparent object sorting
- [x] Terrain system (heightmap + procedural noise)
- [x] Standalone game runtime + build export
- [ ] Multi-platform support (Linux, macOS)
- [ ] Vulkan compute shaders
- [ ] Navmesh / AI pathfinding

## License

TBD

---

> Every line of code in this project is authored by AI (Claude).
