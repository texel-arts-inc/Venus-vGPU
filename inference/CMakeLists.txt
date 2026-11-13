# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

if(NOT APPLE OR NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "VENUS_BUILD_OLLAMA requires Apple Silicon macOS.")
endif()

include(ExternalProject)
include(ProcessorCount)
find_package(Git REQUIRED)
find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(SPIRV-Headers CONFIG REQUIRED)
find_executable(VENUS_OLLAMA_GO NAMES go REQUIRED)
get_filename_component(_go_bin "${VENUS_OLLAMA_GO}" DIRECTORY)

ProcessorCount(_jobs)
if(NOT _jobs)
    set(_jobs 1)
endif()
set(VENUS_OLLAMA_JOBS "${_jobs}" CACHE STRING "Parallel jobs for the Ollama native build")
set(_ollama_version 0.40.16)
set(_ollama_revision d63b702609c4300a2ca07be2bdb0adb9834725e5)
set(_ollama_stage "${CMAKE_BINARY_DIR}/ollama")
set(_ollama_patch_dir "${PROJECT_SOURCE_DIR}/patches/ollama")
set(_ollama_patches
    "${_ollama_patch_dir}/darwin-vulkan.patch"
    "${_ollama_patch_dir}/vulkan-device-ordinal.patch"
)
string(JOIN "|" _ollama_patch_args ${_ollama_patches})
set(_verify_revision "${PROJECT_SOURCE_DIR}/cmake/VerifyRevision.cmake")
set(_apply_patches "${PROJECT_SOURCE_DIR}/cmake/ApplyPatches.cmake")

ExternalProject_Add(venus-ollama
    EXCLUDE_FROM_ALL TRUE
    GIT_REPOSITORY https://github.com/ollama/ollama.git
    GIT_TAG "v${_ollama_version}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    UPDATE_DISCONNECTED TRUE
    LIST_SEPARATOR "|"
    SOURCE_DIR "${CMAKE_BINARY_DIR}/upstream/ollama"
    BINARY_DIR "${CMAKE_BINARY_DIR}/ollama-build"
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" "-DGIT=${GIT_EXECUTABLE}"
        "-DEXPECTED=${_ollama_revision}" -P "${_verify_revision}"
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" "-DGIT=${GIT_EXECUTABLE}"
        "-DPATCH_FILES=${_ollama_patch_args}" -P "${_apply_patches}"
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_OSX_ARCHITECTURES=arm64
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DGO_EXECUTABLE=${VENUS_OLLAMA_GO}"
        "-DVulkan_INCLUDE_DIR=${Vulkan_INCLUDE_DIR}"
        "-DVulkan_LIBRARY=${Vulkan_LIBRARY}"
        "-DVulkan_GLSLC_EXECUTABLE=${Vulkan_GLSLC_EXECUTABLE}"
        "-DSPIRV-Headers_DIR=${SPIRV-Headers_DIR}"
        "-DVENUS_VERIFY_REVISION=${_verify_revision}"
        -DOLLAMA_LLAMA_BACKENDS=vulkan
        -DOLLAMA_MLX_BACKENDS=
        -DGGML_METAL=OFF
        -DGGML_METAL_EMBED_LIBRARY=OFF
        "-DOLLAMA_VERSION=${_ollama_version}"
        "-DOLLAMA_BUILD_PARALLEL=${VENUS_OLLAMA_JOBS}"
        "-DOLLAMA_GO_OUTPUT=${_ollama_stage}/bin/ollama"
        "-DOLLAMA_PAYLOAD_INSTALL_PREFIX=${_ollama_stage}"
    BUILD_COMMAND "${CMAKE_COMMAND}" -E env
        "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
        "PATH=${_go_bin}:$ENV{PATH}"
        "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --target ollama-local ollama-llama-server-vulkan ollama-go-license
    BUILD_BYPRODUCTS
        "${_ollama_stage}/bin/ollama"
        "${_ollama_stage}/lib/ollama/llama-server"
        "${_ollama_stage}/lib/ollama/vulkan/libggml-vulkan.so"
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "<SOURCE_DIR>/LICENSE" "${_ollama_stage}/lib/ollama/OLLAMA_LICENSE"
    USES_TERMINAL_DOWNLOAD TRUE
    USES_TERMINAL_BUILD TRUE
)
ExternalProject_Add_StepDependencies(venus-ollama patch
    ${_ollama_patches} "${_verify_revision}" "${_apply_patches}")

file(MAKE_DIRECTORY "${_ollama_stage}")
configure_file(ollama-venus "${_ollama_stage}/ollama-venus" COPYONLY)
