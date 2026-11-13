# Venus vGPU

Venus vGPU exposes a Thunderbolt GPU to macOS Vulkan applications through a
small Linux VM. The macOS Vulkan ICD sends Venus commands over IVSHMEM.
QEMU assigns the GPU to Linux through DriverKit. The guest renderer calls the
Vulkan driver installed in Linux.

## Build

CMake is the entry point. It fetches pinned upstream Git revisions and
applies the files under [patches](patches). Mesa and virglrenderer use Meson
internally, QEMU uses its configure and Ninja build, and Linux uses Kbuild.
Use separate build directories for macOS and Linux aarch64.

### macOS framework

Install Xcode, CMake 3.24 or newer, Git, Ninja, Meson, pkg-config, and Python
3.10 or newer with Mako and PyYAML. Put the Meson/Python environment on PATH.
The Vulkan loader is needed to run Vulkan applications.

```sh
cmake -S . -B build-host -G Ninja -DVENUS_BUILD_HOST=ON
cmake --build build-host --target venus-framework
```

The ICD manifest is at
`build-host/framework/Venus.framework/Resources/icd.d/Venus_icd.json`.
Use `VK_DRIVER_FILES` to select it. A running Venus guest is required for
physical-device enumeration.

### Guest and standalone installer

Build the guest on Linux aarch64. See [Guest build](guest/README.md) for
dependencies, signing, and driver options. The macOS
[host build](host/README.md) consumes the four guest artifacts, generates
the matching device tree, and packages the private QEMU runtime.

The public source tree contains no NVIDIA driver payload. To build an
NVIDIA guest, review NVIDIA's terms and pass the arm64 installer to CMake,
or explicitly enable the vendor download described in the guest instructions.
Keep locally built NVIDIA guest images out of public release assets.

### Cornell box

Build the [Cornell box](demo/README.md) with the Vulkan loader and headers,
SDL3, and glslang. The demo uses one scene for vertex rasterization, mesh
rasterization, and ray tracing through Vulkan ray queries.

```sh
cmake -S . -B build-demo -G Ninja -DVENUS_BUILD_HOST=OFF -DVENUS_BUILD_DEMO=ON
cmake --build build-demo
```

## Guest driver choices

| Selection | Guest driver | Status |
| --- | --- | --- |
| `none` | No NVIDIA payload | Default build option |
| `nvidia` | NVIDIA Linux arm64 Vulkan and matching kernel modules | RTX 4000 SFF Ada reference configuration |
| `nouveau` | Nouveau with Mesa NVK and matching firmware | Experimental on this VM |

[NVK](https://docs.mesa3d.org/drivers/nvk.html) supports Ada, and the pinned
Mesa 26.3.0 source uses Nouveau's DRM interface for NVK. The open-source
driver stack still needs firmware for Ada. Check the firmware license
before including it in a release. [Nova](https://docs.kernel.org/7.2/gpu/nova/index.html)
is a separate kernel-driver project. Its interface is outside the pinned
NVK build and this repo's tested GPU configurations.

Use **Disconnect GPU** in the Venus app and wait for **GPU released** before
physically unplugging the enclosure. The runtime uses direct PCI BAR mappings.

See [source provenance and release notes](docs/PROVENANCE.md).
See [public release requirements](docs/RELEASING.md) for attribution,
corresponding source, and DriverKit signing.
