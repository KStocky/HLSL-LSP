include_guard(GLOBAL)

set(HLSL_DXC_VERSION "1.9.2607.13")
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
    set(HLSL_DXC_DESCRIPTION "official DXC ${HLSL_DXC_VERSION}")
    if(WIN32)
        FetchContent_Declare(
            hlsl_dxc_package
            URL
                "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.dxc/${HLSL_DXC_VERSION}/microsoft.direct3d.dxc.${HLSL_DXC_VERSION}.nupkg"
            URL_HASH
                SHA256=5d6acd23089b2979a3c1d39b7e31227da989a47b5d9f3db57111ad4717ea537e
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(hlsl_dxc_package)
        set(DXC_INCLUDE_DIR
            "${hlsl_dxc_package_SOURCE_DIR}/build/native/include")
        set(DXC_RUNTIME_DIR
            "${hlsl_dxc_package_SOURCE_DIR}/build/native/bin/x64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        FetchContent_Declare(
            hlsl_dxc_package
            URL
                "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/linux_dxc_2026_07_29.x86_x64.tar.gz"
            URL_HASH
                SHA256=55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(hlsl_dxc_package)
        set(DXC_INCLUDE_DIR "${hlsl_dxc_package_SOURCE_DIR}/include")
        set(DXC_RUNTIME_DIR "${hlsl_dxc_package_SOURCE_DIR}/lib")
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

message(STATUS
    "Using ${HLSL_DXC_DESCRIPTION}: ${DXCOMPILER_RUNTIME}")
