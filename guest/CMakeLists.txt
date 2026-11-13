# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
    OR NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$"
    OR CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "Build the guest in a native aarch64 Fedora Linux builder. See guest/README.md.")
endif()

set(VENUS_GUEST_GPU "none" CACHE STRING "Guest GPU driver (none, nvidia, nouveau)")
set_property(CACHE VENUS_GUEST_GPU PROPERTY STRINGS none nvidia nouveau)
set(VENUS_GUEST_LINUX_SOURCE "" CACHE PATH "Local clean Linux Git checkout at v7.3.3 (optional)")
set(VENUS_GUEST_LINUX_ARCHIVE "" CACHE FILEPATH "Local kernel.org Linux 7.3.3 .tar.xz (optional)")
set(VENUS_GUEST_MODULE_KEY "" CACHE FILEPATH "Private RSA signing key and certificate in a combined PEM")
set(VENUS_GUEST_NVIDIA_INSTALLER "" CACHE FILEPATH "Local NVIDIA-Linux-aarch64-620.70.06.run")
set(VENUS_GUEST_NVIDIA_SHA256 "3fa65a9271e0dde2a4e58c4965c38885a02a3a42accdcb26fc817410607f3e03")
option(VENUS_GUEST_DOWNLOAD_NVIDIA "Download the pinned NVIDIA installer" OFF)
option(VENUS_GUEST_ACCEPT_NVIDIA_LICENSE "Confirm acceptance of the NVIDIA installer license" OFF)
set(VENUS_GUEST_SOURCE_DATE_EPOCH "1793966400" CACHE STRING "Unix timestamp used for guest builds")
set(VENUS_GUEST_VRAM_SIZE_MIB "24576" CACHE STRING "VRAM scratch capacity in MiB")
include(ProcessorCount)
ProcessorCount(guest_processors)
if(NOT guest_processors)
  set(guest_processors 1)
endif()
set(VENUS_GUEST_JOBS "${guest_processors}" CACHE STRING "Parallel Linux kernel build jobs")

if(NOT VENUS_GUEST_GPU MATCHES "^(none|nvidia|nouveau)$")
  message(FATAL_ERROR "VENUS_GUEST_GPU must be none, nvidia or nouveau. See guest/README.md for Nova status.")
