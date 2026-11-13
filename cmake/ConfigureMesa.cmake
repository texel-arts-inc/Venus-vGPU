# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set(mode)
if(EXISTS "${BINARY_DIR}/meson-private/coredata.dat")
    set(mode --reconfigure)
endif()
execute_process(
    COMMAND "${MESON}" setup ${mode} --wrap-mode=nodownload --buildtype=release
        "${BINARY_DIR}" "${SOURCE_DIR}"
        -Dvulkan-drivers=virtio -Dgallium-drivers= -Dplatforms=macos
        -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false
        -Dllvm=disabled -Dvideo-codecs= -Dbuild-tests=false -Dvalgrind=disabled
        -Dlibunwind=disabled -Dzstd=disabled -Dspirv-tools=disabled -Db_lundef=false
        -Dcpp_link_args=-Wl,-undefined,dynamic_lookup -Db_ndebug=if-release
    COMMAND_ERROR_IS_FATAL ANY
)
