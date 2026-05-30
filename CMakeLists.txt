cmake_minimum_required(VERSION 3.15)

# Set the vcpkg toolchain file before project()
set(CMAKE_TOOLCHAIN_FILE "C:/Users/TaiAx/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "vcpkg toolchain")

project(LucCompiler LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add the specific share folder to the prefix path so find_package can see LLVMConfig.cmake
# list(APPEND CMAKE_PREFIX_PATH "C:/Users/TaiAx/vcpkg/installed/x64-windows/share/llvm")

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

# Add source files
add_executable(luc-comp 
    src/main.cpp 
    src/lexer/Lexer.cpp
    src/diagnostics/diagnostic.cpp
    src/diagnostics/DiagnosticMessages.cpp
    src/parser/Parser.cpp
    src/parser/ParserExpr.cpp
    src/parser/ParserDecl.cpp
    src/parser/ParserType.cpp
    src/parser/lookahead/ParserLookahead.cpp
    src/parser/ParserPattern.cpp
    src/semantic/SymbolTable.cpp
    src/semantic/SemanticAnalyzer.cpp
    src/semantic/TypeResolver.cpp
    src/semantic/SemanticCollector.cpp
    src/semantic/SemanticDecl.cpp
    src/semantic/SemanticExpr.cpp
    src/semantic/SemanticStmt.cpp
    src/semantic/TypeChecker.cpp
    src/semantic/Annotator.cpp
    src/debug/ASTDumper.cpp
    src/registry/QualifierRegistry.cpp
    src/registry/AttributeRegistry.cpp
    src/registry/IntrinsicRegistry.cpp
    src/ast/support/StringPool.cpp
    src/parser/top/PackageParser.cpp
    src/parser/top/UseParser.cpp
    src/parser/stmt/StmtParser.cpp
    src/parser/stmt/BlockParser.cpp
    src/parser/stmt/SwitchParser.cpp
    src/parser/stmt/LoopParser.cpp
    src/parser/stmt/FlowControlParser.cpp
    src/parser/stmt/LocalDeclParser.cpp
    src/parser/generic/GenericArgParser.cpp
    src/parser/generic/GenericParamParser.cpp
    src/parser/literal/LiteralParser.cpp
    # src/codegen/CodeGen.cpp
    # src/codegen/CodeGenExpr.cpp
    # src/codegen/CodeGenDecl.cpp
    # src/codegen/CodeGenStmt.cpp
    # src/codegen/luc_runtime.c
)

# =============================================================================
# DEBUG BUILD COMMANDS FOR LucCompiler
# =============================================================================
#
# BASIC USAGE:
#   cmake -B build                                          # Configure with defaults (debug OFF)
#   cmake --build build                                     # Build the project
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc    # Run compiler
#
# =============================================================================
# DEBUG FLAGS (enable specific components)
# =============================================================================
#
# Enable ALL debug logging (master switch):
#   cmake -B build -DLUC_DEBUG_MASTER=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Enable ONLY parser debugging:
#   cmake -B build -DLUC_DEBUG_PARSER=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Enable ONLY semantic analysis debugging:
#   cmake -B build -DLUC_DEBUG_SEMANTIC=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Enable ONLY type parsing debugging:
#   cmake -B build -DLUC_DEBUG_TYPE=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Enable ONLY lexer debugging:
#   cmake -B build -DLUC_DEBUG_LEXER=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Enable ONLY codegen debugging (when implemented):
#   cmake -B build -DLUC_DEBUG_CODEGEN=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Enable multiple components (parser + semantic):
#   cmake -B build -DLUC_DEBUG_PARSER=ON -DLUC_DEBUG_SEMANTIC=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# =============================================================================
# VERBOSITY LEVELS (controls output detail)
# =============================================================================
#
# Available levels: MINIMAL, NORMAL (default), VERBOSE, EXTREME
#
# Normal verbosity (default):
#   cmake -B build -DLUC_DEBUG_PARSER=ON -DLUC_DEBUG_VERBOSITY=NORMAL
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Verbose output (more details):
#   cmake -B build -DLUC_DEBUG_PARSER=ON -DLUC_DEBUG_VERBOSITY=VERBOSE
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Extreme verbosity (everything, including debug helpers):
#   cmake -B build -DLUC_DEBUG_PARSER=ON -DLUC_DEBUG_VERBOSITY=EXTREME
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# =============================================================================
# OUTPUT TO FILE (instead of console)
# =============================================================================
#
# Log to default file (debug.log):
#   cmake -B build -DLUC_DEBUG_PARSER=ON -DLUC_DEBUG_TO_FILE=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#   type debug.log  # View the log
#
# Log to custom file path:
#   cmake -B build -DLUC_DEBUG_PARSER=ON -DLUC_DEBUG_TO_FILE=ON -DLUC_DEBUG_FILE_PATH="parser_output.log"
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#   type parser_output.log
#
# =============================================================================
# COMBINED EXAMPLES (most common debugging scenarios)
# =============================================================================
#
# Scenario 1: Debug parser with normal verbosity (most common for parser issues):
#   cmake -B build -DLUC_DEBUG_PARSER=ON -DLUC_DEBUG_VERBOSITY=NORMAL
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Scenario 2: Debug parser with verbose output (for complex parsing issues):
#   cmake -B build -DLUC_DEBUG_PARSER=ON -DLUC_DEBUG_VERBOSITY=VERBOSE
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Scenario 3: Debug type parsing and save to file:
#   cmake -B build -DLUC_DEBUG_TYPE=ON -DLUC_DEBUG_TO_FILE=ON -DLUC_DEBUG_FILE_PATH="type_debug.log"
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#   notepad type_debug.log
#
# Scenario 4: Debug semantic analysis with verbose output:
#   cmake -B build -DLUC_DEBUG_SEMANTIC=ON -DLUC_DEBUG_VERBOSITY=VERBOSE
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Scenario 5: Debug everything (master) with normal verbosity:
#   cmake -B build -DLUC_DEBUG_MASTER=ON
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc
#
# Scenario 6: Debug everything with extreme verbosity (use for hard-to-find bugs):
#   cmake -B build -DLUC_DEBUG_MASTER=ON -DLUC_DEBUG_VERBOSITY=EXTREME
#   cmake --build build
#   .\build\Debug\luc-comp.exe tests\attribute_test.luc 2>&1 | tee full_debug.log
#
# =============================================================================
# CLEAN BUILD (remove cache and rebuild from scratch)
# =============================================================================
#
# Windows:
#   rmdir /s /q build
#   cmake -B build -DLUC_DEBUG_PARSER=ON
#   cmake --build build
#
# Linux/Mac:
#   rm -rf build
#   cmake -B build -DLUC_DEBUG_PARSER=ON
#   cmake --build build
#
# =============================================================================
# RELEASE BUILD (no debug logging, optimized)
# =============================================================================
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   .\build\Release\luc-comp.exe tests\attribute_test.luc
#
# =============================================================================
# QUICK REFERENCE CARD
# =============================================================================
#
# | Flag                          | Purpose                          | Default |
# |-------------------------------|----------------------------------|---------|
# | -DLUC_DEBUG_MASTER=ON         | Enable ALL debug logging         | OFF     |
# | -DLUC_DEBUG_LEXER=ON          | Enable lexer logging             | OFF     |
# | -DLUC_DEBUG_PARSER=ON         | Enable parser logging            | OFF     |
# | -DLUC_DEBUG_PARSE_RESULT=ON   | Enable parse result logging      | OFF     |
# | -DLUC_DEBUG_TYPE=ON           | Enable type parsing logging      | OFF     |
# | -DLUC_DEBUG_SEMANTIC=ON       | Enable semantic analysis logging | OFF     |
# | -DLUC_DEBUG_CODEGEN=ON        | Enable codegen logging           | OFF     |
# | -DLUC_DEBUG_VERBOSITY=LEVEL   | Output detail (MINIMAL/NORMAL/   | NORMAL  |
# |                               | VERBOSE/EXTREME)                 |         |
# | -DLUC_DEBUG_TO_FILE=ON        | Write logs to file               | OFF     |
# | -DLUC_DEBUG_FILE_PATH=path    | Custom log file path             | debug.log|
# | -DCMAKE_BUILD_TYPE=Release    | Release build (no debug symbols) | Debug   |
# | -DCMAKE_BUILD_TYPE=Debug      | Debug build (with symbols)       | Debug   |
#
# =============================================================================
# TROUBLESHOOTING
# =============================================================================
#
# If debug output doesn't appear:
#   1. Verify CMake cache: cmake -B build -L | findstr LUC_DEBUG
#   2. Clean rebuild: rmdir /s /q build && cmake -B build -DLUC_DEBUG_PARSER=ON
#   3. Check build type: Should be Debug (not Release)
#   4. Verify macro is defined: Look for "LUC_DEBUG_* is DEFINED" during compilation
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
option(LUC_DEBUG_MASTER "Enable all debug logging (overrides individual flags)" OFF)

# Component-specific debug flags (only take effect if LUC_DEBUG_MASTER is OFF)
option(LUC_DEBUG_LEXER   "Enable lexer debug logging" OFF)
option(LUC_DEBUG_PARSER  "Enable parser debug logging" OFF)
option(LUC_DEBUG_PARSE_RESULT "Enable parse result debug logging" OFF)
option(LUC_DEBUG_TYPE    "Enable type parsing debug logging" OFF)
option(LUC_DEBUG_SEMANTIC "Enable semantic analysis debug logging" OFF)
option(LUC_DEBUG_CODEGEN "Enable code generation debug logging" OFF)

# Verbosity levels (numeric: 0=MINIMAL, 1=NORMAL, 2=VERBOSE, 3=EXTREME)
set(LUC_DEBUG_VERBOSITY 1 CACHE STRING "Debug verbosity: 0=MINIMAL, 1=NORMAL, 2=VERBOSE, 3=EXTREME")
set_property(CACHE LUC_DEBUG_VERBOSITY PROPERTY STRINGS 0 1 2 3)

# Option to output debug logs to file instead of stdout
option(LUC_DEBUG_TO_FILE "Write debug logs to file instead of stdout" OFF)
set(LUC_DEBUG_FILE_PATH "debug.log" CACHE STRING "Path to debug log file when LUC_DEBUG_TO_FILE is ON")


# Print summary of debug configuration
message(STATUS "========================================")
message(STATUS "Debug Configuration:")
message(STATUS "  Master: ${LUC_DEBUG_MASTER}")

if(NOT LUC_DEBUG_MASTER)
    message(STATUS "  Lexer:   ${LUC_DEBUG_LEXER}")
    message(STATUS "  Parser:  ${LUC_DEBUG_PARSER}")
    message(STATUS "  Parse Result: ${LUC_DEBUG_PARSE_RESULT}")
    message(STATUS "  Type:    ${LUC_DEBUG_TYPE}")
    message(STATUS "  Semantic: ${LUC_DEBUG_SEMANTIC}")
    message(STATUS "  CodeGen: ${LUC_DEBUG_CODEGEN}")
endif()

message(STATUS "  Verbosity: ${LUC_DEBUG_VERBOSITY}")
message(STATUS "  Log to file: ${LUC_DEBUG_TO_FILE}")
if(LUC_DEBUG_TO_FILE)
    message(STATUS "DEBUG LOGGING TO FILE ENABLED")
    message(STATUS "Log file will be written to: ${LUC_DEBUG_FILE_PATH}")
    message(STATUS "Note: Path is relative to where the executable runs")
    message(STATUS "To get absolute path, use: -DLUC_DEBUG_FILE_PATH=\"C:/full/path/log.txt\"")
endif()
message(STATUS "========================================")

# Build the debug definitions string
set(LUC_DEBUG_DEFINITIONS "")

if(LUC_DEBUG_MASTER)
    # Master debug enables everything
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_MASTER=1")
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_LEXER=1")
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_PARSER=1")
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_PARSE_RESULT=1")
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_TYPE=1")
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_SEMANTIC=1")
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_CODEGEN=1")
else()
    # Individual flags
    if(LUC_DEBUG_LEXER)
        list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_LEXER=1")
    endif()
    if(LUC_DEBUG_PARSER)
        list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_PARSER=1")
    endif()
    if(LUC_DEBUG_PARSE_RESULT)
        list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_PARSE_RESULT=1")
    endif()
    if(LUC_DEBUG_TYPE)
        list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_TYPE=1")
    endif()
    if(LUC_DEBUG_SEMANTIC)
        list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_SEMANTIC=1")
    endif()
    if(LUC_DEBUG_CODEGEN)
        list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_CODEGEN=1")
    endif()
endif()

# Add verbosity level 
list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_VERBOSITY=${LUC_DEBUG_VERBOSITY}")

# Add file logging option
if(LUC_DEBUG_TO_FILE)
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_TO_FILE=1")
    string(REPLACE "\\" "\\\\" LUC_DEBUG_FILE_PATH_ESCAPED "${LUC_DEBUG_FILE_PATH}")
    list(APPEND LUC_DEBUG_DEFINITIONS "LUC_DEBUG_FILE_PATH=\"${LUC_DEBUG_FILE_PATH_ESCAPED}\"")
endif()

# Apply definitions only if at least one debug option is enabled
if(LUC_DEBUG_DEFINITIONS)
    target_compile_definitions(luc-comp PRIVATE ${LUC_DEBUG_DEFINITIONS})
    message(STATUS "Debug definitions applied: ${LUC_DEBUG_DEFINITIONS}")
endif()

target_compile_definitions(luc-comp PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(luc-comp PRIVATE 
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
    set_property(TARGET luc-comp APPEND_STRING PROPERTY LINK_FLAGS " /ignore:4199")
endif()

# Link everything
target_link_libraries(luc-comp PRIVATE ${llvm_libs} ZLIB::ZLIB)