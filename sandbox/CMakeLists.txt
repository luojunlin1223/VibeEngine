# Sandbox test application
add_executable(Sandbox main.cpp)

target_link_libraries(Sandbox PRIVATE VibeEngineLib)

if(MSVC)
    target_compile_options(Sandbox PRIVATE /bigobj)
endif()

# Inject project root so Sandbox can find ScriptProject/ at runtime
target_compile_definitions(Sandbox PRIVATE
    VE_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

# Copy scene files from source Assets/ to output directory
file(GLOB VE_SCENE_FILES "${CMAKE_SOURCE_DIR}/Assets/*.vscene")
foreach(SCENE_FILE ${VE_SCENE_FILES})
    get_filename_component(SCENE_NAME ${SCENE_FILE} NAME)
    add_custom_command(TARGET Sandbox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/Assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SCENE_FILE}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/Assets/${SCENE_NAME}"
        COMMENT "Copying scene: ${SCENE_NAME}"
    )
endforeach()
