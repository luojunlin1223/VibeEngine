# VibeEngine

An experimental game engine where **every line of code is written by AI**.

Built with C++17, CMake, and dual OpenGL/Vulkan rendering backends.

## Features

### Rendering
- **Dual Graphics Backend** — runtime-switchable OpenGL 4.6 and Vulkan 1.3
- **PBR Lighting** — physically-based rendering with GGX/Smith/Schlick BRDF
- **Sky Rendering** — gradient and equirectangular texture sky sphere
- **Texture System** — full texture pipeline for both backends
- **ShaderLab** — Unity-style `.shader` file format with parser, compiler, and runtime loading

### Editor
- **ImGui-based Editor** — docking layout with viewport, hierarchy, inspector, content browser
- **Editor Camera** — 2D orthographic and 3D perspective modes with orbit, pan, zoom
- **Gizmo System** — translation gizmos with screen-space hit testing and axis dragging
- **Content Browser** — asset browsing with thumbnails, folder tree, drag-and-drop
- **Scene Serialization** — YAML-based `.vscene` files with save/load/file dialogs
- **Play Mode** — Play/Stop with scene snapshot and restore

### Entity Component System
- **entt ECS** — high-performance entity-component system
- Components: Transform, MeshRenderer, DirectionalLight, Rigidbody, Collider, Tag
- Built-in meshes: Triangle, Quad, Cube, Sphere

### Physics
- **Jolt Physics** — 3D rigid body simulation (static, kinematic, dynamic)
- Box, Sphere, and Capsule colliders
- Fixed 60Hz timestep with accumulator

### Asset Pipeline
- **Asset Database** — file watching, `.meta` files, UUID tracking
- **FBX Import** — mesh importing via ufbx with vertex deduplication
- **Material System** — shader + property overrides with YAML serialization

## ShaderLab

VibeEngine uses a Unity-compatible ShaderLab syntax for shader authoring, providing a unified format for cross-platform shader development:

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
            ZTest LEqual

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            #version 460 core

            #ifdef VERTEX
            // ... vertex shader code ...
            #endif

            #ifdef FRAGMENT
            // ... fragment shader code ...
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

### Render State
- `Cull Back|Front|Off`
- `ZWrite On|Off`
- `ZTest Less|LEqual|Equal|GEqual|Greater|NotEqual|Always|Never`
- `Blend SrcFactor DstFactor`

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
| UI | Dear ImGui (docking) |
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
│   │   ├── Core/                     # Application, Window, Log
│   │   ├── Renderer/                 # Shader, Buffer, Texture, Material, ShaderLab
│   │   ├── Scene/                    # Scene, Entity, Components, MeshLibrary
│   │   ├── Asset/                    # AssetDatabase, FileWatcher, MeshImporter
│   │   ├── Physics/                  # PhysicsWorld
│   │   └── Platform/                 # OpenGL/ and Vulkan/ backend implementations
│   ├── src/                          # Implementation files (mirrors include/)
│   └── shaders/                      # ShaderLab .shader files + GLSL/SPIR-V sources
├── sandbox/                          # Editor/test application
└── vendor/                           # Third-party (ufbx, stb)
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
│   Sandbox    │  ← Editor application (inherits Application)
├─────────────┤
│  ShaderLab  │  ← .shader file parser + compiler
├─────────────┤
│  Renderer   │  ← RenderCommand / Shader / Buffer / Material / Texture
├──────┬──────┤
│OpenGL│Vulkan│  ← Platform backends (runtime-switchable)
├──────┴──────┤
│  Scene/ECS  │  ← Entity, Components, MeshLibrary, Physics
├─────────────┤
│  Core       │  ← Application, Window, Log, Asset System
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
- [x] Runtime renderer switching (OpenGL <-> Vulkan)
- [x] Scene serialization (YAML)
- [x] Editor camera (2D/3D)
- [x] Gizmo system (translation, selection)
- [x] 3D renderer (PBR lighting, depth buffer)
- [x] Texture system (OpenGL + Vulkan)
- [x] Directional light component
- [x] Sky rendering (gradient + equirectangular)
- [x] Physics system (Jolt Physics)
- [x] Asset management (FileWatcher, AssetDatabase, Content Browser)
- [x] FBX import (ufbx)
- [x] ShaderLab (.shader file format)
- [ ] Material editor with ShaderLab property UI
- [ ] Audio system
- [ ] Scripting system
- [ ] Multi-platform support

## License

TBD

---

> Every line of code in this project is authored by AI (Claude).
