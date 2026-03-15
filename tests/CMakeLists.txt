# VibeEngine unit tests
add_executable(VibeTests main.cpp)

target_link_libraries(VibeTests PRIVATE VibeEngineLib)

target_include_directories(VibeTests PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/doctest
)

# We provide our own main() to initialize the engine log system
