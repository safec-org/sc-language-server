cmake_minimum_required(VERSION 3.20)
project(sc-language-server VERSION 1.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SAFEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SafeC/compiler"
    CACHE PATH "SafeC compiler source directory")
set(SAFEGUARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SafeC/safeguard"
    CACHE PATH "safeguard source directory (for the .scx transpiler)")

# ── SafeC frontend static library (no LLVM needed) ──────────────────────────
set(SAFEC_FRONTEND_SOURCES
    ${SAFEC_DIR}/src/Preprocessor.cpp
    # CHeaderImporter replaced by stub — the real impl pulls in LLVM headers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/CHeaderImporterStub.cpp
    ${SAFEC_DIR}/src/Lexer.cpp
    ${SAFEC_DIR}/src/AST.cpp
    ${SAFEC_DIR}/src/Type.cpp
    ${SAFEC_DIR}/src/Parser.cpp
    ${SAFEC_DIR}/src/Sema.cpp
    ${SAFEC_DIR}/src/Clone.cpp
    ${SAFEC_DIR}/src/ConstEval.cpp
)
# Frontend sources newer SafeC checkouts have: the heap/arena lifetime
# analysis Sema::run() calls (SafeC#52). Added when present, so this builds
# against a SafeC from before and after it.
foreach(src MemoryFlow.cpp)
    if(EXISTS ${SAFEC_DIR}/src/${src})
        list(APPEND SAFEC_FRONTEND_SOURCES ${SAFEC_DIR}/src/${src})
    endif()
endforeach()

add_library(safec_frontend STATIC ${SAFEC_FRONTEND_SOURCES})
target_include_directories(safec_frontend PUBLIC ${SAFEC_DIR}/include)
# CHeaderImporterStub.cpp's configuration hook (CHeaderBridge.h).
target_include_directories(safec_frontend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(safec_frontend PRIVATE /W3 /EHs-c- /GR-)
else()
    target_compile_options(safec_frontend PRIVATE
        -fno-exceptions
        -fno-rtti
        -Wall -Wextra -Wno-unused-parameter
    )
endif()
# Silence LLVM-style macros that may appear in headers
target_compile_definitions(safec_frontend PRIVATE SAFEC_NO_LLVM=1)

# ── nlohmann/json (interface / header-only) ──────────────────────────────────
add_library(nlohmann_json INTERFACE)
# third_party/ contains nlohmann/json.hpp — include as "nlohmann/json.hpp"
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── LSP server executable ────────────────────────────────────────────────────
# ScxTranspiler.cpp is referenced by path from the sibling safeguard
# checkout (same pattern as safec_frontend's SAFEC_DIR above) rather than
# vendored — one copy of the .scx transpiler, shared by 'safeguard build'
# and the LSP, so the two can't drift out of sync with each other.
add_executable(sc-lsp
    src/main.cpp
    src/Transport.cpp
    src/Analysis.cpp
    src/Server.cpp
    src/ProjectConfig.cpp
    ${SAFEGUARD_DIR}/src/ScxTranspiler.cpp
    # Package.toml parsing (for [features] defines), shared with safeguard
    # for the same reason as the transpiler.
    ${SAFEGUARD_DIR}/src/Manifest.cpp
)
target_include_directories(sc-lsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${SAFEGUARD_DIR}/include
)
target_link_libraries(sc-lsp PRIVATE safec_frontend nlohmann_json)
# Default -I path for '#include <std/...>' resolution. Preprocessor::
# resolveInclude joins this directory with the include's full spelling
# ("std/collections/vec.h"), so the path must be the *parent* of std/ (the
# SafeC repo root, i.e. <SAFEC_DIR>/.. since SAFEC_DIR is .../SafeC/compiler
# by default) — not std/ itself, which would resolve to a nonexistent
# ".../std/std/collections/vec.h". Without this, the preprocessor has no
# include path pointing at std/ at all, so every stdlib header fails to
# resolve and analysis on any file that includes one stops at the #include
# line. This is the last resort of ProjectConfig.cpp's safecRoot(), after
# $SAFEC_HOME and the installation sc-lsp runs from (<prefix>/bin/sc-lsp
# beside <prefix>/std) — the path baked in here only exists on the machine
# that built the binary. A client can extend the include path via
# initializationOptions.includePaths.
get_filename_component(SAFEC_STD_DIR_DEFAULT "${SAFEC_DIR}/.." ABSOLUTE)
target_compile_definitions(sc-lsp PRIVATE SAFEC_STD_DIR="${SAFEC_STD_DIR_DEFAULT}"
                                         SC_LSP_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(sc-lsp PRIVATE /W3)
else()
    target_compile_options(sc-lsp PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

install(TARGETS sc-lsp DESTINATION bin)
