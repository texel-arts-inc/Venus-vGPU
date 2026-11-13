#!/bin/bash
set -eu
export PATH=/usr/bin:/usr/sbin
export LD_LIBRARY_PATH=/usr/lib64:/usr/lib
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t selinuxfs selinuxfs /sys/fs/selinux
read -r signature </sys/module/module/parameters/sig_enforce
test "$signature" = Y
mkdir /newroot
for ((attempt = 0; attempt < 100; attempt++)); do
    [[ -b /dev/vdb ]] && break
    sleep 0.1
done
mount -t ext4 -o ro /dev/vdb /newroot
mount --move /dev /newroot/dev
mount --move /proc /newroot/proc
mount --move /sys /newroot/sys
exec /bootstrap-policy
