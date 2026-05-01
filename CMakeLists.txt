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
    src/diagnostics/DiagnosticEngine.cpp
    src/parser/Parser.cpp
    src/parser/ParserExpr.cpp
    src/parser/ParserDecl.cpp
    src/parser/ParserType.cpp
    src/parser/ParserStmt.cpp
    src/semantic/SymbolTable.cpp
    src/semantic/SemanticAnalyzer.cpp
    src/semantic/TypeResolver.cpp
    src/semantic/SemanticCollector.cpp
    src/semantic/SemanticDecl.cpp
    src/semantic/SemanticExpr.cpp
    src/semantic/SemanticStmt.cpp
    src/semantic/TypeChecker.cpp
    src/semantic/Annotator.cpp
    # src/codegen/CodeGen.cpp
    # src/codegen/CodeGenExpr.cpp
    # src/codegen/CodeGenDecl.cpp
    # src/codegen/CodeGenStmt.cpp
    # src/codegen/luc_runtime.c
)

target_compile_definitions(luc-comp PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(luc-comp PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${LLVM_INCLUDE_DIRS}
)
# target_link_libraries(luc-comp PRIVATE ZLIB::ZLIB)

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