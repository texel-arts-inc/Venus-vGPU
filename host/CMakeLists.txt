# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(ExternalProject)
find_package(Git REQUIRED)
find_executable(VENUS_XCRUN NAMES xcrun REQUIRED)
find_executable(VENUS_XCODEBUILD NAMES xcodebuild REQUIRED)
find_executable(VENUS_NINJA NAMES ninja REQUIRED)
find_executable(VENUS_PYTHON NAMES python3 REQUIRED)

set(VENUS_APP_ID com.texel-arts.venus-vgpu)
set(VENUS_DRIVER_ID "${VENUS_APP_ID}.driver")
set(VENUS_MACOS_MIN_VERSION "26.2" CACHE STRING "macOS deployment target for Venus.app")
set(VENUS_BUNDLE_VERSION "1" CACHE STRING "CFBundleVersion for Venus.app and its DriverKit extension")
set(VENUS_GUEST_ARTIFACTS_DIR "" CACHE PATH "Directory containing the Linux guest artifacts")
set(VENUS_QEMU_PREFIX "" CACHE PATH "Optional existing patched and relocated QEMU runtime prefix")
set(VENUS_QEMU_ARCHIVE "" CACHE FILEPATH "Optional local QEMU 11.1.3 release archive")
set(VENUS_DTC_ARCHIVE "" CACHE FILEPATH "Optional local archive of the pinned DTC commit")
set(VENUS_BASE_DTB "" CACHE FILEPATH "Optional base.dtb generated with the Venus VM memory topology")
set(VENUS_APP_SIGN_IDENTITY "-" CACHE STRING "macOS app and DriverKit signing identity")
set(VENUS_INSTALLER_SIGN_IDENTITY "" CACHE STRING "Optional macOS installer signing identity")
set(VENUS_APP_PROVISIONING_PROFILE "" CACHE FILEPATH "Optional app provisioning profile")
set(VENUS_DRIVER_PROVISIONING_PROFILE "" CACHE FILEPATH "Optional DriverKit provisioning profile")
set(VENUS_DRIVER_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/driver/Entitlements.plist"
    CACHE FILEPATH "DriverKit entitlements covered by the signing profile")

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "The Venus host targets require Apple Silicon.")
endif()

set(app_executable "${CMAKE_CURRENT_BINARY_DIR}/app/Venus")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/app")
configure_file(app/Info.plist.in app/Info.plist @ONLY)
set(app_sources
    app/ContentView.swift app/MoveToApplications.swift app/QEMUBundleSupport.swift
    app/SystemExtensionActivationManager.swift app/VenusMicroVM.swift app/VFIOUserHostAppApp.swift)
