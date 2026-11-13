# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Build the app's private QEMU runtime from source build products or a runtime prefix."""

import bz2
import json
import re
import shutil
import subprocess
from pathlib import Path


SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")
QEMU_HELPER = "qemu-system-aarch64-vfio"
INSTALL_DATA = Path("/Applications/Venus.app/Contents/Resources/QEMU")


def command(*args):
    subprocess.run(args, check=True)


def command_output(*args):
    return subprocess.check_output(args, text=True)


def load_commands(image):
    return command_output("/usr/bin/otool", "-l", image).splitlines()


def linked_images(image):
    for line in command_output("/usr/bin/otool", "-L", image).splitlines()[1:]:
        name = line.strip().partition(" (compatibility version")[0]
        if name and not name.startswith(SYSTEM_PREFIXES):
            yield name


def install_names(image, attribute):
    return {
        match[1]
        for line in load_commands(image)
        if (match := re.fullmatch(rf"\s+{attribute} (.+) \(offset \d+\)", line))
    }


def copy_runtime(prefix, contents):
    helpers = contents / "Helpers"
    frameworks = contents / "Frameworks"
    tools = {".qemu-system-aarch64-vfio": QEMU_HELPER, ".qemu-img": "qemu-img",
             "ivshmem-server": "ivshmem-server"}

    pending = []
    for source, name in tools.items():
        helper = helpers / name
        shutil.copy2(prefix / "bin" / source, helper)
        pending.append(helper)

    seen = set()
    checked = set()
    while pending:
        image = pending.pop().resolve()
        if image in checked:
            continue
        checked.add(image)
        for dependency in install_names(image, "name"):
            if dependency.startswith(SYSTEM_PREFIXES):
                continue
            if not dependency.startswith("@rpath/"):
                raise ValueError(f"{image} loads a dependency outside its bundle: {dependency}")
            name = dependency.removeprefix("@rpath/")
            framework = Path(name).parts[0]
            if not framework.endswith(".framework") or not (prefix / "Frameworks" / name).is_file():
                raise ValueError(f"{image} has an unresolved dependency: {dependency}")
            if framework not in seen:
                shutil.copytree(prefix / "Frameworks" / framework, frameworks / framework, symlinks=True)
                seen.add(framework)
            target = (frameworks / name).resolve(strict=True)
            if not target.is_relative_to(frameworks):
                raise ValueError(f"{image} has a dependency symlink outside its bundle: {dependency}")
            pending.append(target)

    resources = contents / "Resources"
    shutil.copytree(prefix / "share/qemu-11.1.3", resources / "QEMU")
    notices = prefix / "share/virtualization"
    licenses = resources / "OpenSourceLicenses"
    shutil.copytree(notices / "licenses", licenses)
    shutil.copy2(notices / "sources.json", licenses)
    shutil.copytree(notices / "qemu-11.1.3-build", licenses / "QEMU-build")


def bundle_dylibs(executables, frameworks):
    """Relocate the absolute install names emitted by the host pkg-config libraries."""
    sources = {}
    pending = [(original, image, True) for original, image in executables]
    images = []

    while pending:
        original, image, helper = pending.pop()
        dependencies = list(linked_images(original))
        if not helper:
            dependencies = dependencies[1:]  # LC_ID_DYLIB precedes imports in otool -L.
        edits = []
        for dependency in dependencies:
            source = Path(dependency)
            if not source.is_absolute():
                raise ValueError(f"{original} uses an unsupported install name: {dependency}")
            canonical = source.resolve(strict=True)
            filename = source.name
            dest = frameworks / filename
            if filename not in sources:
                sources[filename] = canonical
                shutil.copy2(canonical, dest)
                dest.chmod(dest.stat().st_mode | 0o200)
                pending.append((canonical, dest, False))
            elif sources[filename] != canonical:
                raise ValueError(f"Two different Mach-O libraries share the name {filename}")
            location = f"../Frameworks/{filename}" if helper else filename
            edits += ["-change", dependency, f"@loader_path/{location}"]
        for rpath in sorted(install_names(image, "path")):
            edits += ["-delete_rpath", rpath]
        if not helper:
            edits += ["-id", f"@loader_path/{image.name}"]
        if edits:
            command("/usr/bin/install_name_tool", *edits, image)
        images.append(image)

    for image in images:
        for dependency in linked_images(image):
            if not dependency.startswith("@loader_path/"):
                raise ValueError(f"Unexpected install name after relocation: {dependency}")
            target = (image.parent / dependency.removeprefix("@loader_path/")).resolve()
            if not target.is_file() or not target.is_relative_to(frameworks):
                raise ValueError(f"Unresolved private dependency of {image}: {dependency}")


