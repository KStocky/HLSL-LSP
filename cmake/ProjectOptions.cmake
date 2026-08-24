add_library(hlsl_project_options INTERFACE)
target_compile_features(hlsl_project_options INTERFACE cxx_std_23)

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(hlsl_project_options INTERFACE
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /EHsc)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(hlsl_project_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror)
else()
    message(FATAL_ERROR
        "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}'. Use MSVC, clang-cl, or Clang.")
endif()

set_target_properties(hlsl_project_options PROPERTIES
    CXX_EXTENSIONS OFF)

function(hlsl_find_clang_tool variable tool)
    if(DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
        return()
    endif()

    set(hints)
    if(CMAKE_GENERATOR_INSTANCE)
        list(APPEND hints
            "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/Llvm/x64/bin")
    endif()
    if(WIN32)
        list(APPEND hints "C:/Program Files/LLVM/bin")
    endif()

    find_program(${variable}
        NAMES "${tool}"
        HINTS ${hints}
        DOC "Path to ${tool}")
    set(${variable} "${${variable}}" CACHE FILEPATH "Path to ${tool}" FORCE)
endfunction()

function(hlsl_enable_clang_tidy target)
    if(NOT HLSL_ENABLE_CLANG_TIDY)
        return()
    endif()

    hlsl_find_clang_tool(CLANG_TIDY_EXECUTABLE clang-tidy)
    if(NOT CLANG_TIDY_EXECUTABLE)
        message(FATAL_ERROR
            "HLSL_ENABLE_CLANG_TIDY is ON, but clang-tidy was not found")
    endif()

    set_target_properties("${target}" PROPERTIES
        CXX_CLANG_TIDY
            "${CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*;--extra-arg=/EHsc")
endfunction()

function(hlsl_copy_dxc_runtime target)
    if(UNIX)
        add_dependencies("${target}" hlsl_dxc_runtime)
        set_target_properties("${target}" PROPERTIES
            BUILD_RPATH "${HLSL_DXC_BUILD_RUNTIME_DIR}"
            INSTALL_RPATH "$ORIGIN")
        return()
    endif()

    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DXCOMPILER_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)

    if(DXIL_RUNTIME)
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${DXIL_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

function(hlsl_add_format_targets)
    hlsl_find_clang_tool(CLANG_FORMAT_EXECUTABLE clang-format)
    if(NOT CLANG_FORMAT_EXECUTABLE)
        message(STATUS
            "clang-format was not found; format targets will not be available")
        return()
    endif()

    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/*.h"
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp")

    add_custom_target(format
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${sources}
        COMMENT "Formatting first-party C++ sources"
        VERBATIM)
    add_custom_target(format-check
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${sources}
        COMMENT "Checking first-party C++ formatting"
        VERBATIM)
endfunction()
