# Venus guest

Build in a dedicated Fedora 45 aarch64 Linux VM or machine. The builder uses native GCC, Fedora userspace and the Fedora targeted SELinux policy. Allow at least 12 GiB of free build space for the Linux Git checkout, Kbuild files and image staging. Image assembly needs root access to the local policy store and creates root-owned filesystem contents. It compiles policy in a temporary copy of the store, and writes the kernel, initramfs and disk images to the CMake binary tree.

Install the builder tools and libraries.

```sh
sudo dnf install \
  cmake git-core gcc gcc-c++ make binutils bc bison flex perl \
  openssl openssl-devel elfutils-libelf-devel \
  python3 meson ninja-build pkgconf-pkg-config \
  libepoxy-devel mesa-libgbm-devel libdrm-devel vulkan-loader vulkan-headers \
  libglvnd-egl policycoreutils selinux-policy-targeted e2fsprogs cpio zstd kmod qemu-img
```

From the repository root, configure and build with a separate output directory.

```sh
cmake -S . -B /var/tmp/venus-guest-build -G Ninja \
  -DVENUS_BUILD_GUEST=ON -DVENUS_GUEST_GPU=none
sudo cmake --build /var/tmp/venus-guest-build --target venus-guest
```

The `venus-guest` target generates `guest/artifacts/Image`, `guest/artifacts/guest.initrd`, `guest/artifacts/venus-root.qcow2` and `guest/artifacts/vram.qcow2`. Use that directory as `VENUS_GUEST_ARTIFACTS_DIR` when packaging on macOS. The host supplies the base device tree from the matching QEMU topology. Run `sha256sum -c SHA256SUMS` in the output directory to check the four boot files and the builder RPM manifest.

## GPU profiles

- `VENUS_GUEST_GPU=none` is the default. It starts an enforcing maintenance guest without a Vulkan renderer or any NVIDIA driver. Its console reports `VENUS MICROVM BOOTED enforcing=1 gpu=none`.
- `VENUS_GUEST_GPU=nvidia` builds the NVIDIA-supplied open kernel modules against the guest kernel, signs them, and extracts the matching proprietary user-mode Vulkan libraries and firmware from the user's NVIDIA installer. This is the profile with explicit NVIDIA VRAM save and restore integration.
- `VENUS_GUEST_GPU=nouveau` selects the Linux Nouveau kernel driver and Fedora Mesa's NVK Vulkan ICD. Install `mesa-vulkan-drivers` and `nvidia-gpu-firmware` in the builder first. The Ada card uses NVIDIA GSP firmware from Fedora's firmware package. Apple PCIe passthrough, NVK rendering and SELinux behavior on this path are experimental. The guest reports the host's managed sleep request as unsupported because the NVIDIA-specific VRAM retention protocol is unavailable. [Mesa NVK documentation](https://docs.mesa3d.org/drivers/nvk.html)
- Nova is an experimental Rust GSP kernel driver under development. The guest configuration disables Nova and this project has no verified Nova/NVK or Nova passthrough path. [Linux Nova documentation](https://docs.kernel.org/gpu/nova/index.html)

For NVIDIA, read the installer license and obtain `NVIDIA-Linux-aarch64-620.70.06.run` directly from NVIDIA. If you accept the license, configure with these options in addition to `VENUS_BUILD_GUEST=ON`. [NVIDIA installer and license](https://download.nvidia.com/XFree86/Linux-aarch64/620.70.06/)

```sh
-DVENUS_GUEST_GPU=nvidia -DVENUS_GUEST_ACCEPT_NVIDIA_LICENSE=ON -DVENUS_GUEST_NVIDIA_INSTALLER=/absolute/path/NVIDIA-Linux-aarch64-620.70.06.run
```

To download that same installer during the build, substitute `-DVENUS_GUEST_DOWNLOAD_NVIDIA=ON` for the local installer path. Both inputs are checked against NVIDIA's published SHA-256. The resulting NVIDIA guest contains NVIDIA components and is for use under the accepted license. [NVIDIA's published checksum](https://download.nvidia.com/XFree86/Linux-aarch64/620.70.06/NVIDIA-Linux-aarch64-620.70.06.run.sha256sum)

## Build inputs

- Linux v7.3.3 is fetched from Git at commit `d50d994bffa3c7c6db2d8bc4c5f0c55e3f46b0cd`. A clean Git checkout at that commit can be supplied with `VENUS_GUEST_LINUX_SOURCE`. For the kernel.org `linux-7.3.3.tar.xz`, set `VENUS_GUEST_LINUX_ARCHIVE` to the file. CMake checks the release archive's SHA-256 before extraction. virglrenderer uses the Git revision and patch specified by the top-level CMake project. [Linux 7.3.3 release commit](https://www.kernel.org/pub/linux/kernel/v7.x/ChangeLog-7.3.3)
- Fedora's local packages and targeted policy are build inputs. Keep the builder's RPM snapshot and a clean, unmodified Fedora targeted policy store for repeat builds. The guest records installed package NEVRAs in `builder-rpms.txt`. `VENUS_GUEST_SOURCE_DATE_EPOCH` sets the image and kernel timestamps. Repeat builds should reuse the same key, toolchain, packages, epoch and options. Two image assembly runs with the same inputs on one builder produced matching hashes. Byte-for-byte equivalence across builders has not been verified.
- `VENUS_GUEST_MODULE_KEY` can specify an unencrypted private RSA key and X.509 certificate concatenated in a PEM. With the default setting the build generates and retains a local signing key under the binary tree. Keep the PEM private and reuse it across separate build directories when you need the same module trust root. The corresponding kernel enforces module signatures. The initramfs loads the guest policy and checks enforcing mode before starting the confined renderer.
- `VENUS_GUEST_JOBS` sets kernel build parallelism. `VENUS_GUEST_VRAM_SIZE_MIB` defaults to 24576 MiB of virtual scratch capacity. The installed QCOW2 begins sparse. NVIDIA's suspend mechanism needs scratch capacity for the entire GPU VRAM allocation.
