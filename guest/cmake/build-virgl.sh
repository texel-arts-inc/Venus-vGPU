#!/bin/bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
set -euo pipefail
meson=$1 ninja=$2 compiler=$3 source=$4 build=$5
export CC="$compiler"
if [[ -f $build/build.ninja ]]; then
    "$meson" setup --reconfigure "$build" "$source"
else
    "$meson" setup "$build" "$source" --buildtype=release \
        -Dplatforms=egl -Dvenus=true -Dvulkan-dload=true -Dvulkan-preload=false \
        -Drender-server-mode=process -Drender-server-worker=process \
        -Dvtest=false -Dtests=false -Dunstable-apis=true
fi
"$ninja" -C "$build" src/libvirglrenderer.so.1.11.0 server/virgl_render_server
