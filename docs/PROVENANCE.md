# Source provenance

| Component | Revision or source |
| --- | --- |
| Mesa | `mesa-26.3.0`, commit `0a6fa60abb737d26ac8a316a4aad7c9ddf64f9e4` from [Mesa Git](https://gitlab.freedesktop.org/mesa/mesa) |
| Venus protocol | `v1.1.3`, commit `ca19b6358d7cc491bc3e4de76f04c6700876a8fa` from [Venus protocol Git](https://gitlab.freedesktop.org/virgl/venus-protocol) |
| virglrenderer | Commit `5d26f605f50f8e22002ec6db5fb775e1992d4e96` from [UTM virglrenderer](https://github.com/utmapp/virglrenderer) |
| QEMU | `v11.1.3`, commit `005c61c0ef6f6ae615283440b4bb7be51b2c6ddd` from [QEMU Git](https://github.com/qemu/qemu) |
| Apple VFIO | DriverKit and Apple DMA source derived from [Scott Goldman's QEMU VFIO Apple port](https://github.com/scottjg/qemu-vfio-apple/tree/335a6f24229a5c985b63ea8ab001ae088786e3a4) |
| Linux | `v7.3.3`, commit `d50d994bffa3c7c6db2d8bc4c5f0c55e3f46b0cd` from [Linux stable Git](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git) |
| NVIDIA reference driver | `620.70.06` from [NVIDIA's arm64 release directory](https://download.nvidia.com/XFree86/Linux-aarch64/620.70.06/) |
| Vulkan CTS (optional test patch) | `vulkan-cts-1.4.7.1`, commit `0d18b347943e5e843529a05c95220e27fe30c538` from [Khronos VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) |
| Ollama | `v0.40.16`, commit `d63b702609c4300a2ca07be2bdb0adb9834725e5` from [Ollama Git](https://github.com/ollama/ollama) |
| llama.cpp for Ollama | `b12177`, commit `6c01da6278ab6c69ed67d9124806b50d2b299ba7` from [llama.cpp Git](https://github.com/ggml-org/llama.cpp) |

Upstream components keep their original licenses and copyright notices. The
patches record the source modifications applied by CMake. A binary release
needs the corresponding source and build scripts for its GPL components
and the notices required by each bundled dependency. Publish source
alongside any binary release and keep local signing keys and credentials
outside the repository.

The public source archive contains source and build instructions. It
does not carry a blanket relicensing of third-party code or the NVIDIA
driver. First-party source files without a license notice need a grant from
their rights holder before public redistribution. The development
installer uses a local signing path. Distribution on normal macOS systems
requires an appropriately provisioned and notarized DriverKit release.

The Apple DMA source includes a UVM allocation kprobe for the reference
NVIDIA guest. Document and review that integration separately from the
unmodified NVIDIA driver files if considering a binary release that
bundles NVIDIA components.

## License scope

| Source path | License scope |
| --- | --- |
| `cmake/`, top-level CMake, and original Cornell demo | Apache-2.0 WITH LLVM-exception for files marked with that SPDX expression |
| `patches/mesa/` | Each affected Mesa file's terms. New files with an existing notice retain it. |
| `patches/virglrenderer/` | The virglrenderer source terms, including the applicable MIT notices. |
| `patches/qemu/`, `host/qemu/pm/` | QEMU's applicable GPL terms and the terms of the respective source files. |
| `patches/vulkan-cts/` | Apache-2.0, as in the affected Khronos CTS source. |
| `patches/ollama/` | MIT, as in the upstream Ollama source. |
| `guest/kernel/modules/` | The SPDX expression in each source file, including the Linux syscall exception on the UAPI header. |
| `host/app/` and `host/driver/` | Apple VFIO-derived files keep their upstream terms. The pinned fork's top-level license sets GPL-2.0-or-later as the fallback for its unmarked sources. Review any historical additions with unclear provenance. |
| Historical guest renderer, launcher, and policy sources | Existing notices and upstream terms. Historical files without a traceable license grant need ownership and license confirmation. |

The license texts live in [LICENSES](../LICENSES). The
[NOTICE](../NOTICE) lists projects, upstream contributors, and the demo's
scene reference.