endif()
foreach(value VENUS_GUEST_JOBS VENUS_GUEST_SOURCE_DATE_EPOCH VENUS_GUEST_VRAM_SIZE_MIB)
  if(NOT "${${value}}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "${value} must be a positive integer")
  endif()
endforeach()
foreach(path VENUS_GUEST_LINUX_SOURCE VENUS_GUEST_LINUX_ARCHIVE VENUS_GUEST_MODULE_KEY VENUS_GUEST_NVIDIA_INSTALLER)
  if(${path} AND NOT EXISTS "${${path}}")
    message(FATAL_ERROR "${path} does not exist: ${${path}}")
  endif()
endforeach()

if(VENUS_GUEST_GPU STREQUAL "nvidia")
  if(NOT VENUS_GUEST_ACCEPT_NVIDIA_LICENSE)
    message(FATAL_ERROR "Review https://download.nvidia.com/XFree86/Linux-aarch64/620.70.06/license.txt and set VENUS_GUEST_ACCEPT_NVIDIA_LICENSE=ON if you accept it.")
  endif()
  if(VENUS_GUEST_NVIDIA_INSTALLER AND VENUS_GUEST_DOWNLOAD_NVIDIA)
    message(FATAL_ERROR "Choose a local NVIDIA installer or enable VENUS_GUEST_DOWNLOAD_NVIDIA.")
  endif()
  if(NOT VENUS_GUEST_NVIDIA_INSTALLER AND NOT VENUS_GUEST_DOWNLOAD_NVIDIA)
    message(FATAL_ERROR "Set VENUS_GUEST_NVIDIA_INSTALLER or enable VENUS_GUEST_DOWNLOAD_NVIDIA.")
  endif()
elseif(VENUS_GUEST_NVIDIA_INSTALLER OR VENUS_GUEST_DOWNLOAD_NVIDIA)
  message(FATAL_ERROR "NVIDIA installer inputs require VENUS_GUEST_GPU=nvidia.")
endif()

find_package(Threads REQUIRED)
find_executable(BASH_PROGRAM NAMES bash REQUIRED)
find_executable(GNU_MAKE NAMES make REQUIRED)
find_executable(GUEST_GCC NAMES gcc REQUIRED)
find_executable(GUEST_MESON NAMES meson REQUIRED)
find_executable(GUEST_NINJA NAMES ninja REQUIRED)
find_executable(GUEST_QEMU_IMG NAMES qemu-img REQUIRED)
find_executable(GUEST_RPM NAMES rpm REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(tool semodule mke2fs debugfs e2fsck openssl modinfo modprobe depmod zstd cpio)
  string(TOUPPER "${tool}" tool_variable)
  find_executable(GUEST_${tool_variable} NAMES "${tool}" REQUIRED)
endforeach()

set(guest_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(guest_build "${CMAKE_CURRENT_BINARY_DIR}")
set(guest_artifacts "${guest_build}/artifacts")
set(guest_runtime "${guest_dir}/runtime")
set(virgl_source "${VENUS_VIRGL_SOURCE_DIR}")
set(virgl_build "${guest_build}/virglrenderer")
set(virgl_lib "${virgl_build}/src/libvirglrenderer.so.1.11.0")
set(virgl_server "${virgl_build}/server/virgl_render_server")

configure_file(cmake/guest-inputs.in guest-inputs @ONLY)
add_custom_command(
  OUTPUT "${virgl_lib}" "${virgl_server}"
  COMMAND "${BASH_PROGRAM}" "${guest_dir}/cmake/build-virgl.sh"
          "${GUEST_MESON}" "${GUEST_NINJA}" "${GUEST_GCC}" "${virgl_source}" "${virgl_build}"
  DEPENDS venus-virgl-source "${guest_dir}/cmake/build-virgl.sh" "${PROJECT_SOURCE_DIR}/patches/virglrenderer/venus-guest.patch"
  USES_TERMINAL VERBATIM)
add_custom_target(venus-guest-virgl DEPENDS "${virgl_lib}" "${virgl_server}")

add_executable(venus-guest-renderer EXCLUDE_FROM_ALL
  renderer/guest.c renderer/rv_control.c renderer/renderer-pm.c renderer/venus-pm-wire.c)
set_target_properties(venus-guest-renderer PROPERTIES OUTPUT_NAME rv-renderer
  BUILD_RPATH_USE_ORIGIN ON BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "")
target_include_directories(venus-guest-renderer PRIVATE "${virgl_source}/src" "${virgl_build}/src" kernel/modules)
target_compile_definitions(venus-guest-renderer PRIVATE VIRGL_RENDERER_UNSTABLE_APIS=1)
target_link_libraries(venus-guest-renderer PRIVATE "${virgl_lib}" Threads::Threads)
add_dependencies(venus-guest-renderer venus-guest-virgl)

add_executable(venus-guest-bridge EXCLUDE_FROM_ALL runtime/venus-pm-bridge.c renderer/venus-pm-wire.c)
target_include_directories(venus-guest-bridge PRIVATE renderer)
add_executable(venus-guest-power EXCLUDE_FROM_ALL runtime/venus-gpu-power.c)
add_executable(venus-guest-bootstrap EXCLUDE_FROM_ALL runtime/microvm-bootstrap.c)
add_executable(venus-guest-poweroff EXCLUDE_FROM_ALL runtime/poweroff.c)
foreach(target venus-guest-renderer venus-guest-bridge venus-guest-power venus-guest-bootstrap venus-guest-poweroff)
  target_compile_options(${target} PRIVATE -O2 -Wall -Wextra)
endforeach()

include(ExternalProject)
find_package(Git REQUIRED)
set(kernel_commit "d50d994bffa3c7c6db2d8bc4c5f0c55e3f46b0cd")
set(kernel_sha256 "79c66dde66cd96877fcde8f7d36aec946b1f423d2b97b5a7b1751b75c764f3d1")
set(linux_source "${guest_build}/linux-source")
if(VENUS_GUEST_LINUX_SOURCE AND VENUS_GUEST_LINUX_ARCHIVE)
  message(FATAL_ERROR "Choose either VENUS_GUEST_LINUX_SOURCE or VENUS_GUEST_LINUX_ARCHIVE.")
elseif(VENUS_GUEST_LINUX_SOURCE)
  set(linux_download
    DOWNLOAD_COMMAND "${CMAKE_COMMAND}" "-DGIT=${GIT_EXECUTABLE}"
      "-DLOCAL_SOURCE=${VENUS_GUEST_LINUX_SOURCE}" "-DDESTINATION=${linux_source}"
      "-DCOMMIT=${kernel_commit}" -P "${guest_dir}/cmake/LocalLinux.cmake"
    UPDATE_COMMAND "")
elseif(VENUS_GUEST_LINUX_ARCHIVE)
  set(linux_download URL "${VENUS_GUEST_LINUX_ARCHIVE}"
    URL_HASH "SHA256=${kernel_sha256}" DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
else()
  set(linux_download GIT_REPOSITORY "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
    GIT_TAG "${kernel_commit}" GIT_PROGRESS TRUE UPDATE_DISCONNECTED TRUE)
endif()
ExternalProject_Add(venus-linux-source
  ${linux_download}
  SOURCE_DIR "${linux_source}"
  CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "" EXCLUDE_FROM_ALL TRUE)
set(linux_stamp "${guest_build}/venus-linux-source-prefix/src/venus-linux-source-stamp/venus-linux-source-download")

set(nvidia_archive "")
if(VENUS_GUEST_GPU STREQUAL "nvidia")
  set(nvidia_archive "${guest_build}/NVIDIA-Linux-aarch64-620.70.06.run")
  add_custom_command(
    OUTPUT "${nvidia_archive}"
    COMMAND "${CMAKE_COMMAND}" "-DLOCAL_FILE=${VENUS_GUEST_NVIDIA_INSTALLER}"
            "-DDESTINATION=${nvidia_archive}"
            "-DURL=https://download.nvidia.com/XFree86/Linux-aarch64/620.70.06/NVIDIA-Linux-aarch64-620.70.06.run"
            "-DSHA256=${VENUS_GUEST_NVIDIA_SHA256}" -P "${guest_dir}/cmake/Acquire.cmake"
    DEPENDS "${guest_build}/guest-inputs" "${guest_dir}/cmake/Acquire.cmake" ${VENUS_GUEST_NVIDIA_INSTALLER}
    USES_TERMINAL VERBATIM)
endif()

file(GLOB_RECURSE kernel_module_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false "${guest_dir}/kernel/modules/*")
set(guest_kernel_stamp "${guest_build}/kernel-ready")
add_custom_command(
  OUTPUT "${guest_kernel_stamp}"
  COMMAND "${CMAKE_COMMAND}" -E env "SOURCE_DATE_EPOCH=${VENUS_GUEST_SOURCE_DATE_EPOCH}"
          "${BASH_PROGRAM}" "${guest_dir}/cmake/build-kernel.sh"
          "${guest_dir}" "${guest_build}" "${GNU_MAKE}" "${GUEST_GCC}" "${VENUS_GUEST_JOBS}"
          "${VENUS_GUEST_GPU}" "${linux_source}"
          "${VENUS_GUEST_MODULE_KEY}" "${nvidia_archive}" "${VENUS_GUEST_NVIDIA_SHA256}"
  DEPENDS venus-linux-source "${linux_stamp}" ${nvidia_archive} "${guest_build}/guest-inputs"
          "${guest_dir}/kernel/linux-7.3.3.config" "${guest_dir}/cmake/build-kernel.sh"
          ${kernel_module_inputs} ${VENUS_GUEST_MODULE_KEY}
  USES_TERMINAL VERBATIM)
add_custom_target(venus-guest-kernel DEPENDS "${guest_kernel_stamp}")

set(guest_files "${guest_artifacts}/Image" "${guest_artifacts}/guest.initrd"
                "${guest_artifacts}/venus-root.qcow2" "${guest_artifacts}/vram.qcow2"
                "${guest_artifacts}/SHA256SUMS" "${guest_artifacts}/builder-rpms.txt")
file(GLOB guest_runtime_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false "${guest_runtime}/*")
add_custom_command(
  OUTPUT ${guest_files}
  COMMAND "${CMAKE_COMMAND}" -E env "SOURCE_DATE_EPOCH=${VENUS_GUEST_SOURCE_DATE_EPOCH}"
          "${BASH_PROGRAM}" "${guest_dir}/cmake/build-images.sh"
          "${guest_dir}" "${guest_build}" "${guest_artifacts}" "${VENUS_GUEST_GPU}"
          "${VENUS_GUEST_VRAM_SIZE_MIB}" "${GUEST_QEMU_IMG}" "${Python3_EXECUTABLE}"
          "$<TARGET_FILE:venus-guest-renderer>" "${virgl_lib}" "${virgl_server}"
          "$<TARGET_FILE:venus-guest-bridge>" "$<TARGET_FILE:venus-guest-power>"
          "$<TARGET_FILE:venus-guest-bootstrap>" "$<TARGET_FILE:venus-guest-poweroff>"
  DEPENDS "${guest_kernel_stamp}" "${virgl_lib}" "${virgl_server}" "${guest_build}/guest-inputs"
          venus-guest-renderer venus-guest-bridge venus-guest-power venus-guest-bootstrap venus-guest-poweroff
          "${guest_dir}/cmake/build-images.sh" "${guest_dir}/cmake/label-ext4.py" ${guest_runtime_inputs}
  USES_TERMINAL VERBATIM)
add_custom_target(venus-guest ALL DEPENDS ${guest_files})
