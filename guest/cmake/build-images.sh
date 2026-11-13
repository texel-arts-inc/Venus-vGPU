#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
set -euo pipefail
source_dir=$1 build=$2 output=$3 gpu=$4 vram_mib=$5 qemu_img=$6 python=$7
renderer=$8 virgl_lib=$9 virgl_server=${10} pm_bridge=${11} gpu_power=${12}
bootstrap=${13} poweroff=${14}
export LC_ALL=C TZ=UTC
export E2FSPROGS_FAKE_TIME=$SOURCE_DATE_EPOCH
unset LD_LIBRARY_PATH

nvidia=$build/nvidia-installer
umask 022

if [[ $(id -u) != 0 ]]; then
    echo 'Build the guest images as root in the dedicated aarch64 Fedora builder (SELinux policy store and root-owned images).' >&2
    exit 1
fi

work=$(mktemp -d "$build/images.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
runtime=$work/runtime
early=$work/early
policy_root=$work/policy
result=$work/result
mkdir -p "$result" "$runtime" "$early" "$policy_root/etc/selinux" "$policy_root/store"
chmod 0755 "$runtime" "$early"

# Compile the Fedora targeted base plus the two Venus policy modules into a
# temporary store. The builder's SELinux settings and policy remain untouched.
cp -a /etc/selinux/config /etc/selinux/targeted "$policy_root/etc/selinux/"
cp -a /var/lib/selinux/targeted "$policy_root/store/"
semodule -N -p "$policy_root" -S /store -s targeted \
    -i "$source_dir/runtime/venus-pm.cil" "$source_dir/runtime/microvm-selinux.cil"
policy_dir=$policy_root/etc/selinux/targeted
policy=$(find "$policy_dir/policy" -maxdepth 1 -type f -name 'policy.*' -print | sort -V | tail -n 1)
test -f "$policy"

add_file() {
    local source=$1 destination=$2
    install -d "$image_tree$(dirname "$destination")"
    cp -L -- "$source" "$image_tree$destination"
}

add_elf() {
    local source=$1 destination=$2 linked dependency
    add_file "$source" "$destination"
    if ! linked=$(ldd "$source") || [[ $linked == *'not found'* ]]; then
        printf '%s\n' "$linked" >&2
        return 1
    fi
    while IFS= read -r dependency; do
        case "$dependency" in
            */libvirglrenderer.so.1) continue ;; # Staged above under its guest SONAME.
            "$nvidia/"*) continue ;; # The NVIDIA libraries below have explicit guest paths.
        esac
        add_file "$dependency" "$dependency"
    done < <(awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}' <<< "$linked")
}

make_usrmerge() {
    mkdir -p "$image_tree"/usr/{bin,sbin,lib,lib64}
    for path in bin sbin lib lib64; do
        ln -s "usr/$path" "$image_tree/$path"
    done
}

image_tree=$runtime
make_usrmerge
mkdir -p "$runtime"/{dev,proc,sys,run,etc/vulkan/icd.d,usr/lib/venus/modules,var/lib/venus-vram}
install -d -m 1777 "$runtime/tmp"
for command in bash mount umount insmod rmmod mknod chown mkdir cat setpriv sleep setfiles; do
    path=$(command -v "$command")
    add_elf "$path" "/usr/bin/$command"
done
add_elf "$virgl_lib" /usr/lib64/libvirglrenderer.so.1
export LD_LIBRARY_PATH="$(dirname "$virgl_lib")"
add_elf "$renderer" /usr/lib/venus/rv-renderer
add_elf "$virgl_server" /usr/libexec/virgl_render_server
unset LD_LIBRARY_PATH
add_elf "$pm_bridge" /usr/libexec/venus-pm-bridge
add_elf "$gpu_power" /usr/libexec/venus-gpu-power
add_elf "$poweroff" /usr/lib/venus/poweroff
add_elf /usr/lib64/libvulkan.so.1 /usr/lib64/libvulkan.so.1
add_elf /usr/lib64/libEGL.so.1 /usr/lib64/libEGL.so.1

install -m 0755 "$source_dir/runtime/microvm-init.sh" "$runtime/init"
install -m 0644 "$policy" "$runtime/etc/venus-policy"
install -m 0644 "$source_dir/runtime/microvm-device-contexts" "$runtime/etc/venus-device-contexts"
install -m 0644 "$build/guest-modules/"* "$runtime/usr/lib/venus/modules/"
printf '%s\n' "$gpu" > "$runtime/etc/venus-gpu"
printf 'root:x:0:0:root:/root:/bin/bash\nvenus:x:1000:1000:Venus:/run/venus:/bin/bash\n' > "$runtime/etc/passwd"
printf 'root:x:0:\nvenus:x:1000:\n' > "$runtime/etc/group"

