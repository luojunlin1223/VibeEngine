# Sandbox test application
add_executable(Sandbox main.cpp)

target_link_libraries(Sandbox PRIVATE VibeEngineLib)

# Inject project root so Sandbox can find ScriptProject/ at runtime
target_compile_definitions(Sandbox PRIVATE
    VE_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)
