#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Apply Fedora file contexts to an unmounted ext4 image with debugfs."""

import argparse
import ctypes as c
import os
from pathlib import Path
import re
import subprocess
import tempfile


class SelinuxOption(c.Structure):
    _fields_ = [("type", c.c_int), ("value", c.c_char_p)]


class FileContexts:
    def __init__(self, context_path):
        self.lib = c.CDLL("libselinux.so.1", use_errno=True)
        self.lib.selabel_open.argtypes = [c.c_uint, c.POINTER(SelinuxOption), c.c_uint]
        self.lib.selabel_open.restype = c.c_void_p
        self.lib.selabel_lookup_raw.argtypes = [
            c.c_void_p, c.POINTER(c.c_char_p), c.c_char_p, c.c_int
        ]
        self.lib.freecon.argtypes = [c.c_char_p]
        self.lib.selabel_close.argtypes = [c.c_void_p]
        # SELABEL_CTX_FILE=0, SELABEL_OPT_PATH=3 (libselinux label.h).
        option = SelinuxOption(3, os.fsencode(context_path))
        self.handle = self.lib.selabel_open(0, c.byref(option), 1)
        if not self.handle:
            raise OSError(c.get_errno(), f"cannot open {context_path}")

    def lookup(self, path, mode):
        value = c.c_char_p()
        if self.lib.selabel_lookup_raw(self.handle, c.byref(value), os.fsencode(path), mode):
            raise OSError(c.get_errno(), f"cannot label {path}")
        context = value.value
        self.lib.freecon(value)
        return context

    def close(self):
        self.lib.selabel_close(self.handle)


OVERRIDES = {
    "/init": "init_exec_t",
    "/usr/bin/setpriv": "init_exec_t",
    "/usr/lib/venus/rv-renderer": "venus_microvm_renderer_exec_t",
    "/usr/libexec/virgl_render_server": "venus_microvm_renderer_exec_t",
    "/usr/libexec/venus-pm-bridge": "venus_microvm_bridge_exec_t",
    "/usr/libexec/venus-gpu-power": "venus_microvm_power_exec_t",
    "/usr/lib/venus/modules": "modules_object_t",
}


def image_paths(root):
    yield root
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(dirs + files):
            yield Path(base) / name


def write_labels(image, labels, verify_paths):
    with tempfile.TemporaryDirectory(prefix="venus-labels-", dir="/tmp") as work:
        work = Path(work)
        commands = []
        contexts = {}
        for path, context in labels.items():
            if context not in contexts:
                context_file = work / f"context-{len(contexts)}"
                context_file.write_bytes(context + b"\0")
                contexts[context] = context_file
            if not re.fullmatch(r"/[A-Za-z0-9_./+@:-]*", path):
                raise ValueError(f"Unsupported ext4 path {path!r}")
            commands.append(f"ea_set -f {contexts[context]} {path} security.selinux")
        for index, path in enumerate(verify_paths):
            commands.append(f"ea_get -f {work / f'check-{index}'} {path} security.selinux")
        script = work / "debugfs.commands"
        script.write_text("\n".join(commands) + "\n")
        result = subprocess.run(
            ["debugfs", "-w", "-f", script, image], capture_output=True, text=True, check=True
        )
        # debugfs command errors can appear only on stderr with exit status 0.
        diagnostics = [line for line in result.stderr.splitlines() if not line.startswith("debugfs ")]
        if diagnostics:
            raise RuntimeError("\n".join(diagnostics))
        for index, path in enumerate(verify_paths):
            if (work / f"check-{index}").read_bytes() != labels[path] + b"\0":
                raise RuntimeError(f"SELinux label verification failed for {path}")


def system_context(kind):
    return f"system_u:object_r:{kind}:s0".encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--contexts", type=Path)
    args = parser.parse_args()
    if not args.root:
        write_labels(args.image, {"/": system_context("venus_microvm_vram_t")}, ["/"])
        return

    labels = {}
    contexts = FileContexts(args.contexts)
    try:
        for file in image_paths(args.root):
            path = "/" if file == args.root else "/" + file.relative_to(args.root).as_posix()
            kind = OVERRIDES.get(path)
            if path.startswith("/usr/lib/venus/modules/") and path.endswith(".ko"):
                kind = "modules_object_t"
            labels[path] = system_context(kind) if kind else contexts.lookup(path, file.lstat().st_mode)
    finally:
        contexts.close()
    # The ext4 population step creates lost+found in addition to the staging tree.
    labels["/lost+found"] = system_context("lost_found_t")
    write_labels(args.image, labels, ["/", *OVERRIDES])


if __name__ == "__main__":
    main()
