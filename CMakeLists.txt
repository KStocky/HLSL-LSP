cmake_minimum_required(VERSION 3.28)

project(HlslLsp VERSION 0.8.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)

option(HLSL_ENABLE_CLANG_TIDY "Run clang-tidy while compiling first-party code" OFF)
option(HLSL_BUILD_FUZZERS "Build libFuzzer protocol and workspace harnesses" OFF)

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
include(Dxc)

add_library(hlsl_json_rpc STATIC)
target_sources(hlsl_json_rpc
    PRIVATE
        src/json_rpc/dispatcher.cpp
        src/json_rpc/framing.cpp
        src/json_rpc/message.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/hlsl_intellisense/json_rpc/dispatcher.h
            include/hlsl_intellisense/json_rpc/framing.h
            include/hlsl_intellisense/json_rpc/message.h)
target_link_libraries(hlsl_json_rpc
    PUBLIC
        hlsl_project_options
        nlohmann_json::nlohmann_json)
hlsl_enable_clang_tidy(hlsl_json_rpc)

add_library(hlsl_workspace STATIC)
target_sources(hlsl_workspace
    PRIVATE
        src/workspace/configuration.cpp
        src/workspace/document_store.cpp
        src/workspace/document_uri.cpp
        src/workspace/include_resolver.cpp
        src/workspace/text_position.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/hlsl_intellisense/workspace/configuration.h
            include/hlsl_intellisense/workspace/document_store.h
            include/hlsl_intellisense/workspace/document_uri.h
            include/hlsl_intellisense/workspace/error.h
            include/hlsl_intellisense/workspace/include_resolver.h
            include/hlsl_intellisense/workspace/text_position.h)
target_link_libraries(hlsl_workspace
    PUBLIC
        hlsl_dxc
        hlsl_project_options
    PRIVATE
        nlohmann_json::nlohmann_json)
hlsl_enable_clang_tidy(hlsl_workspace)

add_library(hlsl_dxc STATIC)
target_sources(hlsl_dxc
    PRIVATE
        src/dxc/compilation_info.cpp
        src/dxc/intellisense.cpp
        src/dxc/memory_layout.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/hlsl_intellisense/dxc/intellisense.h)
target_link_libraries(hlsl_dxc
    PUBLIC hlsl_project_options)
target_include_directories(hlsl_dxc SYSTEM PRIVATE
    "${DXC_INCLUDE_DIR}")
if(CMAKE_DL_LIBS)
    target_link_libraries(hlsl_dxc PRIVATE "${CMAKE_DL_LIBS}")
endif()
if(WIN32)
    target_link_libraries(hlsl_dxc PRIVATE ole32 oleaut32)
endif()
hlsl_enable_clang_tidy(hlsl_dxc)

add_library(hlsl_lsp STATIC)
target_sources(hlsl_lsp
    PRIVATE
        src/analysis/manager.cpp
        src/analysis/scheduler.cpp
        src/lsp/server.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/hlsl_intellisense/analysis/manager.h
            include/hlsl_intellisense/analysis/scheduler.h
            include/hlsl_intellisense/lsp/server.h)
target_link_libraries(hlsl_lsp
    PUBLIC
        hlsl_dxc
        hlsl_json_rpc
        hlsl_workspace
        hlsl_project_options)
target_compile_definitions(hlsl_lsp PUBLIC
    HLSL_LSP_VERSION="${PROJECT_VERSION}")
hlsl_enable_clang_tidy(hlsl_lsp)

add_executable(hlsl-lsp
    src/app/main.cpp)
target_link_libraries(hlsl-lsp PRIVATE
    hlsl_lsp
    hlsl_project_options)
hlsl_enable_clang_tidy(hlsl-lsp)
hlsl_copy_dxc_runtime(hlsl-lsp)

install(TARGETS hlsl-lsp
    RUNTIME DESTINATION "."
    COMPONENT Runtime)
install(FILES ${HLSL_DXC_RUNTIME_FILES}
    DESTINATION "."
    COMPONENT Runtime)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    DESTINATION "licenses/hlsl-lsp"
    COMPONENT Runtime)
if(DXC_LICENSE_FILE)
    install(FILES "${DXC_LICENSE_FILE}"
        DESTINATION "licenses/dxc"
        COMPONENT Runtime)
endif()
if(EXISTS "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT")
    install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT"
        DESTINATION "licenses/nlohmann-json"
        COMPONENT Runtime)
endif()