add_custom_command(OUTPUT "${app_executable}"
    COMMAND "${VENUS_XCRUN}" --sdk macosx swiftc -O -target "arm64-apple-macosx${VENUS_MACOS_MIN_VERSION}"
        -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/app/module-cache"
        -module-name Venus ${app_sources} -o "${app_executable}"
    DEPENDS ${app_sources} "${CMAKE_CURRENT_BINARY_DIR}/app/Info.plist"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
add_custom_target(venus-app DEPENDS "${app_executable}")

set(driver_source "${CMAKE_CURRENT_SOURCE_DIR}/driver")
set(driver_bundle "${CMAKE_CURRENT_BINARY_DIR}/driver/Release-driverkit/${VENUS_DRIVER_ID}.dext")
add_custom_command(OUTPUT "${driver_bundle}/${VENUS_DRIVER_ID}"
    COMMAND "${VENUS_XCODEBUILD}" -quiet -project "${driver_source}/VFIOUserPCIDriver.xcodeproj"
        -scheme VFIOUserPCIDriver -configuration Release
        -derivedDataPath "${CMAKE_CURRENT_BINARY_DIR}/driver-derived"
        SDKROOT=driverkit ARCHS=arm64
        CODE_SIGNING_ALLOWED=NO "PRODUCT_BUNDLE_IDENTIFIER=${VENUS_DRIVER_ID}" "PRODUCT_NAME=${VENUS_DRIVER_ID}"
        "MARKETING_VERSION=${PROJECT_VERSION}" "CURRENT_PROJECT_VERSION=${VENUS_BUNDLE_VERSION}"
        "INFOPLIST_FILE=${driver_source}/Info.plist" "SYMROOT=${CMAKE_CURRENT_BINARY_DIR}/driver"
        "OBJROOT=${CMAKE_CURRENT_BINARY_DIR}/driver-obj"
        "CLANG_MODULE_CACHE_PATH=${CMAKE_CURRENT_BINARY_DIR}/driver-module-cache"
    DEPENDS driver/Info.plist driver/VFIOUserPCIDriver.xcodeproj/project.pbxproj
        driver/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp driver/VFIOUserPCIDriver/VFIOUserPCIDriver.iig
        driver/VFIOUserPCIDriver/VFIOUserPCIDriverUserClient.iig
    VERBATIM)
add_custom_target(venus-driver DEPENDS "${driver_bundle}/${VENUS_DRIVER_ID}")

set(dtc_source "${CMAKE_CURRENT_BINARY_DIR}/dtc-src")
if(VENUS_DTC_ARCHIVE)
    set(dtc_fetch URL "${VENUS_DTC_ARCHIVE}"
        URL_HASH SHA256=e115f987eec23a1ba25150a46ced1675de3716072d3b4905afb3a9cda0f007c7
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
else()
    set(dtc_fetch GIT_REPOSITORY https://github.com/dgibson/dtc.git
        GIT_TAG b6910bec11614980a21e46fbccc35934b671bd81)
endif()
set(fdt_dir "${dtc_source}/libfdt")
set(fdt_sources)
foreach(src IN ITEMS fdt fdt_addresses fdt_check fdt_empty_tree fdt_overlay
        fdt_ro fdt_rw fdt_strerror fdt_sw fdt_wip)
    list(APPEND fdt_sources "${fdt_dir}/${src}.c")
endforeach()
ExternalProject_Add(venus-dtc EXCLUDE_FROM_ALL TRUE ${dtc_fetch}
    SOURCE_DIR "${dtc_source}" UPDATE_DISCONNECTED TRUE
    CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${fdt_sources})
set_source_files_properties(${fdt_sources} PROPERTIES GENERATED TRUE)
add_library(venus-fdt STATIC EXCLUDE_FROM_ALL ${fdt_sources})
target_include_directories(venus-fdt PUBLIC "${fdt_dir}")
add_dependencies(venus-fdt venus-dtc)
find_library(VENUS_COREFOUNDATION CoreFoundation REQUIRED)
find_library(VENUS_IOKIT IOKit REQUIRED)
add_executable(venus-vgpud EXCLUDE_FROM_ALL launcher/venus-vgpud.c launcher/venus-vgpu-dtb.c)
target_link_libraries(venus-vgpud PRIVATE venus-fdt "${VENUS_COREFOUNDATION}" "${VENUS_IOKIT}")
set_target_properties(venus-vgpud PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES OSX_ARCHITECTURES arm64)

set(qemu_package_args)
if(VENUS_QEMU_PREFIX)
    add_custom_target(venus-qemu)
    list(APPEND qemu_package_args --qemu-prefix "${VENUS_QEMU_PREFIX}")
else()
    find_package(PkgConfig REQUIRED)
    if(CMAKE_CURRENT_BINARY_DIR MATCHES "[ :]")
        message(FATAL_ERROR "QEMU requires a build path without spaces or colons.")
    endif()
    pkg_check_modules(VENUS_QEMU_GLIB REQUIRED glib-2.0>=2.66)
    pkg_check_modules(VENUS_QEMU_PIXMAN REQUIRED pixman-1)
    if(VENUS_QEMU_ARCHIVE)
        set(qemu_fetch URL "${VENUS_QEMU_ARCHIVE}"
            URL_HASH SHA256=5a8289c7940dc77d3873c81a6c29ba3afe1446fc9d36e77fa1658cdbecb84b22
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    else()
        set(qemu_fetch GIT_REPOSITORY https://github.com/qemu/qemu.git
            GIT_TAG v11.1.3 GIT_SHALLOW TRUE)
    endif()
    set(qemu_src "${CMAKE_CURRENT_BINARY_DIR}/qemu-src")
    set(qemu_dependencies venus-dtc)
    if(NOT VENUS_QEMU_ARCHIVE)
        foreach(spec IN ITEMS
                "keycodemapdb=f5772a62ec52591ff6870b7e8ef32482371f22c6"
                "berkeley-softfloat-3=b64af41c3276f97f0e181920400ee056b9c88037"
                "berkeley-testfloat-3=e7af9751d9f9fd3b47911f51a5cfd08af256a9ab")
            string(REPLACE "=" ";" fields "${spec}")
            list(GET fields 0 name)
            list(GET fields 1 revision)
            set(repository "https://github.com/qemu/${name}.git")
            if(name STREQUAL "berkeley-testfloat-3")
                set(repository "https://gitlab.com/qemu-project/berkeley-testfloat-3.git")
            endif()
            ExternalProject_Add("venus-qemu-${name}" EXCLUDE_FROM_ALL TRUE
                GIT_REPOSITORY "${repository}" GIT_TAG "${revision}"
                GIT_SUBMODULES "" UPDATE_DISCONNECTED TRUE
                SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/qemu-subprojects/${name}"
                CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "")
            list(APPEND qemu_dependencies "venus-qemu-${name}")
        endforeach()
    endif()
    set(qemu_build "${CMAKE_CURRENT_BINARY_DIR}/qemu-build")
    set(qemu_patches "${PROJECT_SOURCE_DIR}/patches/qemu")
    set(patch_series upstream-port.patch darwin-vfio-port.patch apple-vfio-bar-teardown.patch
        darwin-ivshmem.patch qemu-vfio-pm.patch venus-headless.patch)
    file(GLOB pm_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qemu/pm/*.[ch]")
    string(JOIN "|" patch_argument ${patch_series})
    string(JOIN "|" pm_argument ${pm_sources})
    # QEMU's configure expands the Python command as shell words.
    file(CREATE_LINK "${VENUS_PYTHON}" "${CMAKE_CURRENT_BINARY_DIR}/qemu-python" SYMBOLIC)
    file(CREATE_LINK "${VENUS_NINJA}" "${CMAKE_CURRENT_BINARY_DIR}/qemu-ninja" SYMBOLIC)
    ExternalProject_Add(venus-qemu EXCLUDE_FROM_ALL TRUE ${qemu_fetch}
        DEPENDS ${qemu_dependencies} UPDATE_DISCONNECTED TRUE GIT_SUBMODULES ""
        SOURCE_DIR "${qemu_src}" BINARY_DIR "${qemu_build}"
        LIST_SEPARATOR "|"
        PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" "-DGIT_TOOL=${GIT_EXECUTABLE}"
            "-DPATCH_DIR=${qemu_patches}" "-DDTC_DIR=${dtc_source}"
            "-DSUBPROJECTS=${CMAKE_CURRENT_BINARY_DIR}/qemu-subprojects"
            "-DPATCHES=${patch_argument}"
            "-DPM_SOURCES=${pm_argument}" -P "${CMAKE_CURRENT_SOURCE_DIR}/qemu/ApplyPatches.cmake"
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "PIP_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/pip-cache"
            "PKG_CONFIG=${PKG_CONFIG_EXECUTABLE}"
            <SOURCE_DIR>/configure --target-list=aarch64-softmmu --without-default-features
            --enable-hvf --enable-tcg --enable-tools --enable-pixman --disable-guest-agent --disable-docs
            --enable-fdt=internal "--python=${CMAKE_CURRENT_BINARY_DIR}/qemu-python"
            "--ninja=${CMAKE_CURRENT_BINARY_DIR}/qemu-ninja" --extra-ldflags=-Wl,-headerpad_max_install_names
        BUILD_COMMAND "${VENUS_NINJA}" -C <BINARY_DIR>
            qemu-system-aarch64 qemu-img contrib/ivshmem-server/ivshmem-server
        BUILD_BYPRODUCTS "${qemu_build}/qemu-system-aarch64" "${qemu_build}/qemu-img"
            "${qemu_build}/contrib/ivshmem-server/ivshmem-server"
        INSTALL_COMMAND "")
    ExternalProject_Add_StepDependencies(venus-qemu patch
        "${CMAKE_CURRENT_SOURCE_DIR}/qemu/ApplyPatches.cmake" ${pm_sources})
    foreach(name IN LISTS patch_series)
        ExternalProject_Add_StepDependencies(venus-qemu patch "${qemu_patches}/${name}")
    endforeach()
    list(APPEND qemu_package_args --qemu-source "${qemu_src}" --qemu-build "${qemu_build}")
endif()

set(package_file "${CMAKE_BINARY_DIR}/Venus vGPU.pkg")
add_custom_target(venus-package
    COMMAND "${VENUS_PYTHON}" -B "${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.py"
        --output "${package_file}" --version "${PROJECT_VERSION}" --identifier "${VENUS_APP_ID}"
        --app "${app_executable}" --app-info "${CMAKE_CURRENT_BINARY_DIR}/app/Info.plist"
        --app-entitlements "${CMAKE_CURRENT_SOURCE_DIR}/app/Entitlements.plist"
        "--app-profile=${VENUS_APP_PROVISIONING_PROFILE}" --driver "${driver_bundle}"
        --driver-entitlements "${VENUS_DRIVER_ENTITLEMENTS}"
        "--driver-profile=${VENUS_DRIVER_PROVISIONING_PROFILE}" --launcher "$<TARGET_FILE:venus-vgpud>"
        --mesa-licenses "${VENUS_MESA_SOURCE_DIR}/licenses"
        --framework "${VENUS_FRAMEWORK_DIR}" "--guest=${VENUS_GUEST_ARTIFACTS_DIR}"
        "--base-dtb=${VENUS_BASE_DTB}" "--identity=${VENUS_APP_SIGN_IDENTITY}"
        "--installer-identity=${VENUS_INSTALLER_SIGN_IDENTITY}"
        ${qemu_package_args}
    DEPENDS venus-app venus-driver venus-vgpud venus-qemu venus-framework
    BYPRODUCTS "${package_file}" USES_TERMINAL VERBATIM)
find_executable(VENUS_HDIUTIL NAMES hdiutil REQUIRED)
add_custom_target(venus-dmg
    COMMAND "${VENUS_HDIUTIL}" create -ov -format UDZO -volname "Venus vGPU"
        -srcfolder "${package_file}" "${CMAKE_BINARY_DIR}/Venus vGPU.dmg"
    DEPENDS venus-package USES_TERMINAL VERBATIM)
