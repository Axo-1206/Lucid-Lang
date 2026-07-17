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
# DEBUG BUILD COMMANDS FOR LucCompiler
# =============================================================================
#
# BASIC USAGE:
#   cmake -B build                                          # Configure with defaults (debug OFF)
#   cmake --build build                                     # Build the project
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid    # Run compiler
#
# =============================================================================
# DEBUG FLAGS (enable specific components)
# =============================================================================
#
# Enable ALL debug logging (master switch):
#   cmake -B build -DDEBUG_MASTER=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Enable ONLY parser debugging:
#   cmake -B build -DDEBUG_PARSER=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Enable ONLY semantic analysis debugging:
#   cmake -B build -DDEBUG_SEMANTIC=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Enable ONLY type parsing debugging:
#   cmake -B build -DDEBUG_TYPE=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Enable ONLY lexer debugging:
#   cmake -B build -DDEBUG_LEXER=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Enable ONLY interpreter debugging:
#   cmake -B build -DDEBUG_INTERPRETER=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Enable ONLY codegen debugging (when implemented):
#   cmake -B build -DDEBUG_CODEGEN=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Enable multiple components (parser + semantic):
#   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_SEMANTIC=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# =============================================================================
# VERBOSITY LEVELS (controls output detail)
# =============================================================================
#
# Available levels: 0=MINIMAL, 1=NORMAL (default), 2=DETAIL
#
# Minimal verbosity (major events only):
#   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_VERBOSITY=0
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Normal verbosity (default):
#   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_VERBOSITY=1
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Detail verbosity (verbose trace):
#   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_VERBOSITY=2
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# =============================================================================
# OUTPUT TO FILE (instead of console)
# =============================================================================
#
# Log to default file (debug.log):
#   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_TO_FILE=ON
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#   type debug.log  # View the log
#
# Log to custom file path:
#   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_TO_FILE=ON -DDEBUG_FILE_PATH="parser_output.log"
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#   type parser_output.log
#
# =============================================================================
# COMBINED EXAMPLES (most common debugging scenarios)
# =============================================================================
#
# Scenario 1: Debug parser with normal verbosity (most common for parser issues):
#   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_VERBOSITY=1
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Scenario 2: Debug parser with detail output (for complex parsing issues):
#   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_VERBOSITY=2
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Scenario 3: Debug type parsing and save to file:
#   cmake -B build -DDEBUG_TYPE=ON -DDEBUG_TO_FILE=ON -DDEBUG_FILE_PATH="type_debug.log"
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#   notepad type_debug.log
#
# Scenario 4: Debug semantic analysis with detail output:
#   cmake -B build -DDEBUG_SEMANTIC=ON -DDEBUG_VERBOSITY=2
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Scenario 5: Debug everything (master) with normal verbosity:
#   cmake -B build -DDEBUG_MASTER=ON -DDEBUG_VERBOSITY=1
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid
#
# Scenario 6: Debug everything with detail verbosity (use for hard-to-find bugs):
#   cmake -B build -DDEBUG_MASTER=ON -DDEBUG_VERBOSITY=2
#   cmake --build build
#   .\build\Debug\lucid-comp.exe tests\attribute_test.lucid 2>&1 | tee full_debug.log
#
# =============================================================================
# CLEAN BUILD (remove cache and rebuild from scratch)
# =============================================================================
#
# Windows:
#   rmdir /s /q build
#   cmake -B build -DDEBUG_PARSER=ON
#   cmake --build build
#
# Linux/Mac:
#   rm -rf build
#   cmake -B build -DDEBUG_PARSER=ON
#   cmake --build build
#
# =============================================================================
# RELEASE BUILD (no debug logging, optimized)
# =============================================================================
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   .\build\Release\lucid-comp.exe tests\attribute_test.lucid
#
# =============================================================================
# QUICK REFERENCE CARD
# =============================================================================
#
# | Flag                          | Purpose                          | Default |
# |-------------------------------|----------------------------------|---------|
# | -DDEBUG_MASTER=ON             | Enable ALL debug logging         | OFF     |
# | -DDEBUG_LEXER=ON              | Enable lexer logging             | OFF     |
# | -DDEBUG_PARSER=ON             | Enable parser logging            | OFF     |
# | -DDEBUG_TYPE=ON               | Enable type parsing logging      | OFF     |
# | -DDEBUG_SEMANTIC=ON           | Enable semantic analysis logging | OFF     |
# | -DDEBUG_INTERPRETER=ON        | Enable interpreter logging       | OFF     |
# | -DDEBUG_CODEGEN=ON            | Enable codegen logging           | OFF     |
# | -DDEBUG_VERBOSITY=LEVEL       | Output detail (0=MINIMAL,        | 1       |
# |                               | 1=NORMAL, 2=DETAIL)              |         |
# | -DDEBUG_TO_FILE=ON            | Write logs to file               | OFF     |
# | -DDEBUG_FILE_PATH=path        | Custom log file path             | debug.log|
# | -DCMAKE_BUILD_TYPE=Release    | Release build (no debug symbols) | Debug   |
# | -DCMAKE_BUILD_TYPE=Debug      | Debug build (with symbols)       | Debug   |
#
# =============================================================================
# TROUBLESHOOTING
# =============================================================================
#
# If debug output doesn't appear:
#   1. Verify CMake cache: cmake -B build -L | findstr DEBUG
#   2. Clean rebuild: rmdir /s /q build && cmake -B build -DDEBUG_PARSER=ON
#   3. Check build type: Should be Debug (not Release)
#   4. Verify macro is defined: Look for "DEBUG_* is DEFINED" during compilation
#
# If compilation errors occur:
#   1. Update vcpkg: cd C:/Users/TaiAx/vcpkg && git pull && .\bootstrap-vcpkg.bat
#   2. Reinstall LLVM: .\vcpkg install llvm:x64-windows
#   3. Clear CMake cache and reconfigure
#
# =============================================================================

