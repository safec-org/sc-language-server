cmake_minimum_required(VERSION 3.20)
project(sc-language-server VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SAFEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SafeC/compiler"
    CACHE PATH "SafeC compiler source directory")

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

add_library(safec_frontend STATIC ${SAFEC_FRONTEND_SOURCES})
target_include_directories(safec_frontend PUBLIC ${SAFEC_DIR}/include)
target_compile_options(safec_frontend PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall -Wextra -Wno-unused-parameter
)
# Silence LLVM-style macros that may appear in headers
target_compile_definitions(safec_frontend PRIVATE SAFEC_NO_LLVM=1)

# ── nlohmann/json (interface / header-only) ──────────────────────────────────
add_library(nlohmann_json INTERFACE)
# third_party/ contains nlohmann/json.hpp — include as "nlohmann/json.hpp"
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── LSP server executable ────────────────────────────────────────────────────
add_executable(sc-lsp
    src/main.cpp
    src/Transport.cpp
    src/Analysis.cpp
    src/Server.cpp
)
target_include_directories(sc-lsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(sc-lsp PRIVATE safec_frontend nlohmann_json)
target_compile_options(sc-lsp PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

install(TARGETS sc-lsp DESTINATION bin)
