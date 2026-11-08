# trials_fusion - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(trials_fusion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK tools/setup_sdk.sh prepares (Linux and Android); pass -DREXSDK_DIR=... to use another checkout.
if(NOT WIN32 AND NOT REXSDK_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/rexglue-sdk/CMakeLists.txt")
    get_filename_component(_trials_sdk "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/rexglue-sdk" ABSOLUTE)
    set(REXSDK_DIR "${_trials_sdk}" CACHE PATH "Path to rexglue-sdk source tree")
endif()

include(generated/rexglue.cmake)

# Sources
# Code the three games share (games/shared/src): achievement notifications, the way back to the launcher, the
# render quality modes, the game window, the FPE guard, the achievement list export.
set(TRIALSFUSION_SOURCES
    src/main.cpp
    ../shared/src/fpe_guard.cpp
    src/port_defaults.cpp
    ../shared/src/achievements_export.cpp
    src/input_options.cpp
    src/frame_stats.cpp
    src/draw_distance.cpp
    ../shared/src/achievement_toast.cpp
    ../shared/src/return_to_launcher.cpp
    ../shared/src/render_quality.cpp
    ../shared/src/game_window.cpp
    ../shared/src/frame_limit.cpp
)

if(WIN32)
    add_executable(trials_fusion WIN32 ${TRIALSFUSION_SOURCES})
else()
    add_executable(trials_fusion ${TRIALSFUSION_SOURCES})
endif()

rexglue_setup_target(trials_fusion GPU_PLUGINS xenos)

# Native renderer (docs/native_port.md), as in tools/android/CMakeLists.txt. Needs out/native from tools/native/build_shaders.sh.
if(NOT WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../out/native/shader_cache.cpp")
    get_filename_component(_trials_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set(_native "${_trials_root}/games/shared/native_gpu")
    add_subdirectory("${_trials_root}/third_party/plume" plume)
    target_sources(trials_fusion PRIVATE
        "${_native}/native_gpu.cpp"
        "${_native}/live_commands.cpp"
        "${_native}/renderer.cpp"
        "${_trials_root}/third_party/smol-v/smolv.cpp"
        "${_trials_root}/out/native/shader_cache.cpp"
        "${_trials_root}/out/native/shader_containers.cpp")
    set_source_files_properties("${_trials_root}/out/native/shader_cache.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${_native}")
    target_compile_definitions(trials_fusion PRIVATE TRIALS_NATIVE_GPU=1)
    target_include_directories(trials_fusion PRIVATE "${_native}" "${_trials_root}/third_party/plume"
        "${_trials_root}/third_party/smol-v")
    target_link_libraries(trials_fusion PRIVATE plume X11)
endif()

# Linux: the SDK writes librexruntime into its own folder, and the game looks next to itself.
if(NOT WIN32 AND TARGET rexruntime)
    add_custom_command(TARGET trials_fusion POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rexruntime> $<TARGET_FILE_DIR:trials_fusion>
        VERBATIM)
endif()

if(WIN32)
    # UTF-8 as the program's code page, and a linker map for decoding crash reports.
    target_link_options(trials_fusion PRIVATE "LINKER:/MANIFEST:EMBED" "LINKER:/MANIFESTUAC:NO"
                        "LINKER:/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/trials_fusion.manifest"
                        "LINKER:/MAP")
endif()

target_compile_definitions(trials_fusion PRIVATE TRIALS_DEFAULT_720P=1)

# Private developer aids (dev/, never in the repo): screenshots and a scripted controller for tests.
option(TF_DEV_TOOLS "Build the developer aids in dev/" OFF)
if(TF_DEV_TOOLS)
    target_sources(trials_fusion PRIVATE dev/dev_tools.cpp dev/dev_pad.cpp)
    target_compile_definitions(trials_fusion PRIVATE TF_DEV_TOOLS=1)
endif()
