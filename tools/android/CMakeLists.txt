# A Trials game for Android (arm64): builds libmain.so, which SDL's Java activity loads and enters through SDL_main.
# Uses the code the Linux build generated (games/<game>/generated), so it never runs the rexglue tool: that would be
# built for the device and could not run here. Build with tools/android/build.sh.

cmake_minimum_required(VERSION 3.25)
project(trials_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TRIALS_GAME "" CACHE STRING "trials_hd, trials_evolution or trials_fusion")
if(NOT TRIALS_GAME)
    message(FATAL_ERROR "Set TRIALS_GAME")
endif()

get_filename_component(TRIALS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GAME_DIR "${TRIALS_ROOT}/games/${TRIALS_GAME}")

# <storage>/<game> is the folder a player puts the game in (the SDK's FindAppRoot).
add_compile_definitions(REX_ANDROID_APP_FOLDER="${TRIALS_GAME}")
add_subdirectory("${TRIALS_ROOT}/third_party/rexglue-sdk" rexglue-sdk)

include("${GAME_DIR}/generated/default/sources.cmake")
set(GENERATED_DIR "${GAME_DIR}/generated/default")

add_library(game_recomp OBJECT ${GENERATED_SOURCES})
target_include_directories(game_recomp PRIVATE "${GAME_DIR}" "${GAME_DIR}/src" "${GENERATED_DIR}")
target_link_libraries(game_recomp PRIVATE rex::runtime)
target_compile_options(game_recomp PRIVATE -gline-tables-only)
target_precompile_headers(game_recomp PRIVATE "${GENERATED_DIR}/${TRIALS_GAME}_pch.h")
rexglue_apply_target_settings(game_recomp)
# The SDK's settings ask for -march=armv8-a; the flags from the toolchain win only if they come last.
target_compile_options(game_recomp PRIVATE -march=armv8.2-a)

set(SHARED "${TRIALS_ROOT}/games/shared/src")
set(GAME_SOURCES
    "${GAME_DIR}/src/main.cpp"
    "${GAME_DIR}/src/port_defaults.cpp"
    "${GAME_DIR}/src/input_options.cpp"
    "${GAME_DIR}/src/frame_stats.cpp"
    "${SHARED}/fpe_guard.cpp"
    "${SHARED}/achievements_export.cpp"
    "${SHARED}/achievement_toast.cpp"
    "${SHARED}/return_to_launcher.cpp"
    "${SHARED}/render_quality.cpp"
    "${SHARED}/game_window.cpp"
    "${SHARED}/android_gpu.cpp")
if(TRIALS_GAME STREQUAL "trials_hd")
    list(APPEND GAME_SOURCES "${GAME_DIR}/src/fps_unlock.cpp" "${GAME_DIR}/src/dof_option.cpp")
else()
    list(APPEND GAME_SOURCES "${GAME_DIR}/src/draw_distance.cpp" "${SHARED}/frame_limit.cpp")
endif()

add_library(main SHARED ${GAME_SOURCES})
target_include_directories(main PRIVATE "${GAME_DIR}" "${GAME_DIR}/src" "${GENERATED_DIR}")
target_link_libraries(main PRIVATE game_recomp rex::runtime)
rexglue_apply_target_settings(main)
target_compile_options(main PRIVATE -march=armv8.2-a)
rexglue_configure_target(main GPU_PLUGINS xenos)
if(NOT TRIALS_GAME STREQUAL "trials_hd")
    target_compile_definitions(main PRIVATE TRIALS_DEFAULT_720P=1)
endif()
if(TRIALS_GAME STREQUAL "trials_evolution")
    target_compile_definitions(main PRIVATE TRIALS_EXACT_RENDER_TARGETS=1)
endif()

# Custom Vulkan drivers (Turnip, newer Qualcomm drivers) through adrenotools: SDL_main loads one from the game folder's
# driver/ when present; the hook libraries ship in the APK next to libmain.so.
add_subdirectory("${TRIALS_ROOT}/third_party/libadrenotools" adrenotools)
target_link_libraries(main PRIVATE adrenotools)
target_compile_definitions(main PRIVATE REX_HAS_ADRENOTOOLS=1)

# Native renderer (docs/native_port.md). Needs out/native from tools/native/build_shaders.sh.
if(TRIALS_GAME STREQUAL "trials_fusion" AND EXISTS "${TRIALS_ROOT}/out/native/shader_cache.cpp")
    add_subdirectory("${TRIALS_ROOT}/third_party/plume" plume)
    set(NATIVE "${TRIALS_ROOT}/games/shared/native_gpu")
    target_sources(main PRIVATE
        "${NATIVE}/native_gpu.cpp"
        "${NATIVE}/live_commands.cpp"
        "${NATIVE}/renderer.cpp"
        "${TRIALS_ROOT}/third_party/smol-v/smolv.cpp"
        "${TRIALS_ROOT}/out/native/shader_cache.cpp"
        "${TRIALS_ROOT}/out/native/shader_containers.cpp")
    set_source_files_properties("${TRIALS_ROOT}/out/native/shader_cache.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${NATIVE}")
    target_compile_definitions(main PRIVATE TRIALS_NATIVE_GPU=1)
    target_include_directories(main PRIVATE "${NATIVE}" "${TRIALS_ROOT}/third_party/plume"
        "${TRIALS_ROOT}/third_party/smol-v")
    target_link_libraries(main PRIVATE plume)
endif()
add_dependencies(main main_hook hook_impl file_redirect_hook gsl_alloc_hook)
