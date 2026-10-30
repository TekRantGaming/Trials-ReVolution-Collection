# trials_fusion - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(trials_fusion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
