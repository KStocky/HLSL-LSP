cmake_minimum_required(VERSION 3.28)

project(HLSLIntellisense VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(HLSL_INTELLISENSE_DEFAULT_DXC_INCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/../UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/ShaderConductor/External/DirectXShaderCompiler/include")
set(HLSL_INTELLISENSE_DEFAULT_DXC_RUNTIME
    "${CMAKE_CURRENT_SOURCE_DIR}/../UnrealEngine/Engine/Binaries/ThirdParty/ShaderConductor/Win64")

set(DXC_INCLUDE_DIR "${HLSL_INTELLISENSE_DEFAULT_DXC_INCLUDE}" CACHE PATH
    "Directory containing the dxc headers")
set(DXC_RUNTIME_DIR "${HLSL_INTELLISENSE_DEFAULT_DXC_RUNTIME}" CACHE PATH
    "Directory containing dxcompiler.dll and dxil.dll")

if(NOT EXISTS "${DXC_INCLUDE_DIR}/dxc/dxcisense.h")
    message(FATAL_ERROR
        "dxc/dxcisense.h was not found under DXC_INCLUDE_DIR='${DXC_INCLUDE_DIR}'")
endif()

find_file(DXCOMPILER_DLL
    NAMES dxcompiler.dll
    HINTS "${DXC_RUNTIME_DIR}"
    NO_DEFAULT_PATH
    REQUIRED)
find_file(DXIL_DLL
    NAMES dxil.dll
    HINTS "${DXC_RUNTIME_DIR}"
    NO_DEFAULT_PATH
    REQUIRED)

add_executable(hlsl-intellisense-poc
    src/main.cpp)

target_compile_features(hlsl-intellisense-poc PRIVATE cxx_std_23)
set_target_properties(hlsl-intellisense-poc PROPERTIES
    CXX_EXTENSIONS OFF)

target_include_directories(hlsl-intellisense-poc SYSTEM PRIVATE
    "${DXC_INCLUDE_DIR}")

if(MSVC)
    target_compile_options(hlsl-intellisense-poc PRIVATE
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /EHsc)
else()
    message(FATAL_ERROR "This proof of concept currently requires MSVC on Windows")
endif()

add_custom_command(TARGET hlsl-intellisense-poc POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:hlsl-intellisense-poc>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${DXIL_DLL}" "$<TARGET_FILE_DIR:hlsl-intellisense-poc>"
    VERBATIM)

if(BUILD_TESTING)
    add_test(NAME dxc-intellisense-smoke
        COMMAND hlsl-intellisense-poc)
endif()
