# Cross-compilation toolchain for Prism.exe: MinGW-w64, 64-bit Windows target.
# Used on Fedora with mingw64-gcc-c++, and on any distro that ships the same.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(PRISM_MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "MinGW-w64 target triple")

set(CMAKE_C_COMPILER   ${PRISM_MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${PRISM_MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${PRISM_MINGW_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${PRISM_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