def stage_source_runtime(source, build, contents):
    helpers = contents / "Helpers"
    resources = contents / "Resources"
    inputs = {
        "qemu-system-aarch64": QEMU_HELPER,
        "qemu-img": "qemu-img",
        "contrib/ivshmem-server/ivshmem-server": "ivshmem-server",
    }
    images = []
    for source_name, helper in inputs.items():
        original = build / source_name
        image = helpers / helper
        shutil.copy2(original, image)
        images.append((original, image))
    bundle_dylibs(images, contents / "Frameworks")

    qemu_data = resources / "QEMU"
    qemu_data.mkdir()
    pc_bios = source / "pc-bios"
    for filename in ("edk2-aarch64-code.fd", "edk2-arm-vars.fd"):
        with bz2.open(pc_bios / f"{filename}.bz2", "rb") as compressed:
            with (qemu_data / filename).open("wb") as unpacked:
                shutil.copyfileobj(compressed, unpacked)
    (qemu_data / "firmware").mkdir()
    shutil.copy2(pc_bios / "descriptors/60-edk2-aarch64.json", qemu_data / "firmware")
    shutil.copy2(pc_bios / "edk2-licenses.txt", qemu_data)
    # A passthrough device can request its option ROM via the virtio/PCI bus.
    for rom in pc_bios.glob("*.rom"):
        shutil.copy2(rom, qemu_data)

    project = Path(__file__).resolve().parents[1] / "qemu"
    licenses = resources / "OpenSourceLicenses"
    shutil.copytree(project / "licenses", licenses)
    qemu_notices = licenses / "qemu"
    qemu_notices.mkdir()
    for filename in ("COPYING", "COPYING.LIB", "LICENSE"):
        shutil.copy2(source / filename, qemu_notices)
    dtc_notices = licenses / "dtc"
    dtc_notices.mkdir()
    for filename in ("BSD-2-Clause", "GPL"):
        shutil.copy2(source / "subprojects/dtc" / filename, dtc_notices)
    shutil.copy2(project / "sources.json", licenses)
    shutil.copytree(project.parents[1] / "patches/qemu", qemu_notices / "patches")
    shutil.copytree(project / "pm", qemu_notices / "pm")


def stage_qemu(contents, *, prefix=None, source=None, build=None):
    (contents / "Frameworks").mkdir()
    (contents / "Resources").mkdir()
    if prefix:
        copy_runtime(prefix, contents)
    else:
        stage_source_runtime(source, build, contents)
    qemu_data = contents / "Resources/QEMU"
    for descriptor in (qemu_data / "firmware").glob("*.json"):
        firmware = json.loads(descriptor.read_text())
        for key in ("executable", "nvram-template"):
            entry = firmware["mapping"].get(key)
            if entry is None:
                continue
            filename = Path(entry["filename"]).name
            if not (qemu_data / filename).is_file():
                raise ValueError(f"{descriptor} references missing firmware: {filename}")
            entry["filename"] = str(INSTALL_DATA / filename)
        descriptor.write_text(json.dumps(firmware, indent=4) + "\n")
    for link in contents.rglob("*"):
        if link.is_symlink() and not link.resolve(strict=True).is_relative_to(contents):
            raise ValueError(f"Runtime symlink leaves the app bundle: {link}")