if [[ $gpu == nvidia ]]; then
    version=620.70.06
    export LD_LIBRARY_PATH=$nvidia
    add_elf "$nvidia/libGLX_nvidia.so.$version" /usr/lib64/libGLX_nvidia.so.0
    add_elf "$nvidia/libEGL_nvidia.so.$version" /usr/lib64/libEGL_nvidia.so.0
    add_elf "$nvidia/libnvidia-ml.so.$version" /usr/lib64/libnvidia-ml.so.1
    for name in cfg eglcore glcore glsi glvkspirv gpucomp rtcore allocator; do
        add_elf "$nvidia/libnvidia-$name.so.$version" "/usr/lib64/libnvidia-$name.so.$version"
    done
    if [[ -f $nvidia/libnvidia-present.so.$version ]]; then
        add_elf "$nvidia/libnvidia-present.so.$version" "/usr/lib64/libnvidia-present.so.$version"
    fi
    unset LD_LIBRARY_PATH
    add_file "$nvidia/nvidia_icd.json" /etc/vulkan/icd.d/nvidia_icd.json
    add_file "$nvidia/10_nvidia.json" /usr/share/glvnd/egl_vendor.d/10_nvidia.json
    add_file "$nvidia/LICENSE" /usr/share/licenses/nvidia-driver/LICENSE
    while IFS= read -r firmware; do
        [[ -n $firmware ]] || continue
        source="$nvidia/firmware/$firmware"
        if [[ ! -f $source ]]; then
            source="$nvidia/firmware/${firmware##*/}"
        fi
        add_file "$source" "/usr/lib/firmware/$firmware"
    done < <(modinfo -F firmware "$build/guest-modules/nvidia.ko")
elif [[ $gpu == nouveau ]]; then
    icds=(/usr/share/vulkan/icd.d/nouveau_icd*.json)
    if [[ ${#icds[@]} != 1 || ! -f ${icds[0]} ]]; then
        echo 'Install Fedora aarch64 mesa-vulkan-drivers with the Nouveau/NVK ICD.' >&2
        exit 1
    fi
    add_file "${icds[0]}" /etc/vulkan/icd.d/nouveau_icd.json
    nvk_library=$("$python" -c 'import json, sys; print(json.load(open(sys.argv[1]))["ICD"]["library_path"])' "${icds[0]}")
    if [[ $nvk_library != /* ]]; then
        nvk_library="/usr/lib64/$nvk_library"
    fi
    add_elf "$nvk_library" "$nvk_library"
    declare -A firmware_destinations
    while IFS= read -r firmware; do
        [[ $firmware == nvidia/* ]] || continue
        for suffix in '' .xz .zst; do
            source="/usr/lib/firmware/$firmware$suffix"
            [[ -f $source ]] || continue
            destination="/usr/lib/firmware/$firmware$suffix"
            target=$(readlink -f "$source")
            if [[ ${firmware_destinations[$target]+present} ]]; then
                mkdir -p "$runtime$(dirname "$destination")"
                ln "$runtime${firmware_destinations[$target]}" "$runtime$destination"
            else
                add_file "$source" "$destination"
                firmware_destinations[$target]=$destination
            fi
            break
        done
    done < <(modinfo -F firmware "$build/guest-modules/nouveau.ko" | sort -u)
    if [[ ! -d $runtime/usr/lib/firmware/nvidia/ad102/gsp ]]; then
        echo 'Install Fedora nvidia-gpu-firmware with the Linux kernel version’s required Ada GSP firmware.' >&2
        exit 1
    fi
fi

image_tree=$early
make_usrmerge
mkdir -p "$early"/{dev,proc,sys}
for command in bash mount mkdir sleep; do
    path=$(command -v "$command")
    add_elf "$path" "/usr/bin/$command"
done
install -m 0755 "$source_dir/runtime/microvm-early-init.sh" "$early/init"
add_elf "$bootstrap" /bootstrap-policy

find "$runtime" "$early" -print0 | xargs -0 touch -h -d "@$SOURCE_DATE_EPOCH"
(
    cd "$early"
    find . -print0 | sort -z | cpio --null -o --format=newc --reproducible --owner=0:0 --quiet
) | zstd -q -T1 -3 -o "$result/guest.initrd"
zstd -t "$result/guest.initrd"

root_uuid=fbefbfbb-46f3-4041-a4cf-00e17d5f4132
vram_uuid=6077afdf-362b-4173-a162-a72b20486076
root_size_mib=$(du -sm "$runtime" | cut -f1)
root_size_mib=$((root_size_mib + 256))
truncate -s "${root_size_mib}M" "$work/venus-root.ext4"
mke2fs -q -t ext4 -m 0 -U "$root_uuid" -L venus-runtime \
    -E "root_owner=0:0,no_copy_xattrs,hash_seed=$root_uuid,lazy_itable_init=0,lazy_journal_init=0" \
    -d "$runtime" "$work/venus-root.ext4"
"$python" "$source_dir/cmake/label-ext4.py" "$work/venus-root.ext4" \
    --root "$runtime" --contexts "$policy_dir/contexts/files/file_contexts"
e2fsck -fn "$work/venus-root.ext4"

truncate -s "${vram_mib}M" "$work/venus-vram.ext4"
mke2fs -q -t ext4 -m 0 -U "$vram_uuid" -L venus-vram \
    -E "root_owner=0:0,hash_seed=$vram_uuid,lazy_itable_init=0,lazy_journal_init=0" "$work/venus-vram.ext4"
"$python" "$source_dir/cmake/label-ext4.py" "$work/venus-vram.ext4"
e2fsck -fn "$work/venus-vram.ext4"

"$qemu_img" convert -f raw -O qcow2 "$work/venus-root.ext4" "$result/venus-root.qcow2"
"$qemu_img" convert -f raw -O qcow2 "$work/venus-vram.ext4" "$result/vram.qcow2"
"$qemu_img" check "$result/venus-root.qcow2"
"$qemu_img" check "$result/vram.qcow2"
install -m 0644 "$build/kbuild/arch/arm64/boot/Image" "$result/Image"
rpm -qa --qf '%{NAME}-%{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}\n' | sort > "$result/builder-rpms.txt"
(
    cd "$result"
    sha256sum Image guest.initrd venus-root.qcow2 vram.qcow2 builder-rpms.txt > SHA256SUMS
)
mkdir -p "$output"
mv -f -- "$result/"* "$output/"
