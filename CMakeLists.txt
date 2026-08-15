cmake_minimum_required(VERSION 3.28)

project(HlslLsp VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)

option(HLSL_ENABLE_CLANG_TIDY "Run clang-tidy while compiling first-party code" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_MakeAvailable(Catch2 nlohmann_json)

if(WIN32)
    set(HLSL_INTELLISENSE_DEFAULT_DXC_INCLUDE
        "${CMAKE_CURRENT_SOURCE_DIR}/../UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/ShaderConductor/External/DirectXShaderCompiler/include/dxc")
    set(HLSL_INTELLISENSE_DEFAULT_DXC_RUNTIME
        "${CMAKE_CURRENT_SOURCE_DIR}/../UnrealEngine/Engine/Binaries/ThirdParty/ShaderConductor/Win64")
else()
    set(HLSL_INTELLISENSE_DEFAULT_DXC_INCLUDE "")
    set(HLSL_INTELLISENSE_DEFAULT_DXC_RUNTIME "")
endif()

set(DXC_INCLUDE_DIR "${HLSL_INTELLISENSE_DEFAULT_DXC_INCLUDE}" CACHE PATH
    "Directory containing the dxc headers")
set(DXC_RUNTIME_DIR "${HLSL_INTELLISENSE_DEFAULT_DXC_RUNTIME}" CACHE PATH
    "Directory containing the DXC runtime")

if(NOT EXISTS "${DXC_INCLUDE_DIR}/dxcisense.h")
    message(FATAL_ERROR
        "dxcisense.h was not found under DXC_INCLUDE_DIR='${DXC_INCLUDE_DIR}'")
endif()

find_file(DXCOMPILER_RUNTIME
    NAMES dxcompiler.dll libdxcompiler.so
    HINTS "${DXC_RUNTIME_DIR}"
    NO_DEFAULT_PATH
    REQUIRED)

find_file(DXIL_RUNTIME
    NAMES dxil.dll libdxil.so
    HINTS "${DXC_RUNTIME_DIR}"
    NO_DEFAULT_PATH)

add_library(hlsl_dxc STATIC)
target_sources(hlsl_dxc
    PRIVATE
        src/dxc/proof_of_concept.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/hlsl_intellisense/dxc/proof_of_concept.h)
target_link_libraries(hlsl_dxc
    PUBLIC hlsl_project_options)
target_include_directories(hlsl_dxc SYSTEM PRIVATE
    "${DXC_INCLUDE_DIR}")
if(CMAKE_DL_LIBS)
    target_link_libraries(hlsl_dxc PRIVATE "${CMAKE_DL_LIBS}")
endif()
if(WIN32)
    target_link_libraries(hlsl_dxc PRIVATE ole32)
endif()
hlsl_enable_clang_tidy(hlsl_dxc)

add_executable(hlsl-intellisense-poc
    src/app/main.cpp)
target_link_libraries(hlsl-intellisense-poc PRIVATE
    hlsl_dxc
    hlsl_project_options)
hlsl_enable_clang_tidy(hlsl-intellisense-poc)
hlsl_copy_dxc_runtime(hlsl-intellisense-poc)

if(BUILD_TESTING)
    add_executable(hlsl-intellisense-tests
        tests/dxc/proof_of_concept_tests.cpp)
    target_link_libraries(hlsl-intellisense-tests PRIVATE
        Catch2::Catch2WithMain
        hlsl_dxc
        hlsl_project_options)
    hlsl_enable_clang_tidy(hlsl-intellisense-tests)
    hlsl_copy_dxc_runtime(hlsl-intellisense-tests)

    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
    catch_discover_tests(hlsl-intellisense-tests)
endif()

hlsl_add_format_targets()
