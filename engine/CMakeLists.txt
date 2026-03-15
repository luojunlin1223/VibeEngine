# VibeEngine core library
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(VibeEngineLib STATIC ${ENGINE_SOURCES}
    ${CMAKE_SOURCE_DIR}/vendor/ufbx/ufbx.c
    ${CMAKE_SOURCE_DIR}/vendor/cgltf/cgltf_impl.c
    ${CMAKE_SOURCE_DIR}/vendor/pl_mpeg/pl_mpeg_impl.c
)

target_include_directories(VibeEngineLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor/stb
        ${CMAKE_SOURCE_DIR}/vendor/ufbx
        ${CMAKE_SOURCE_DIR}/vendor/miniaudio
        ${CMAKE_SOURCE_DIR}/vendor/cgltf
        ${CMAKE_SOURCE_DIR}/vendor/pl_mpeg
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

# ── Copy ShaderLab .shader files to output directory ─────────────────
# Copy ShaderLab .shader files next to the executable (handles per-config dirs)
set(VE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
file(GLOB VE_SHADER_FILES "${VE_SHADER_DIR}/*.shader")
foreach(SHADER_FILE ${VE_SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
    add_custom_command(TARGET VibeEngineLib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHADER_FILE}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/shaders/${SHADER_NAME}"
        COMMENT "Copying shader: ${SHADER_NAME}"
    )
endforeach()
