# VibeEngine core library
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(VibeEngineLib STATIC ${ENGINE_SOURCES})

target_include_directories(VibeEngineLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(VibeEngineLib
    PUBLIC
        glfw
        spdlog::spdlog
        glad_gl_core_46
        Vulkan::Vulkan
        imgui_lib
        EnTT::EnTT
)

# Platform definitions
if(WIN32)
    target_compile_definitions(VibeEngineLib PUBLIC VE_PLATFORM_WINDOWS)
endif()

target_compile_definitions(VibeEngineLib PUBLIC
    $<$<CONFIG:Debug>:VE_DEBUG>
    $<$<CONFIG:Release>:VE_RELEASE>
)
