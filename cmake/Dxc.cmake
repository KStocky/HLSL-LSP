include_guard(GLOBAL)

set(HLSL_DXC_RELEASE "2609-kstocky.3")
set(DXC_INCLUDE_DIR "" CACHE PATH
    "Optional directory containing dxcisense.h; requires DXC_RUNTIME_DIR")
set(DXC_RUNTIME_DIR "" CACHE PATH
    "Optional directory containing the DXC runtime; requires DXC_INCLUDE_DIR")

if((DXC_INCLUDE_DIR AND NOT DXC_RUNTIME_DIR) OR
   (DXC_RUNTIME_DIR AND NOT DXC_INCLUDE_DIR))
    message(FATAL_ERROR
        "DXC_INCLUDE_DIR and DXC_RUNTIME_DIR must be specified together")
endif()

if(NOT DXC_INCLUDE_DIR)
    if(WIN32)
        set(HLSL_DXC_DESCRIPTION
            "KStocky DXC release ${HLSL_DXC_RELEASE}")
        FetchContent_Declare(
            hlsl_dxc_package
            URL
                "https://github.com/KStocky/DirectXShaderCompiler/releases/download/${HLSL_DXC_RELEASE}/dxc_${HLSL_DXC_RELEASE}.x64.zip"
            URL_HASH
                SHA256=1f4d60f2c5f0ce2a8b07049071611e5174f0bb486c792307aef32b1c50bbaeff
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(hlsl_dxc_package)
        set(DXC_INCLUDE_DIR
            "${hlsl_dxc_package_SOURCE_DIR}/inc")
        set(DXC_RUNTIME_DIR
            "${hlsl_dxc_package_SOURCE_DIR}/bin/x64")
        set(DXC_LICENSE_FILE
            "${hlsl_dxc_package_SOURCE_DIR}/LICENSE-LLVM.txt")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
           CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(HLSL_DXC_DESCRIPTION
            "KStocky DXC release ${HLSL_DXC_RELEASE}")
        FetchContent_Declare(
            hlsl_dxc_package
            URL
                "https://github.com/KStocky/DirectXShaderCompiler/releases/download/${HLSL_DXC_RELEASE}/linux_dxc_${HLSL_DXC_RELEASE}.x86_x64.tar.gz"
            URL_HASH
                SHA256=8db20e74260ffcdfe2285a7bc5ad052530505479c4c2467d1195dbf2056950d4
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(hlsl_dxc_package)
        set(HLSL_DXC_LINUX_ROOT "${hlsl_dxc_package_SOURCE_DIR}")
        set(HLSL_DXC_LINUX_ARCHIVE_ROOT
            "${hlsl_dxc_package_SOURCE_DIR}/linux_dxc_${HLSL_DXC_RELEASE}.x86_x64")
        if(EXISTS "${HLSL_DXC_LINUX_ARCHIVE_ROOT}/include")
            set(HLSL_DXC_LINUX_ROOT "${HLSL_DXC_LINUX_ARCHIVE_ROOT}")
        endif()
        set(DXC_INCLUDE_DIR "${HLSL_DXC_LINUX_ROOT}/include")
        set(DXC_RUNTIME_DIR "${HLSL_DXC_LINUX_ROOT}/lib")
        set(DXC_LICENSE_FILE
            "${HLSL_DXC_LINUX_ROOT}/LICENSE-LLVM.txt")
    else()
        message(FATAL_ERROR
            "Automatic DXC acquisition supports Windows x64 and Linux x64. "
            "Set DXC_INCLUDE_DIR and DXC_RUNTIME_DIR for this platform.")
    endif()
else()
    set(HLSL_DXC_DESCRIPTION "custom DXC")
endif()

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

set(HLSL_DXC_RUNTIME_FILES "${DXCOMPILER_RUNTIME}")
if(WIN32 AND DXIL_RUNTIME)
    list(APPEND HLSL_DXC_RUNTIME_FILES "${DXIL_RUNTIME}")
endif()

if(UNIX)
    set(HLSL_DXC_BUILD_RUNTIME_DIR "${CMAKE_BINARY_DIR}/dxc-runtime")
    set(HLSL_DXC_BUILD_RUNTIME_FILES)
    foreach(runtime IN LISTS HLSL_DXC_RUNTIME_FILES)
        get_filename_component(runtime_name "${runtime}" NAME)
        set(staged_runtime
            "${HLSL_DXC_BUILD_RUNTIME_DIR}/${runtime_name}")
        add_custom_command(
            OUTPUT "${staged_runtime}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${HLSL_DXC_BUILD_RUNTIME_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime}" "${staged_runtime}"
            DEPENDS "${runtime}"
            VERBATIM)
        list(APPEND HLSL_DXC_BUILD_RUNTIME_FILES "${staged_runtime}")
    endforeach()
    add_custom_target(hlsl_dxc_runtime
        DEPENDS ${HLSL_DXC_BUILD_RUNTIME_FILES})
endif()

message(STATUS
    "Using ${HLSL_DXC_DESCRIPTION}: ${DXCOMPILER_RUNTIME}")
