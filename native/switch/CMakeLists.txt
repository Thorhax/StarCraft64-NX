# Nintendo Switch (devkitA64 + libnx) profile. Milestone 1: game, runtime,
# renderer and input libraries only; the libnx shell follows.
cmake_minimum_required(VERSION 3.24)
project(sc64_switch LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# gnu++20: newlib hides POSIX helpers (fileno, fdopen, putc_unlocked) in strict ISO mode.
set(CMAKE_CXX_EXTENSIONS ON)
get_filename_component(SC64_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SC64_RUNTIME_DIR "${SC64_ROOT}/local/deps/N64ModernRuntime-switch" CACHE PATH "Switch runtime (tools/switch/prepare_runtime.py)")
set(SC64_GENERATED_DIR "${SC64_ROOT}/local/generated/game" CACHE PATH "Private game C")
if(NOT EXISTS "${SC64_RUNTIME_DIR}/sc64-shell-patch.json" OR
   NOT EXISTS "${SC64_GENERATED_DIR}/recomp_overlays.inl")
    message(FATAL_ERROR "Run tools/switch/prepare_runtime.py and prepare the private game first")
endif()
include("${SC64_ROOT}/cmake/runtime.cmake")
find_package(Threads REQUIRED)
add_compile_options(-UNDEBUG -ffunction-sections -fdata-sections
    "$<$<COMPILE_LANGUAGE:CXX>:-include>" "$<$<COMPILE_LANGUAGE:CXX>:cstdlib>")
sc64_add_runtime("${SC64_RUNTIME_DIR}" runtime EXCLUDE_FROM_ALL)
target_include_directories(librecomp PRIVATE "${SC64_ROOT}/native/runtime")
# fmt builds without GNU extensions; expose newlib POSIX stdio (fileno, fdopen, putc_unlocked).
target_compile_definitions(fmt PRIVATE _GNU_SOURCE)
# sljit (LiveRecomp, code mods only) needs <sys/mman.h>: use the failing shim.
target_include_directories(LiveRecomp BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_include_directories(ultramodern PRIVATE "${SC64_ROOT}/native/runtime" "${SC64_ROOT}/native/shell")

file(GLOB SC64_GAME_SOURCES CONFIGURE_DEPENDS "${SC64_GENERATED_DIR}/*.c")
add_library(sc64_game STATIC ${SC64_GAME_SOURCES})
target_include_directories(sc64_game PUBLIC "${SC64_GENERATED_DIR}" "${SC64_RUNTIME_DIR}/N64Recomp/include")
target_compile_options(sc64_game PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -fexceptions)

add_library(sc64_gfx_core STATIC
    ${SC64_ROOT}/native/gfx_core/interpreter.cpp ${SC64_ROOT}/native/gfx_core/tmem.cpp
    ${SC64_ROOT}/native/gfx_core/software.cpp)
target_include_directories(sc64_gfx_core PUBLIC ${SC64_ROOT}/native/gfx_core)
target_link_libraries(sc64_gfx_core PUBLIC Threads::Threads)
include("${SC64_ROOT}/cmake/gfx_optimize.cmake")
sc64_optimize_gfx_targets(sc64_gfx_core)

add_library(sc64_input STATIC
    ${SC64_ROOT}/native/shell/input/gestures.cpp ${SC64_ROOT}/native/shell/input/map_taps.cpp
    ${SC64_ROOT}/native/shell/input/minimap_input.cpp ${SC64_ROOT}/native/shell/input/hud_buttons.cpp
    ${SC64_ROOT}/native/shell/input/hud_input.cpp ${SC64_ROOT}/native/shell/input/ui_input.cpp
    ${SC64_ROOT}/native/shell/input/devices.cpp)
target_include_directories(sc64_input PUBLIC "${SC64_ROOT}/native/shell/input")
add_library(sc64_scripts STATIC "${SC64_ROOT}/native/shell/scripts.cpp")
target_include_directories(sc64_scripts PUBLIC "${SC64_ROOT}/native/shell" "${SC64_RUNTIME_DIR}/thirdparty")
target_link_libraries(sc64_scripts PUBLIC sc64_input)

  # Shell: the platform-neutral SDL3-shell sources plus the libnx app/platform.
  # miniaudio keeps only its null backend for now (silent, correctly paced).
  add_library(sc64_shell STATIC
      ${SC64_ROOT}/native/shell/session.cpp ${SC64_ROOT}/native/shell/audio.cpp
      ${SC64_ROOT}/native/shell/miniaudio.c ${SC64_ROOT}/native/shell/log.cpp
      ${SC64_ROOT}/native/shell/native.cpp
      ${SC64_ROOT}/native/runtime/flash_device.cpp ${SC64_ROOT}/native/runtime/ui_feedback.cpp
      ${SC64_ROOT}/native/runtime/sha.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/platform.cpp)
  target_include_directories(sc64_shell PUBLIC "${SC64_ROOT}/native/shell" "${SC64_ROOT}/native/runtime"
      "${SC64_ROOT}/local/toolchains")
  target_compile_definitions(sc64_shell PUBLIC SC64_SDL_VERSION="none"
      SC64_PRIVATE_ROOT="sdmc:/switch/StarCraft64"
      MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_NULL MA_NO_RUNTIME_LINKING)
  target_link_libraries(sc64_shell PUBLIC sc64_game sc64_gfx_core sc64_scripts librecomp ultramodern Threads::Threads)
# Voiced briefings (native/shell/voices.cpp).
target_sources(sc64_shell PRIVATE ${SC64_ROOT}/native/shell/voices.cpp)
target_compile_definitions(sc64_shell PRIVATE SC64_VOICES=1)
# PC movies (native/shell/movie.cpp) through the switch-ffmpeg portlibs.
find_library(SC64_AVFORMAT avformat PATHS /opt/devkitpro/portlibs/switch/lib)
if(SC64_AVFORMAT)
    target_sources(sc64_shell PRIVATE ${SC64_ROOT}/native/shell/movie.cpp)
    target_compile_definitions(sc64_shell PUBLIC SC64_MOVIES=1)
    target_include_directories(sc64_shell PRIVATE /opt/devkitpro/portlibs/switch/include)
    target_link_libraries(sc64_shell PUBLIC "${SC64_AVFORMAT}" avcodec swscale swresample avutil dav1d bz2 z)
    target_link_directories(sc64_shell PUBLIC /opt/devkitpro/portlibs/switch/lib)
    message(STATUS "SC64 Switch: FFmpeg movies enabled")
endif()

  add_executable(sc64_switch ${CMAKE_CURRENT_SOURCE_DIR}/app.cpp)
  find_library(SC64_LIBNX nx PATHS /opt/devkitpro/libnx/lib "$ENV{DEVKITPRO}/libnx/lib" REQUIRED)
  target_include_directories(sc64_switch PRIVATE /opt/devkitpro/libnx/include)
  target_link_libraries(sc64_switch PRIVATE sc64_shell "${SC64_LIBNX}")
  target_link_options(sc64_switch PRIVATE -Wl,--gc-sections -Wl,--wrap=pthread_create)
# Mesa for Horizon (tools/switch/import_portlibs.sh, mounted by build.sh): EGL +
# OpenGL (Zink on loaderless NVK). Optional: without it the build stays
# software-only with libnx framebuffer presentation.
set(SC64_SWITCH_GL_CONFIG "/opt/devkitpro/portlibs/switch/lib/cmake/OpenGL")
if(EXISTS "${SC64_SWITCH_GL_CONFIG}/OpenGLConfig.cmake")
    set(OpenGL_DIR "${SC64_SWITCH_GL_CONFIG}")
    find_package(OpenGL CONFIG REQUIRED)
    target_link_libraries(sc64_switch PRIVATE OpenGL::GL OpenGL::EGL)
    target_compile_definitions(sc64_switch PRIVATE SC64_SWITCH_GL=1)
    target_sources(sc64_switch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gl_video.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/gl_device.cpp ${CMAKE_CURRENT_SOURCE_DIR}/gl_raster.cpp)
    message(STATUS "SC64 Switch: Mesa EGL/OpenGL enabled")
else()
    message(STATUS "SC64 Switch: Mesa portlibs not found, software presentation only")
endif()
  set_target_properties(sc64_switch PROPERTIES OUTPUT_NAME StarCraft64)
  # NRO packaging with the devkitPro tools (nacptool, elf2nro).
  find_program(SC64_NACPTOOL nacptool PATHS /opt/devkitpro/tools/bin REQUIRED)
  find_program(SC64_ELF2NRO elf2nro PATHS /opt/devkitpro/tools/bin REQUIRED)
  set(SC64_NACP "${CMAKE_CURRENT_BINARY_DIR}/StarCraft64.nacp")
set(SC64_SWITCH_VERSION "1.3.0")
set(SC64_SWITCH_ICON "${SC64_ROOT}/local/icons/switch-icon.jpg")
set(SC64_ICON_ARGUMENT "")
if(EXISTS "${SC64_SWITCH_ICON}")
    set(SC64_ICON_ARGUMENT "--icon=${SC64_SWITCH_ICON}")
endif()
  set(SC64_NRO "${CMAKE_CURRENT_BINARY_DIR}/StarCraft64.nro")
  add_custom_command(TARGET sc64_switch POST_BUILD
      COMMAND "${SC64_NACPTOOL}" --create "StarCraft 64" "gabryboy12-ai" "${SC64_SWITCH_VERSION}" "${SC64_NACP}"
      COMMAND "${SC64_ELF2NRO}" "$<TARGET_FILE:sc64_switch>" "${SC64_NRO}" "--nacp=${SC64_NACP}" ${SC64_ICON_ARGUMENT}
      COMMENT "Packaging ${SC64_NRO}" VERBATIM)