# =============================================================================
# DEBUG LOGGING OPTIONS
# =============================================================================

# Master debug switch - enables all debug logging
option(DEBUG_MASTER "Enable all debug logging (overrides individual flags)" OFF)

# Component-specific debug flags (only take effect if DEBUG_MASTER is OFF)
option(DEBUG_LEXER       "Enable lexer debug logging" OFF)
option(DEBUG_PARSER      "Enable parser debug logging" OFF)
option(DEBUG_TYPE        "Enable type parsing debug logging" OFF)
option(DEBUG_SEMANTIC    "Enable semantic analysis debug logging" OFF)
option(DEBUG_INTERPRETER "Enable interpreter debug logging" OFF)
option(DEBUG_CODEGEN     "Enable code generation debug logging" OFF)

# Verbosity levels (numeric: 0=MINIMAL, 1=NORMAL, 2=DETAIL)
set(DEBUG_VERBOSITY 1 CACHE STRING "Debug verbosity: 0=MINIMAL, 1=NORMAL, 2=DETAIL")
set_property(CACHE DEBUG_VERBOSITY PROPERTY STRINGS 0 1 2)

# Option to output debug logs to file instead of stdout
option(DEBUG_TO_FILE "Write debug logs to file instead of stdout" OFF)
set(DEBUG_FILE_PATH "debug.log" CACHE STRING "Path to debug log file when DEBUG_TO_FILE is ON")

# Print summary of debug configuration
message(STATUS "========================================")
message(STATUS "Debug Configuration:")
message(STATUS "  Master: ${DEBUG_MASTER}")

if(NOT DEBUG_MASTER)
    message(STATUS "  Lexer:       ${DEBUG_LEXER}")
    message(STATUS "  Parser:      ${DEBUG_PARSER}")
    message(STATUS "  Type:        ${DEBUG_TYPE}")
    message(STATUS "  Semantic:    ${DEBUG_SEMANTIC}")
    message(STATUS "  Interpreter: ${DEBUG_INTERPRETER}")
    message(STATUS "  CodeGen:     ${DEBUG_CODEGEN}")
endif()

message(STATUS "  Verbosity:   ${DEBUG_VERBOSITY} (0=MINIMAL, 1=NORMAL, 2=DETAIL)")
message(STATUS "  Log to file: ${DEBUG_TO_FILE}")
if(DEBUG_TO_FILE)
    message(STATUS "  Log file:   ${DEBUG_FILE_PATH}")
endif()
message(STATUS "========================================")

# Build the debug definitions string
set(DEBUG_DEFINITIONS "")

if(DEBUG_MASTER)
    # Master debug enables everything
    list(APPEND DEBUG_DEFINITIONS "DEBUG_MASTER=1")
    list(APPEND DEBUG_DEFINITIONS "DEBUG_LEXER=1")
    list(APPEND DEBUG_DEFINITIONS "DEBUG_PARSER=1")
    list(APPEND DEBUG_DEFINITIONS "DEBUG_TYPE=1")
    list(APPEND DEBUG_DEFINITIONS "DEBUG_SEMANTIC=1")
    list(APPEND DEBUG_DEFINITIONS "DEBUG_INTERPRETER=1")
    list(APPEND DEBUG_DEFINITIONS "DEBUG_CODEGEN=1")
else()
    # Individual flags
    if(DEBUG_LEXER)
        list(APPEND DEBUG_DEFINITIONS "DEBUG_LEXER=1")
    endif()
    if(DEBUG_PARSER)
        list(APPEND DEBUG_DEFINITIONS "DEBUG_PARSER=1")
    endif()
    if(DEBUG_TYPE)
        list(APPEND DEBUG_DEFINITIONS "DEBUG_TYPE=1")
    endif()
    if(DEBUG_SEMANTIC)
        list(APPEND DEBUG_DEFINITIONS "DEBUG_SEMANTIC=1")
    endif()
    if(DEBUG_INTERPRETER)
        list(APPEND DEBUG_DEFINITIONS "DEBUG_INTERPRETER=1")
    endif()
    if(DEBUG_CODEGEN)
        list(APPEND DEBUG_DEFINITIONS "DEBUG_CODEGEN=1")
    endif()
endif()

# Add verbosity level 
list(APPEND DEBUG_DEFINITIONS "DEBUG_VERBOSITY=${DEBUG_VERBOSITY}")

# Add file logging option
if(DEBUG_TO_FILE)
    list(APPEND DEBUG_DEFINITIONS "DEBUG_TO_FILE=1")
    string(REPLACE "\\" "\\\\" DEBUG_FILE_PATH_ESCAPED "${DEBUG_FILE_PATH}")
    list(APPEND DEBUG_DEFINITIONS "DEBUG_FILE_PATH=\"${DEBUG_FILE_PATH_ESCAPED}\"")
endif()

# Apply definitions only if at least one debug option is enabled
if(DEBUG_DEFINITIONS)
    target_compile_definitions(lucid-comp PRIVATE ${DEBUG_DEFINITIONS})
    message(STATUS "Debug definitions applied: ${DEBUG_DEFINITIONS}")
endif()

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
message(STATUS "")
message(STATUS "Debug Flags:")
if(DEBUG_DEFINITIONS)
    foreach(def ${DEBUG_DEFINITIONS})
        message(STATUS "  ${def}")
    endforeach()
else()
    message(STATUS "  None enabled (release build)")
endif()
message(STATUS "========================================")
