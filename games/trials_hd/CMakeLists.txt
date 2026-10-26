# trials_hd - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(trials_hd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(TRIALSHD_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(trials_hd WIN32 ${TRIALSHD_SOURCES})
else()
    add_executable(trials_hd ${TRIALSHD_SOURCES})
endif()

rexglue_setup_target(trials_hd GPU_PLUGINS xenos)

if(WIN32)
    # A linker map for decoding crash reports.
    target_link_options(trials_hd PRIVATE "LINKER:/MAP")
endif()

# Private developer aids (dev/, never in the repo): screenshots and a scripted controller for tests.
option(TH_DEV_TOOLS "Build the developer aids in dev/" OFF)
if(TH_DEV_TOOLS)
    target_sources(trials_hd PRIVATE dev/dev_tools.cpp dev/dev_pad.cpp dev/dev_input_hooks.cpp)
    target_compile_definitions(trials_hd PRIVATE TH_DEV_TOOLS=1)
endif()
