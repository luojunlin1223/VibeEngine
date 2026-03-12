cmake_minimum_required(VERSION 3.20)
project(GameScripts LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_library(GameScripts SHARED
    PlayerController.cpp
)

target_include_directories(GameScripts PRIVATE
    ${CMAKE_SOURCE_DIR}/../engine/include
)

target_compile_definitions(GameScripts PRIVATE VE_SCRIPT_DLL)