if(BUILD_TESTING)
    add_executable(hlsl-intellisense-tests
        tests/dxc/corpus_tests.cpp
        tests/dxc/intellisense_tests.cpp)
    target_link_libraries(hlsl-intellisense-tests PRIVATE
        Catch2::Catch2WithMain
        hlsl_workspace
        hlsl_project_options)
    hlsl_enable_clang_tidy(hlsl-intellisense-tests)
    target_compile_definitions(hlsl-intellisense-tests PRIVATE
        HLSL_TEST_CORPUS_DIR="${PROJECT_SOURCE_DIR}/tests/corpus"
        HLSL_TEST_DXC_RUNTIME_DIR="${DXC_RUNTIME_DIR}")
    hlsl_copy_dxc_runtime(hlsl-intellisense-tests)

    add_executable(hlsl-lsp-tests
        tests/analysis/manager_tests.cpp
        tests/analysis/scheduler_tests.cpp
        tests/lsp/server_tests.cpp)
    target_link_libraries(hlsl-lsp-tests PRIVATE
        Catch2::Catch2WithMain
        hlsl_lsp
        hlsl_project_options)
    hlsl_enable_clang_tidy(hlsl-lsp-tests)
    target_compile_definitions(hlsl-lsp-tests PRIVATE
        HLSL_TEST_DXC_RUNTIME_DIR="${DXC_RUNTIME_DIR}")
    hlsl_copy_dxc_runtime(hlsl-lsp-tests)

    add_executable(hlsl-json-rpc-tests
        tests/json_rpc/dispatcher_tests.cpp
        tests/json_rpc/framing_tests.cpp
        tests/json_rpc/message_tests.cpp)
    target_link_libraries(hlsl-json-rpc-tests PRIVATE
        Catch2::Catch2WithMain
        hlsl_json_rpc
        hlsl_project_options)
    hlsl_enable_clang_tidy(hlsl-json-rpc-tests)

    add_executable(hlsl-workspace-tests
        tests/workspace/configuration_tests.cpp
        tests/workspace/document_store_tests.cpp
        tests/workspace/document_uri_tests.cpp
        tests/workspace/include_resolver_tests.cpp
        tests/workspace/text_position_tests.cpp)
    target_link_libraries(hlsl-workspace-tests PRIVATE
        Catch2::Catch2WithMain
        hlsl_workspace
        hlsl_project_options)
    hlsl_enable_clang_tidy(hlsl-workspace-tests)

    add_executable(hlsl-analysis-benchmark
        benchmarks/analysis_benchmark.cpp)
    target_link_libraries(hlsl-analysis-benchmark PRIVATE
        hlsl_lsp
        hlsl_project_options)
    target_compile_definitions(hlsl-analysis-benchmark PRIVATE
        HLSL_BENCHMARK_SHADER_DIR="${PROJECT_SOURCE_DIR}/benchmarks/shaders")
    hlsl_enable_clang_tidy(hlsl-analysis-benchmark)
    hlsl_copy_dxc_runtime(hlsl-analysis-benchmark)

    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
    catch_discover_tests(hlsl-intellisense-tests)
    catch_discover_tests(hlsl-json-rpc-tests)
    catch_discover_tests(hlsl-workspace-tests)
    catch_discover_tests(hlsl-lsp-tests)
    add_test(NAME analysis-structural-benchmark
        COMMAND hlsl-analysis-benchmark)
endif()

if(HLSL_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
       CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(FATAL_ERROR "HLSL_BUILD_FUZZERS requires Clang with the GNU-like frontend")
    endif()

    function(hlsl_add_fuzzer target source library)
        add_executable("${target}" "${source}")
        target_link_libraries("${target}" PRIVATE "${library}" hlsl_project_options)
        target_compile_options("${target}" PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options("${target}" PRIVATE -fsanitize=fuzzer,address,undefined)
    endfunction()

    foreach(fuzzed_library hlsl_json_rpc hlsl_workspace)
        target_compile_options("${fuzzed_library}" PRIVATE
            -fsanitize=fuzzer-no-link,address,undefined)
        target_link_options("${fuzzed_library}" INTERFACE
            -fsanitize=address,undefined)
    endforeach()

    hlsl_add_fuzzer(hlsl-framing-fuzzer fuzz/framing_fuzzer.cpp hlsl_json_rpc)
    hlsl_add_fuzzer(hlsl-json-fuzzer fuzz/json_fuzzer.cpp hlsl_json_rpc)
    hlsl_add_fuzzer(hlsl-uri-fuzzer fuzz/uri_fuzzer.cpp hlsl_workspace)
    hlsl_add_fuzzer(hlsl-edits-fuzzer fuzz/edits_fuzzer.cpp hlsl_workspace)
endif()

hlsl_add_format_targets()
