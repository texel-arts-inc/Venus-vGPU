# macOS host build

Build on Apple Silicon with Xcode and its macOS and DriverKit SDKs, CMake
3.24 or newer, Git, Ninja, Meson, pkg-config, and Python 3.10 or newer. Put
the tools and Python's Mako and PyYAML modules on the build environment's
search path. The QEMU build also needs the GLib and pixman development
packages exposed to pkg-config and uses Python's venv and pip. Give QEMU a
build directory without spaces or colons.

From the source root, configure CMake with the Linux
[guest build output](../guest/README.md).

```sh
cmake -S . -B build-host -G Ninja \
  -DVENUS_GUEST_ARTIFACTS_DIR=/absolute/path/to/build-guest/guest/artifacts
cmake --build build-host --target venus-package
```

The target produces `build-host/Venus vGPU.pkg`. The package installs
`/Applications/Venus.app` and `/Library/Frameworks/Venus.framework`. The
app carries a private headless arm64 QEMU runtime, `qemu-img`, and the
Darwin IVSHMEM server. Packaging follows the Mach-O dependency closure,
rewrites dependency paths into the app, and checks the bundled QEMU's
VFIO, DMA, IVSHMEM, and power-management interfaces. It generates
`base.dtb` from the same two-node, 3 GiB memory topology as the launcher.
Device tree generation uses HVF with no physical device attached and exits
before booting Linux. The optional `venus-dmg` target wraps the package.

Individual build targets are `venus-app` for the SwiftUI executable,
`venus-driver` for the DriverKit extension, `venus-vgpud` for the
microVM launcher, `venus-qemu` for the QEMU runtime, and `venus-framework`
for the Vulkan ICD. QEMU and libfdt default to pinned Git source. The
`VENUS_QEMU_ARCHIVE` and `VENUS_DTC_ARCHIVE` cache entries accept the
hash-verified release archives. `VENUS_QEMU_PREFIX` accepts an existing
relocated 11.1.3 runtime with the matching Venus VFIO and IVSHMEM changes
for local package validation. `VENUS_BASE_DTB` accepts a device tree
generated with the same HVF, CPU and memory configuration.

## Activation and signing

The default signing identity is ad hoc, for local build and package
inspection. For deployment, supply `VENUS_APP_SIGN_IDENTITY` and the
app and DriverKit provisioning profiles with
`VENUS_APP_PROVISIONING_PROFILE` and `VENUS_DRIVER_PROVISIONING_PROFILE`.
The Apple-issued entitlements and provisioning profiles must cover the
bundle IDs `com.texel-arts.venus-vgpu` and
`com.texel-arts.venus-vgpu.driver`. If your provisioned DriverKit
entitlements differ, pass the plist with `VENUS_DRIVER_ENTITLEMENTS`.
`VENUS_INSTALLER_SIGN_IDENTITY` selects the optional installer signing
identity. Notarize and staple a distribution build before shipping it.

After installing a provisioned build, open Venus from Applications,
activate the system extension, and use **Start Venus** with the GPU
connected. **Disconnect GPU** stops the guest and checks for DriverKit
release before telling you the enclosure can be disconnected.

The NVIDIA-free maintenance guest verifies the guest boot path. To serve
Vulkan, package a guest with a compatible GPU renderer and driver.
Keep vendor driver and firmware artifacts subject to their individual
redistribution terms. The package includes project notices and license
texts for the source QEMU build's GLib, pixman, libintl, PCRE2 and libfdt
dependencies. If your development libraries introduce another private
Mach-O dependency, include its corresponding notices and source
information before distributing a binary package. See the project
[source provenance](../docs/PROVENANCE.md).
