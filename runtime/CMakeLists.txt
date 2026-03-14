# VibeEngine standalone game runtime
add_executable(VibeRuntime main.cpp)

target_link_libraries(VibeRuntime PRIVATE VibeEngineLib)

# No VE_PROJECT_ROOT needed — runtime uses relative paths
