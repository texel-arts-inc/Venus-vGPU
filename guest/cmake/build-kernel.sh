#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
set -euo pipefail
source_dir=$1 build=$2 make=$3 gcc=$4 jobs=$5 gpu=$6 linux=$7
signing_key=$8 nvidia_archive=$9 nvidia_sha256=${10}
export LC_ALL=C TZ=UTC ARCH=arm64
export KBUILD_BUILD_USER=venus KBUILD_BUILD_HOST=builder KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_TIMESTAMP
KBUILD_BUILD_TIMESTAMP=$(date -u -d "@$SOURCE_DATE_EPOCH" '+%Y-%m-%d %H:%M:%S UTC')
export LOCALVERSION=

out=$build/kbuild
module_source=$build/venus-modules
module_stage=$build/guest-modules
full_module_stage=$build/full-modules
nvidia=$build/nvidia-installer
rm -f -- "$build/kernel-ready"
mkdir -p "$out/certs"
rm -rf -- "$module_source" "$module_stage" "$full_module_stage" "$nvidia"
mkdir -p "$module_stage" "$full_module_stage"
cp -R "$source_dir/kernel/modules" "$module_source"

# Keep a stable development key per build directory. An explicit combined PEM
# supplies both the private RSA key and the X.509 certificate for repeat builds.
if [[ -z $signing_key ]]; then
    signing_key=$build/module-signing.pem
    if [[ ! -f $signing_key ]]; then
        (
            umask 077
            openssl req -new -nodes -x509 -newkey rsa:4096 -sha256 -days 3650 \
                -subj '/CN=Venus guest local module signing/' -keyout "$signing_key.key" -out "$signing_key.crt"
            cat "$signing_key.key" "$signing_key.crt" > "$signing_key.tmp"
            rm -- "$signing_key.key" "$signing_key.crt"
            mv -- "$signing_key.tmp" "$signing_key"
        )
    fi
fi
openssl rsa -in "$signing_key" -passin pass: -check -noout
openssl x509 -in "$signing_key" -noout
if ! cmp -s "$signing_key" "$out/certs/venus-module.pem"; then
    install -m 0600 "$signing_key" "$out/certs/venus-module.pem"
fi
openssl x509 -in "$signing_key" -outform DER -out "$out/certs/venus-module.der"

cp "$source_dir/kernel/linux-7.3.3.config" "$out/.config"
config=("$linux/scripts/config" --file "$out/.config")
"${config[@]}" --set-str MODULE_SIG_KEY certs/venus-module.pem \
    --set-str SYSTEM_TRUSTED_KEYS "" \
    --disable MODULE_COMPRESS --disable MODULE_COMPRESS_ALL --disable RUST \
    --disable NOVA_CORE --disable NOVA_DRM --disable DRM_NOVA
if [[ $gpu == nouveau ]]; then
    "${config[@]}" --module DRM_NOUVEAU
else
    "${config[@]}" --disable DRM_NOUVEAU
fi

kbuild=("$make" -C "$linux" "O=$out" "CC=$gcc" "-j$jobs")
"${kbuild[@]}" olddefconfig
for feature in ARM64_4K_PAGES CONTIG_ALLOC NUMA BLK_DEV_INITRD RD_ZSTD DEVTMPFS \
    SERIAL_AMBA_PL011_CONSOLE VIRTIO_BLK VIRTIO_PCI VIRTIO_CONSOLE EXT4_FS \
    EXT4_FS_SECURITY SECURITY_SELINUX MODULE_SIG_FORCE MODULE_SIG_ALL MODULE_SIG_SHA256; do
    if ! grep -qx "CONFIG_$feature=y" "$out/.config"; then
        printf 'Required guest kernel feature CONFIG_%s did not resolve to y\n' "$feature" >&2
        exit 1
    fi
done
if [[ $gpu == nouveau ]] && ! grep -qx 'CONFIG_DRM_NOUVEAU=m' "$out/.config"; then
    echo 'Nouveau kernel configuration did not resolve to a module' >&2
    exit 1
fi
"${kbuild[@]}" Image modules
"${kbuild[@]}" "M=$module_source" modules

release=$(cat "$out/include/config/kernel.release")
if [[ $release != 7.3.3-applevfio ]]; then
    printf 'Unexpected Venus guest kernel release: %s\n' "$release" >&2
    exit 1
fi
module_root=$full_module_stage/lib/modules/$release
mkdir -p "$module_root/extra"
cp "$out"/modules.builtin* "$module_root/"
sign_module() {
    local file=$1
    strip --strip-debug "$file"
    "$out/scripts/sign-file" sha256 "$out/certs/venus-module.pem" "$out/certs/venus-module.der" "$file"
    test -n "$(modinfo -F signer "$file")"
    test "$(modinfo -F vermagic "$file" | cut -d' ' -f1)" = "$release"
}
for name in apple_dma venus_ivshmem rv_guest_pool; do
    install -m 0644 "$module_source/$name.ko" "$module_stage/"
    sign_module "$module_stage/$name.ko"
done

if [[ $gpu == nvidia ]]; then
    # The runfile comes from the explicit local/download license flow. Build
    # the open kernel modules included in the matching NVIDIA release.
    actual_sha256=$(sha256sum "$nvidia_archive" | cut -d' ' -f1)
    test "$actual_sha256" = "${nvidia_sha256,,}"
    bash "$nvidia_archive" --extract-only --target "$nvidia"
    "$make" -C "$nvidia/kernel-open" "-j$jobs" "CC=$gcc" \
        "SYSSRC=$linux" "SYSOUT=$out" NV_KERNEL_MODULES="nvidia nvidia-modeset nvidia-drm" modules
    for name in nvidia nvidia-modeset nvidia-drm; do
        install -m 0644 "$nvidia/kernel-open/$name.ko" "$module_root/extra/"
        sign_module "$module_root/extra/$name.ko"
    done
    target=nvidia_drm
elif [[ $gpu == nouveau ]]; then
    target=nouveau
fi

if [[ $gpu != none ]]; then
    # Resolve the same kernel release's DRM dependency closure. Only the
    # selected GPU driver and its dependencies are copied into the image.
    while IFS= read -r object; do
        file=${object%.o}.ko
        mkdir -p "$module_root/kernel/$(dirname "$file")"
        ln "$out/$file" "$module_root/kernel/$file"
    done < "$out/modules.order"
    depmod -b "$full_module_stage" "$release"
    dependencies=$(modprobe -C /dev/null -d "$full_module_stage" -S "$release" --show-depends "$target")
    while read -r kind file rest; do
        [[ $kind == insmod ]] || continue
        name=${file##*/}
        install -m 0644 "$file" "$module_stage/$name"
        if [[ $file != "$module_root/extra/"* ]]; then
            sign_module "$module_stage/$name"
        fi
        printf '%s\n' "$name" >> "$module_stage/gpu-load-list"
    done <<< "$dependencies"
    test -s "$module_stage/gpu-load-list"
fi

touch "$build/kernel-ready"
