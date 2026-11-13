#!/bin/bash
set -eu

export PATH=/usr/bin:/usr/sbin
export LD_LIBRARY_PATH=/usr/lib64:/usr/lib
export HOME=/run/venus
export XDG_CACHE_HOME=/run/venus/cache

vram=/var/lib/venus-vram
read -r gpu </etc/venus-gpu

shutdown() {
    trap - ERR
    trap '' TERM INT
    for pid in "${console_reader:-}" "${bridge:-}" "${renderer:-}"; do
        [[ -n $pid ]] || continue
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${console_reader:-}" "${bridge:-}" "${renderer:-}"; do
        [[ -n $pid ]] || continue
        wait "$pid" 2>/dev/null || true
    done
    case "$gpu" in
        nvidia) rmmod nvidia_drm nvidia_modeset nvidia 2>/dev/null || true ;;
        nouveau) rmmod nouveau 2>/dev/null || true ;;
    esac
    if [[ ${vram_mounted:-} ]]; then
        umount "$vram" || echo "Failed to unmount Venus VRAM scratch"
    fi
    exec /usr/lib/venus/poweroff
}

startup_failed() {
    echo "Venus microVM runtime startup failed."
    shutdown
}

trap startup_failed ERR
trap shutdown TERM INT

setfiles /etc/venus-device-contexts /dev/console /dev/null /dev/random /dev/urandom 0<&- 1>&- 2>&-
exec </dev/console >/dev/console 2>&1
read -r -N 1 enforcing </sys/fs/selinux/enforce
test "$enforcing" = 1
mount -t tmpfs -o mode=0755,rootcontext=system_u:object_r:var_run_t:s0 tmpfs /run
mkdir -p -m 0700 "$HOME" "$vram"
mkdir -m 0700 "$XDG_CACHE_HOME"
chown -R 1000:1000 "$HOME"
setfiles /etc/venus-device-contexts "$HOME"


# Linux uses the key compiled into the matching kernel Image.
test "$(cat /sys/module/module/parameters/sig_enforce)" = Y

for ((attempt = 0; attempt < 100; attempt++)); do
    [[ -b /dev/vda ]] && break
    sleep 0.1
done
mount -t ext4 -o nosuid,nodev,noexec /dev/vda "$vram"
vram_mounted=1
echo "Venus microVM mounted VRAM scratch"

if [[ $gpu == none ]]; then
    echo 'VENUS MICROVM BOOTED enforcing=1 gpu=none'
    echo 'Type poweroff on the console to stop the VM.'
    while IFS= read -r line; do
        [[ $line != poweroff ]] || shutdown
    done
    shutdown
fi

pm_port=
for ((attempt = 0; attempt < 100; attempt++)); do
    for name in /sys/class/virtio-ports/vport*/name; do
        [[ -r $name ]] || continue
        [[ $(cat "$name") == com.texel-arts.venus-vgpu.pm ]] || continue
        device=${name%/name}
        pm_port=/dev/${device##*/}
        break
    done
    [[ -c $pm_port ]] && break
    sleep 0.1
done
test -c "$pm_port"
setfiles /etc/venus-device-contexts "$pm_port"
echo "Venus microVM found PM port $pm_port"

if [[ $gpu == nouveau ]]; then
    # The Apple DMA transport is shared. Its NVIDIA-specific code patches
    # apply only to the NVIDIA-supplied driver.
    insmod /usr/lib/venus/modules/apple_dma.ko enable_quirks=0
else
    insmod /usr/lib/venus/modules/apple_dma.ko
fi
insmod /usr/lib/venus/modules/venus_ivshmem.ko
insmod /usr/lib/venus/modules/rv_guest_pool.ko
while IFS= read -r module; do
    case "$module" in
        nvidia.ko)
            insmod "/usr/lib/venus/modules/$module" \
                NVreg_PreserveVideoMemoryAllocations=1 NVreg_UseKernelSuspendNotifiers=0 \
                NVreg_TemporaryFilePath="$vram"
            ;;
        nvidia-drm.ko)
            insmod "/usr/lib/venus/modules/$module" modeset=1
            ;;
        *)
            insmod "/usr/lib/venus/modules/$module"
            ;;
    esac
done </usr/lib/venus/modules/gpu-load-list

gpu_nodes=()
case "$gpu" in
    nvidia)
        export VK_DRIVER_FILES=/etc/vulkan/icd.d/nvidia_icd.json
        test -e /dev/nvidia0 || mknod -m 0660 /dev/nvidia0 c 195 0
        test -e /dev/nvidiactl || mknod -m 0660 /dev/nvidiactl c 195 255
        test -e /dev/nvidia-modeset || mknod -m 0660 /dev/nvidia-modeset c 195 254
        gpu_nodes=(/dev/nvidia0 /dev/nvidiactl /dev/nvidia-modeset)
        ;;
    nouveau)
        export VK_DRIVER_FILES=/etc/vulkan/icd.d/nouveau_icd.json
        shopt -s nullglob
        render_nodes=(/dev/dri/renderD*)
        test "${#render_nodes[@]}" -gt 0
        shopt -u nullglob
        ;;
    *)
        echo "Unknown guest GPU profile: $gpu"
        startup_failed
        ;;
esac
chown 1000:1000 /dev/venus-ivshmem /dev/rv-guest-pool "${gpu_nodes[@]}"
setfiles /etc/venus-device-contexts /dev/venus-ivshmem /dev/rv-guest-pool "${gpu_nodes[@]}"
for node in /dev/dri/card* /dev/dri/renderD*; do
    if [[ -c $node ]]; then
        chown 1000:1000 "$node"
        setfiles /etc/venus-device-contexts "$node"
    fi
done
if [[ $gpu == nvidia ]] && ! : <>/dev/nvidia0; then
    echo "NVIDIA driver could not open the GPU. See the NVRM messages above."
    startup_failed
fi

VENUS_RENDERER_PM=1 RENDER_SERVER_EXEC_PATH=/usr/libexec/virgl_render_server \
setpriv --reuid 1000 --regid 1000 --clear-groups \
    /usr/lib/venus/rv-renderer /dev/venus-ivshmem &
renderer=$!

for ((attempt = 0; attempt < 200; attempt++)); do
    kill -0 "$renderer"
    [[ -S $HOME/renderer-pm.sock ]] && break
    sleep 0.1
done
test -S "$HOME/renderer-pm.sock"

/usr/libexec/venus-pm-bridge --microvm "$pm_port" "$gpu" &
bridge=$!
bridge_open=
for ((attempt = 0; attempt < 100; attempt++)); do
    kill -0 "$bridge"
    for fd in /proc/"$bridge"/fd/*; do
        if [[ $fd -ef $pm_port ]]; then
            bridge_open=1
            break
        fi
    done
    [[ $bridge_open ]] && break
    sleep 0.1
done
test -n "$bridge_open"
kill -0 "$renderer" "$bridge"

(
    while IFS= read -r line; do
        if [[ $line == poweroff ]]; then
            kill -TERM 1
            break
        fi
    done
) </dev/console &
console_reader=$!

printf '%s\n' "VENUS MICROVM READY enforcing=1 gpu=$gpu" 'Type poweroff on the console to stop the VM.'
wait -n "$renderer" "$bridge" || true
echo "Venus microVM renderer or PM bridge exited."
shutdown
