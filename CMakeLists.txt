cmake_minimum_required(VERSION 3.15)

# Set the vcpkg toolchain file before project()
set(CMAKE_TOOLCHAIN_FILE "C:/Users/TaiAx/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "vcpkg toolchain")

project(LucCompiler LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add the specific share folder to the prefix path so find_package can see LLVMConfig.cmake
# list(APPEND CMAKE_PREFIX_PATH "C:/Users/TaiAx/vcpkg/installed/x64-windows/share/llvm)

# Add BOTH LLVM and ZLIB paths to the prefix path
list(APPEND CMAKE_PREFIX_PATH 
    "C:/Users/TaiAx/vcpkg/installed/x64-windows/share/llvm"
    "C:/Users/TaiAx/vcpkg/installed/x64-windows"
)

# This provides the ZLIB::ZLIB target that LLVMSupport needs
find_package(ZLIB REQUIRED)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)
message(STATUS "LLVM include dirs: ${LLVM_INCLUDE_DIRS}")

# Standard LLVM setup
add_definitions(${LLVM_DEFINITIONS})

# Automatically gather all .cpp files within src/ and its nested directories
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Create the executable using the automated source file list
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

target_compile_definitions(lucid-comp PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(lucid-comp PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/debug"
    ${LLVM_INCLUDE_DIRS}
)

# Map LLVM components
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

if(MSVC)
    # Existing flag
    set_property(TARGET lucid-comp APPEND_STRING PROPERTY LINK_FLAGS " /ignore:4199")
    
    # Force sequential PDB writes for parallel builds
    target_compile_options(lucid-comp PRIVATE /FS)
    
    # Optional: Enable multi-processor compilation
    target_compile_options(lucid-comp PRIVATE /MP)
endif()

# Link everything
target_link_libraries(lucid-comp PRIVATE ${llvm_libs} ZLIB::ZLIB)