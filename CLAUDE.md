# VibeEngine

A game engine fully driven and developed by AI.

## Project Vision

VibeEngine is an experimental game engine where all code is authored by AI. The goal is to explore how far AI-driven development can push the boundaries of game engine architecture, performance, and usability.

## Tech Stack

- **Language**: C++ (C++17 or later)
- **Build System**: CMake
- **Platform**: Windows (primary), cross-platform as a stretch goal

## Project Structure

```
VibeEngine/
├── CMakeLists.txt          # Root CMake (deps via FetchContent)
├── engine/
│   ├── include/VibeEngine/
│   │   ├── Core/           # Application, Window, Log
│   │   ├── Renderer/       # Abstract: RendererAPI, Buffer, Shader, VertexArray
│   │   └── Platform/       # OpenGL/ and Vulkan/ backend implementations
│   └── src/                # .cpp files (mirrors include/)
└── sandbox/                # Test application
```

## Development Guidelines

### Code Style
- Write clean, readable, well-structured code
- Prefer simplicity over cleverness
- Keep functions small and focused
- Use meaningful names for variables, functions, and types

### Architecture Principles
- Modular design: each subsystem should be independently testable
- Data-oriented design where performance matters
- Clear separation between engine core and editor/tools
- Minimize external dependencies; prefer lightweight, focused libraries

### Commit Conventions
- Use concise, descriptive commit messages in English
- Format: `<type>: <description>` (e.g., `feat: add 2D sprite renderer`)
- Types: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`

### AI Development Notes
- All code in this project is AI-authored
- Each major decision should be documented in commit messages or inline comments
- When adding a new subsystem, create a brief design doc as a comment block at the top of the main file
