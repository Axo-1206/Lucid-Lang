cmake_minimum_required(VERSION 3.15)
project(llvm_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Match the runtime library and configuration of the parent build ────
# This is the whole point of the test. If the parent project builds with
# MultiThreadedDLL and this one doesn't, the test tells you nothing.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

# ─── Find LLVM ─────────────────────────────────────────────────────────
# Inherit the same LLVM that the main project uses. Two ways to do this:
#
#   (a) If the parent project already ran find_package(LLVM) and set
#       LLVM_DIR in its cache, pass -DLLVM_DIR=<path> when configuring this
#       subproject. CMake will then reuse it.
#
#   (b) If you configure this subproject standalone, point LLVM_ROOT at
#       the same install the main project uses:
#
#           cmake -B build -S tools/llvm-smoke ^
#                 -DLLVM_ROOT=C:/vcpkg/installed/x64-windows ^
#                 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
#
# Either way, both projects end up linking the same .lib files.

if(NOT DEFINED LLVM_DIR AND DEFINED ENV{LLVM_ROOT})
    set(LLVM_DIR "$ENV{LLVM_ROOT}/share/llvm" CACHE PATH "" FORCE)
endif()

find_package(LLVM REQUIRED CONFIG)

message(STATUS "llvm-smoke: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
message(STATUS "llvm-smoke: includes ${LLVM_INCLUDE_DIRS}")
message(STATUS "llvm-smoke: libs     ${LLVM_LIBRARY_DIRS}")
message(STATUS "llvm-smoke: runtime  ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "llvm-smoke: config   ${CMAKE_BUILD_TYPE}")

add_executable(llvm_smoke main.cpp)

target_compile_definitions(llvm_smoke PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(llvm_smoke PRIVATE ${LLVM_INCLUDE_DIRS})

llvm_map_components_to_libnames(llvm_libs 
    core 
    support 
    native 
    mc 
    target 
    orcjit 
    executionengine 
    mcjit
    asmwriter
    asmparser
    ${LLVM_TARGETS_TO_BUILD}
)

target_link_libraries(llvm_smoke PRIVATE ${llvm_libs})