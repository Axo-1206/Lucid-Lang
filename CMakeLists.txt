cmake_minimum_required(VERSION 3.15)

project(LucCompiler LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# VCPKG TOOLCHAIN CONFIGURATION
# =============================================================================
#
# The vcpkg toolchain is required for finding packages installed via vcpkg
# (like LLVM). This configuration uses a hardcoded default path with
# environment variable override support.
#
# ─── HOW IT WORKS ────────────────────────────────────────────────────────────
#
#   1. DEFAULT (Hardcoded Path):
#      Uses "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" - works for most
#      Windows developers who installed vcpkg to the default location.
#
#   2. ENVIRONMENT VARIABLE OVERRIDE:
#      Set VCPKG_TOOLCHAIN environment variable to your custom path.
#      Example: $env:VCPKG_TOOLCHAIN = "D:/my_vcpkg/scripts/buildsystems/vcpkg.cmake"
#
#   3. COMMAND LINE OVERRIDE:
#      Pass -DVCPKG_TOOLCHAIN_PATH=... when running CMake.
#      Example: cmake -B build -S . -DVCPKG_TOOLCHAIN_PATH="D:/vcpkg/..."
#
# ─── FOR CONTRIBUTORS ──────────────────────────────────────────────────────
#
#   If you have vcpkg installed in a non-default location, set the
#   environment variable before running CMake:
#
#   Windows PowerShell:
#     $env:VCPKG_TOOLCHAIN = "D:/your_vcpkg_path/scripts/buildsystems/vcpkg.cmake"
#
#   Windows Command Prompt:
#     set VCPKG_TOOLCHAIN=D:/your_vcpkg_path/scripts/buildsystems/vcpkg.cmake
#
#   Linux/macOS:
#     export VCPKG_TOOLCHAIN="/home/user/vcpkg/scripts/buildsystems/vcpkg.cmake"
#
# =============================================================================

# Default vcpkg path (works for most Windows setups with default vcpkg install)
set(VCPKG_DEFAULT_PATH "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")

# Check for environment variable override first
if(DEFINED ENV{VCPKG_TOOLCHAIN})
    set(VCPKG_TOOLCHAIN_PATH "$ENV{VCPKG_TOOLCHAIN}")
    message(STATUS "Using vcpkg from environment variable: ${VCPKG_TOOLCHAIN_PATH}")
# Then check for command-line override
elseif(DEFINED VCPKG_TOOLCHAIN_PATH)
    # Already set via -D command line
    message(STATUS "Using vcpkg from command line: ${VCPKG_TOOLCHAIN_PATH}")
# Finally fall back to hardcoded default
elseif(EXISTS "${VCPKG_DEFAULT_PATH}")
    set(VCPKG_TOOLCHAIN_PATH "${VCPKG_DEFAULT_PATH}")
    message(STATUS "Using vcpkg default path: ${VCPKG_TOOLCHAIN_PATH}")
else()
    message(WARNING "vcpkg toolchain not found at ${VCPKG_DEFAULT_PATH}")
    message(WARNING "Set VCPKG_TOOLCHAIN_PATH or VCPKG_TOOLCHAIN environment variable")
    message(WARNING "If you don't use vcpkg, install LLVM separately and set LLVM_ROOT")
endif()

# Apply the toolchain if found
if(EXISTS "${VCPKG_TOOLCHAIN_PATH}")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_TOOLCHAIN_PATH}" CACHE STRING "vcpkg toolchain")
    message(STATUS "vcpkg toolchain configured: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# =============================================================================
# LLVM PATH CONFIGURATION
# =============================================================================
#
# LLVM is required for the backend. This configuration searches
# several common locations before falling back to standard CMake find_package.
#
# ─── SEARCH ORDER ──────────────────────────────────────────────────────────
#
#   1. Hardcoded paths (common vcpkg and standalone LLVM locations)
#   2. LLVM_ROOT environment variable (if set)
#   3. Standard CMake find_package (searches system paths)
#
# ─── ENVIRONMENT VARIABLES ────────────────────────────────────────────────
#
#   LLVM_ROOT - Point to your LLVM installation directory.
#   Example: $env:LLVM_ROOT = "C:/Program Files/LLVM"
#
# ─── FOR CONTRIBUTORS ──────────────────────────────────────────────────────
#
#   If you installed LLVM via vcpkg to a non-default location, set LLVM_ROOT:
#
#   Windows PowerShell:
#     $env:LLVM_ROOT = "C:/path/to/llvm"
#
#   Or if you downloaded LLVM directly from the website:
#     $env:LLVM_ROOT = "C:/Program Files/LLVM"
#
# =============================================================================

# Common paths where LLVM might be installed
set(LLVM_PATHS
    # vcpkg default installation
    "C:/vcpkg/installed/x64-windows/share/llvm"

    # Standalone LLVM installations
    "C:/Program Files/LLVM/lib/cmake/llvm"
    "C:/LLVM/lib/cmake/llvm"

    # User-specific installations
    "C:/Users/${USERNAME}/vcpkg/installed/x64-windows/share/llvm"
)

# Environment variable override for LLVM
if(DEFINED ENV{LLVM_ROOT})
    list(APPEND LLVM_PATHS "$ENV{LLVM_ROOT}/lib/cmake/llvm")
    message(STATUS "Added LLVM from environment variable: $ENV{LLVM_ROOT}")
endif()

# Try each path to find LLVMConfig.cmake
set(LLVM_FOUND_PATH "")
foreach(path ${LLVM_PATHS})
    if(EXISTS "${path}/LLVMConfig.cmake")
        set(LLVM_FOUND_PATH "${path}")
        message(STATUS "Found LLVM at: ${LLVM_FOUND_PATH}")
        break()
    endif()
endforeach()

# Add the found path to CMAKE_PREFIX_PATH
if(LLVM_FOUND_PATH)
    list(APPEND CMAKE_PREFIX_PATH "${LLVM_FOUND_PATH}")
else()
    message(STATUS "LLVM not found in hardcoded paths, using standard find_package")
    message(STATUS "If LLVM is installed in a non-standard location, set LLVM_ROOT")
endif()

# =============================================================================
# FIND PACKAGES
# =============================================================================

# ZLIB is required by LLVM's support library
find_package(ZLIB REQUIRED)
if(ZLIB_FOUND)
    message(STATUS "Found ZLIB: ${ZLIB_INCLUDE_DIRS}")
else()
    message(FATAL_ERROR "ZLIB not found - required by LLVM")
endif()

# Find LLVM - with vcpkg toolchain, this will find it automatically
# If using standalone LLVM, set LLVM_ROOT or ensure it's in CMAKE_PREFIX_PATH
find_package(LLVM REQUIRED CONFIG)

if(LLVM_FOUND)
    message(STATUS "========================================")
    message(STATUS "LLVM Configuration:")
    message(STATUS "  Version:     ${LLVM_PACKAGE_VERSION}")
    message(STATUS "  Include dir: ${LLVM_INCLUDE_DIRS}")
    message(STATUS "  Library dir: ${LLVM_LIBRARY_DIRS}")
    message(STATUS "  Targets:     ${LLVM_TARGETS_TO_BUILD}")
    message(STATUS "========================================")
else()
    message(FATAL_ERROR "LLVM not found! Please install LLVM or set LLVM_ROOT")
endif()

# =============================================================================
# PROJECT SETUP
# =============================================================================

add_definitions(${LLVM_DEFINITIONS})

# Automatically gather all .cpp files within src/ and its nested directories
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Create executable
add_executable(lucid-comp ${SOURCES})

# =============================================================================
# COMPILER & LINKER SETTINGS
# =============================================================================

target_compile_definitions(lucid-comp PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(lucid-comp PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/debug"
    ${LLVM_INCLUDE_DIRS}
)

# Map LLVM components to library names
# NOTE: If you add more LLVM components, list them here
llvm_map_components_to_libnames(llvm_libs 
    core 
    support 
    native 
    mc 
    target 
    orcjit 
    executionengine 
    mcjit
    ${LLVM_TARGETS_TO_BUILD}
)

# Windows-specific flags
if(MSVC)
    # Suppress LNK4199: /DELAYLOAD ignored on DLL that was not imported
    set_property(TARGET lucid-comp APPEND_STRING PROPERTY LINK_FLAGS " /ignore:4199")
    
    # Force sequential PDB writes for parallel builds (prevents PDB corruption)
    target_compile_options(lucid-comp PRIVATE /FS)
    
    # Enable multi-processor compilation for faster builds
    target_compile_options(lucid-comp PRIVATE /MP)
endif()

# Link everything
target_link_libraries(lucid-comp PRIVATE ${llvm_libs} ZLIB::ZLIB)

# =============================================================================
# FINAL CONFIGURATION SUMMARY
# =============================================================================

message(STATUS "")
message(STATUS "========================================")
message(STATUS "FINAL BUILD CONFIGURATION")
message(STATUS "========================================")
message(STATUS "Project:           ${PROJECT_NAME}")
message(STATUS "CMake version:     ${CMAKE_VERSION}")
message(STATUS "C++ Standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "Generator:         ${CMAKE_GENERATOR}")
message(STATUS "")
message(STATUS "LLVM:")
message(STATUS "  Version:         ${LLVM_PACKAGE_VERSION}")
message(STATUS "  Found at:        ${LLVM_FOUND_PATH}")
message(STATUS "  Include:         ${LLVM_INCLUDE_DIRS}")
message(STATUS "")
message(STATUS "vcpkg:")
message(STATUS "  Toolchain:       ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "========================================")