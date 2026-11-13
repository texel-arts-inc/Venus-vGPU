# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(ExternalProject)
find_package(Git REQUIRED)
find_executable(VENUS_MESON NAMES meson REQUIRED)
find_executable(VENUS_NINJA NAMES ninja REQUIRED)
set(VENUS_MESA_PYTHONPATH "$ENV{PYTHONPATH}" CACHE STRING
    "Optional Python module search path for the Mesa build")
set(VENUS_MESA_ARCHIVE "" CACHE FILEPATH "Optional local Mesa 26.3.0 release archive")
set(VENUS_FRAMEWORK_DIR "${CMAKE_BINARY_DIR}/framework/Venus.framework")
set(VENUS_MESA_SOURCE_DIR "${CMAKE_BINARY_DIR}/upstream/mesa")
set(_mesa_binary_dir "${CMAKE_BINARY_DIR}/mesa-build")
set(_mesa_icd "${_mesa_binary_dir}/src/virtio/vulkan/libvulkan_virtio.dylib")
set(_mesa_patch "${PROJECT_SOURCE_DIR}/patches/mesa/darwin-venus.patch")

if(VENUS_MESA_ARCHIVE)
    get_filename_component(_mesa_archive "${VENUS_MESA_ARCHIVE}" ABSOLUTE)
    set(_mesa_download URL "${_mesa_archive}"
        URL_HASH SHA256=91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
else()
    set(_mesa_download GIT_REPOSITORY https://gitlab.freedesktop.org/mesa/mesa.git
        GIT_TAG mesa-26.3.0 GIT_SHALLOW TRUE GIT_SUBMODULES "" UPDATE_DISCONNECTED TRUE)
    set(_mesa_revision 0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4)
endif()

ExternalProject_Add(venus-protocol-source
    EXCLUDE_FROM_ALL TRUE
    GIT_REPOSITORY https://gitlab.freedesktop.org/virgl/venus-protocol.git
    GIT_TAG v1.1.3 GIT_SHALLOW TRUE GIT_SUBMODULES "" UPDATE_DISCONNECTED TRUE
    SOURCE_DIR "${CMAKE_BINARY_DIR}/upstream/venus-protocol"
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" "-DGIT=${GIT_EXECUTABLE}"
        "-DEXPECTED=ca19b6358d7cc491bc3e4de76f04c6700876a8fa"
        -P "${PROJECT_SOURCE_DIR}/cmake/VerifyRevision.cmake"
    CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND ""
)
ExternalProject_Add(venus-mesa
    EXCLUDE_FROM_ALL TRUE
    ${_mesa_download}
    SOURCE_DIR "${VENUS_MESA_SOURCE_DIR}"
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" "-DGIT=${GIT_EXECUTABLE}"
        "-DEXPECTED=${_mesa_revision}" -P "${PROJECT_SOURCE_DIR}/cmake/VerifyRevision.cmake"
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" "-DGIT=${GIT_EXECUTABLE}"
        "-DPATCH_FILES=${_mesa_patch}" -P "${PROJECT_SOURCE_DIR}/cmake/ApplyPatches.cmake"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${VENUS_MESA_PYTHONPATH}"
        "${CMAKE_COMMAND}" "-DMESON=${VENUS_MESON}" "-DBINARY_DIR=<BINARY_DIR>"
        "-DSOURCE_DIR=<SOURCE_DIR>" -P "${PROJECT_SOURCE_DIR}/cmake/ConfigureMesa.cmake"
    BUILD_COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${VENUS_MESA_PYTHONPATH}"
        "${VENUS_NINJA}" -C <BINARY_DIR> src/virtio/vulkan/libvulkan_virtio.dylib
    BINARY_DIR "${_mesa_binary_dir}"
    BUILD_BYPRODUCTS "${_mesa_icd}"
    INSTALL_COMMAND ""
)
ExternalProject_Add_Step(venus-mesa stage-protocol
    COMMAND "${CMAKE_COMMAND}" "-DPROTOCOL=${CMAKE_BINARY_DIR}/upstream/venus-protocol"
        "-DMESA=<SOURCE_DIR>" -P "${PROJECT_SOURCE_DIR}/cmake/StageProtocol.cmake"
    DEPENDEES patch
    DEPENDERS configure
)
ExternalProject_Add_StepDependencies(venus-mesa stage-protocol venus-protocol-source)

add_custom_command(
    OUTPUT "${VENUS_FRAMEWORK_DIR}/Versions/A/Venus"
    COMMAND "${CMAKE_COMMAND}" "-DICD=${_mesa_icd}" "-DFRAMEWORK=${VENUS_FRAMEWORK_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/StageFramework.cmake"
    DEPENDS "${_mesa_icd}" "${PROJECT_SOURCE_DIR}/cmake/StageFramework.cmake"
    VERBATIM
)
add_custom_target(venus-framework DEPENDS "${VENUS_FRAMEWORK_DIR}/Versions/A/Venus")
add_dependencies(venus-framework venus-mesa)
