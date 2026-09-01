# Prism - Proton/Wine screen-capture host with ReShade support
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This project builds Prism.exe only. The native capture bridge is built by
# bridge/Makefile with winegcc, because a Winelib module needs Wine's own
# toolchain driver and cannot share a MinGW cross-compilation environment.
# scripts/build.sh runs both.

cmake_minimum_required(VERSION 3.16)
project(Prism VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "Prism.exe must be built for Windows. Configure with the bundled toolchain file:\n"
        "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(Prism WIN32
    src/main.cpp
    src/App.cpp
    src/Renderer.cpp
    src/CaptureBridge.cpp
    src/FrameMailbox.cpp
    src/Diagnostics.cpp
    src/Settings.cpp
    src/TestPattern.cpp
    src/Hotkeys.cpp
)

target_include_directories(Prism PRIVATE src include)

target_compile_options(Prism PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
target_compile_definitions(Prism PRIVATE UNICODE _UNICODE)

# d3dcompiler is loaded at run time, so it is deliberately absent here: Prism
# still starts (with an unscaled copy blit) in a prefix that lacks it.
target_link_libraries(Prism PRIVATE d3d11 dxgi shell32 user32 gdi32 ole32)

if(MINGW)
    # Static runtime so Prism.exe drops into a Proton prefix without needing
    # libstdc++/libgcc DLLs alongside ReShade.
    target_link_options(Prism PRIVATE -static -static-libgcc -static-libstdc++ -municode)
endif()

set_target_properties(Prism PROPERTIES
    OUTPUT_NAME "Prism"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
