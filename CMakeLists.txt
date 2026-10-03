cmake_minimum_required(VERSION 3.15)

project(LucCompiler LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# COMPILER & RUNTIME SETTINGS
# =============================================================================

# Force a single, consistent runtime library across the whole build.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)

# Default to Release if the caller didn't pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

# =============================================================================
# SHARED COMPILE OPTIONS
# =============================================================================

set(LUCID_CXX_WARNINGS
    # Add warning flags here as the project grows.
    # Example: $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

set(LUCID_MSVC_FLAGS
    /FS        # force sequential PDB writes
    /MP        # multi-processor compilation
)

# =============================================================================
# LIBRARY: core
# =============================================================================

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/src/core/*.cpp"
)

add_library(lucid_core STATIC ${CORE_SOURCES})

target_include_directories(lucid_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/core/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/core/src"
)

target_compile_options(lucid_core PRIVATE ${LUCID_CXX_WARNINGS})
if(MSVC)
    target_compile_options(lucid_core PRIVATE ${LUCID_MSVC_FLAGS})
endif()

# =============================================================================
# LIBRARY: parser
# =============================================================================

file(GLOB_RECURSE PARSER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/parser/src/parser/*.cpp"
)

add_library(lucid_parser STATIC ${PARSER_SOURCES})

target_include_directories(lucid_parser
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/parser/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/parser/src"
)

target_link_libraries(lucid_parser PUBLIC lucid_core)

target_compile_options(lucid_parser PRIVATE ${LUCID_CXX_WARNINGS})
if(MSVC)
    target_compile_options(lucid_parser PRIVATE ${LUCID_MSVC_FLAGS})
endif()

# =============================================================================
# LIBRARY: sema
# =============================================================================

file(GLOB_RECURSE SEMA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/sema/src/sema/*.cpp"
)

add_library(lucid_sema STATIC ${SEMA_SOURCES})

target_include_directories(lucid_sema
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/sema/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/sema/src"
)

target_link_libraries(lucid_sema PUBLIC lucid_core lucid_parser)

target_compile_options(lucid_sema PRIVATE ${LUCID_CXX_WARNINGS})
if(MSVC)
    target_compile_options(lucid_sema PRIVATE ${LUCID_MSVC_FLAGS})
endif()

# =============================================================================
# LIBRARY: bytecode
# =============================================================================

file(GLOB_RECURSE BYTECODE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/bytecode/src/bytecode/*.cpp"
)

add_library(lucid_bytecode STATIC ${BYTECODE_SOURCES})

target_include_directories(lucid_bytecode
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/bytecode/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/bytecode/src"
)

target_link_libraries(lucid_bytecode PUBLIC lucid_core lucid_parser lucid_sema)

target_compile_options(lucid_bytecode PRIVATE ${LUCID_CXX_WARNINGS})
if(MSVC)
    target_compile_options(lucid_bytecode PRIVATE ${LUCID_MSVC_FLAGS})
endif()

# =============================================================================
# LIBRARY: cli
# =============================================================================

file(GLOB_RECURSE CLI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cli/src/cli/*.cpp"
)

add_library(lucid_cli STATIC ${CLI_SOURCES})

target_include_directories(lucid_cli
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/cli/src"
)

target_link_libraries(lucid_cli PUBLIC
    lucid_core
    lucid_parser
    lucid_sema
    lucid_bytecode
)

target_compile_options(lucid_cli PRIVATE ${LUCID_CXX_WARNINGS})
if(MSVC)
    target_compile_options(lucid_cli PRIVATE ${LUCID_MSVC_FLAGS})
endif()

# =============================================================================
# EXECUTABLE: lucid-comp
# =============================================================================

add_executable(lucid-comp
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)

target_link_libraries(lucid-comp PRIVATE
    lucid_cli
    lucid_bytecode
    lucid_sema
    lucid_parser
    lucid_core
)

if(MSVC)
    set_property(TARGET lucid-comp APPEND_STRING PROPERTY LINK_FLAGS " /ignore:4199")
    target_compile_options(lucid-comp PRIVATE /FS /MP)
endif()

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
message(STATUS "Libraries:")
message(STATUS "  lucid_core      (core/)")
message(STATUS "  lucid_parser    (parser/)")
message(STATUS "  lucid_sema      (sema/)")
message(STATUS "  lucid_bytecode  (bytecode/)")
message(STATUS "  lucid_cli       (cli/)")
message(STATUS "  lucid-comp      (src/main.cpp)")
message(STATUS "========================================")