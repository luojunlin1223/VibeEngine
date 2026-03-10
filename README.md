# VibeEngine

An experimental game engine where **every line of code is written by AI**.

Built with C++17, CMake, and dual OpenGL/Vulkan rendering backends.

## Features

- **Dual Graphics Backend** — runtime-switchable OpenGL 4.6 and Vulkan 1.3
- **Renderer Abstraction Layer** — unified API for Shader, Buffer, VertexArray, RenderCommand
- **OpenGL Backend** — full rendering pipeline with glad2 loader
- **Vulkan Backend** — complete pipeline: swapchain, render pass, graphics pipeline, command buffers, double-buffered frame sync
- **Log System** — color-coded engine/client logging via spdlog
- **GLFW Window** — cross-API window management with automatic context selection

## Tech Stack

| Component | Technology |
|-----------|-----------|
| Language | C++17 |
| Build | CMake 3.20+ |
| Windowing | GLFW 3.4 |
| OpenGL Loader | glad2 (GL 4.6 Core) |
| Vulkan | Vulkan SDK 1.3+ |
| Logging | spdlog 1.15 |
| Dependency Mgmt | CMake FetchContent (auto-download) |

## Project Structure

```
VibeEngine/
├── CMakeLists.txt                    # Root build config + dependency management
├── engine/
│   ├── CMakeLists.txt                # Engine static library
│   ├── include/VibeEngine/
│   │   ├── Core/
│   │   │   ├── Application.h         # App lifecycle + main loop
│   │   │   ├── Log.h                 # spdlog wrapper + macros
│   │   │   └── Window.h              # GLFW window abstraction
│   │   ├── Renderer/
│   │   │   ├── Renderer.h            # High-level renderer
│   │   │   ├── RendererAPI.h         # Abstract rendering interface
│   │   │   ├── RenderCommand.h       # Static command dispatch
│   │   │   ├── Buffer.h              # Vertex/Index buffer + layout
│   │   │   ├── VertexArray.h         # VAO abstraction
│   │   │   ├── Shader.h              # Shader program
│   │   │   └── GraphicsContext.h     # Graphics context interface
│   │   ├── Platform/
│   │   │   ├── OpenGL/               # OpenGL implementations
│   │   │   └── Vulkan/               # Vulkan implementations
│   │   └── VibeEngine.h              # Single-include convenience header
│   └── src/                          # Implementation files (mirrors include/)
└── sandbox/
    ├── CMakeLists.txt
    └── main.cpp                      # Demo app — renders a colored triangle
```

## Build

### Prerequisites

- CMake 3.20+
- C++17 compiler (MSVC 2022 / GCC 11+ / Clang 14+)
- Vulkan SDK 1.3+ (for Vulkan backend)
- Git (for FetchContent dependency download)

### Build Steps

```bash
# Configure (dependencies auto-downloaded on first run)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build --config Debug

# Run
./build/bin/Debug/Sandbox
```

## Switching Graphics Backend

In `sandbox/main.cpp`, change the API passed to the Application constructor:

```cpp
// Use Vulkan
Sandbox() : VE::Application(VE::RendererAPI::API::Vulkan) { ... }

// Use OpenGL
Sandbox() : VE::Application(VE::RendererAPI::API::OpenGL) { ... }
```

## Architecture

```
┌─────────────┐
│   Sandbox    │  ← User application (inherits Application)
├─────────────┤
│  Renderer    │  ← RenderCommand / Shader / Buffer / VertexArray
├──────┬──────┤
│OpenGL│Vulkan│  ← Platform backends (selected at init)
├──────┴──────┤
│  Core       │  ← Application, Window, Log
├─────────────┤
│ GLFW / SDK  │  ← Third-party
└─────────────┘
```

The engine uses a **factory pattern** — abstract interfaces (`RendererAPI`, `Buffer`, `Shader`, etc.) with platform-specific implementations created via static `Create()` methods based on the selected API.

## Roadmap

- [x] CMake build system + FetchContent dependencies
- [x] Window + game loop (GLFW)
- [x] Log system (spdlog)
- [x] Renderer abstraction layer
- [x] OpenGL 4.6 backend (triangle rendering)
- [x] Vulkan 1.3 backend (triangle rendering)
- [ ] Math library (glm or custom)
- [ ] Texture loading + 2D sprite rendering
- [ ] Camera system
- [ ] ECS (Entity-Component-System)
- [ ] Input system
- [ ] Resource manager
- [ ] Scene graph
- [ ] Audio system
- [ ] Physics system
- [ ] Editor UI (ImGui)

## License

TBD

---

> Every line of code in this project is authored by AI (Claude).
