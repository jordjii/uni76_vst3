# ==============================================================================
#  UNI 76 (Nostalgia Audio) - root CMakeLists.txt
#
#  This file is the single source of truth for the project build. Projucer is
#  not used and must not be reintroduced.
# ==============================================================================

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PluginIdentity.cmake")

project(UNI76 VERSION ${UNI76_VERSION} LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
    # Universal binary by default on macOS: Intel + Apple Silicon.
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS" FORCE)
endif()

option(UNI76_BUILD_TESTS "Build the UNI 76 automated test suite" ON)

# Developer-only offline license keygen (Tools/LicenseKeygen) - never part
# of the shipped plugin, so OFF by default; a normal plugin/test build
# never needs it. See docs/LICENSING.md.
option(UNI76_BUILD_TOOLS "Build UNI 76 developer tools (e.g. the license keygen)" OFF)

# ------------------------------------------------------------------------------
# JUCE - fetched at a pinned commit so every developer and CI machine builds
# against an identical framework revision. See cmake/PluginIdentity.cmake.
# ------------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY "${UNI76_JUCE_GIT_REPOSITORY}"
    GIT_TAG        "${UNI76_JUCE_GIT_TAG}"
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

add_subdirectory(Source/Plugin)

if(UNI76_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

if(UNI76_BUILD_TOOLS)
    add_subdirectory(Tools)
endif()
