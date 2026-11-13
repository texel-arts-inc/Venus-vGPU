# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(ExternalProject)
find_package(Git REQUIRED)
set(VENUS_VIRGL_SOURCE_DIR "${CMAKE_BINARY_DIR}/upstream/virglrenderer")
set(_virgl_patch "${PROJECT_SOURCE_DIR}/patches/virglrenderer/venus-guest.patch")
ExternalProject_Add(venus-virgl-source
    EXCLUDE_FROM_ALL TRUE
    GIT_REPOSITORY https://github.com/utmapp/virglrenderer.git
    GIT_TAG 5d26f605f50f8e22002ec6db5fb775e1992d4e96
    UPDATE_DISCONNECTED TRUE
    GIT_SUBMODULES ""
    SOURCE_DIR "${VENUS_VIRGL_SOURCE_DIR}"
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" "-DGIT=${GIT_EXECUTABLE}"
        "-DPATCH_FILES=${_virgl_patch}" -P "${PROJECT_SOURCE_DIR}/cmake/ApplyPatches.cmake"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
)
