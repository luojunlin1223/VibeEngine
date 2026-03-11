# VibeEngine core library
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(VibeEngineLib STATIC ${ENGINE_SOURCES} ${CMAKE_SOURCE_DIR}/vendor/ufbx/ufbx.c)

target_include_directories(VibeEngineLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor/stb
        ${CMAKE_SOURCE_DIR}/vendor/ufbx
)

target_link_libraries(VibeEngineLib
    PUBLIC
        glfw
        spdlog::spdlog
        glad_gl_core_46
        Vulkan::Vulkan
        imgui_lib
        EnTT::EnTT
        yaml-cpp::yaml-cpp
        glm::glm
        Jolt
)

# Platform definitions
if(WIN32)
    target_compile_definitions(VibeEngineLib PUBLIC VE_PLATFORM_WINDOWS)
endif()

target_compile_definitions(VibeEngineLib PUBLIC
    $<$<CONFIG:Debug>:VE_DEBUG>
    $<$<CONFIG:Release>:VE_RELEASE>
)
