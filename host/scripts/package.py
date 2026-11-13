# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Assemble, sign and package Venus.app and Venus.framework."""

import argparse
import os
import plistlib
import shutil
import tempfile
from pathlib import Path

from stage_qemu import QEMU_HELPER, command, command_output, stage_qemu


GUEST_ARTIFACTS = ("Image", "guest.initrd", "vram.qcow2", "venus-root.qcow2")


def input_file(path):
    if not path.is_file() or not path.stat().st_size:
        raise ValueError(f"Missing or empty input: {path}")
    return path


def codesign(path, identity, entitlements=None):
    args = ["/usr/bin/codesign", "--force", "--sign", identity]
    args += ["--timestamp=none"] if identity == "-" else ["--options", "runtime", "--timestamp"]
    if entitlements:
        args += ["--entitlements", entitlements]
    command(*args, path)


def verify_qemu(contents):
    helpers = contents / "Helpers"
    qemu = helpers / QEMU_HELPER
    version = command_output(qemu, "--version").splitlines()[0]
    if not version.startswith("QEMU emulator version 11.1.3"):
        raise ValueError(f"Unexpected bundled QEMU version: {version}")
    devices = command_output(qemu, "-device", "help")
    for device in ("apple-dma-pci", "ivshmem-doorbell", "vfio-apple-pci"):
        if f'name "{device}"' not in devices:
            raise ValueError(f"Bundled QEMU does not support {device}")
    vfio_properties = command_output(qemu, "-device", "vfio-apple-pci,help")
    if "venus-pm-chardev=" not in vfio_properties or "venus-pm-socket=" not in vfio_properties:
        raise ValueError("Bundled QEMU is missing the Venus VFIO power-management interface")
    command(helpers / "qemu-img", "--version")
    command(helpers / "ivshmem-server", "-h")


def create_dtb(contents, guest_resources, input_dtb):
    dtb = guest_resources / "base.dtb"
    if input_dtb:
        shutil.copy2(input_file(input_dtb), dtb)
    else:
        command(
            contents / "Helpers" / QEMU_HELPER,
            "-L", contents / "Resources/QEMU",
            "-nodefaults", "-machine", f"virt,accel=hvf,dumpdtb={dtb}",
            "-cpu", "host", "-smp", "2", "-m", "3072",
            "-object", "memory-backend-ram,id=ram0,size=1536M",
            "-object", "memory-backend-ram,id=pool,size=1536M",
            "-numa", "node,nodeid=0,cpus=0-1,memdev=ram0",
            "-numa", "node,nodeid=1,memdev=pool",
            "-display", "none", "-monitor", "none", "-serial", "none",
            "-kernel", guest_resources / "Image",
        )
    with input_file(dtb).open("rb") as stream:
        if stream.read(4) != b"\xd0\x0d\xfe\xed":
            raise ValueError(f"Invalid QEMU device tree: {dtb}")


def build(args, root, guest_inputs):
    contents = root / "Applications/Venus.app/Contents"
    helpers = contents / "Helpers"
    macos = contents / "MacOS"
    extensions = contents / "Library/SystemExtensions"
    for directory in (helpers, macos, extensions):
        directory.mkdir(parents=True)
    shutil.copy2(args.app, macos / "Venus")
    shutil.copy2(args.app_info, contents / "Info.plist")
    shutil.copy2(args.launcher, helpers / "venus-vgpud")
    driver = extensions / args.driver.name
    shutil.copytree(args.driver, driver, symlinks=True)
    if args.app_profile:
        shutil.copy2(input_file(args.app_profile), contents / "embedded.provisionprofile")
    if args.driver_profile:
        shutil.copy2(input_file(args.driver_profile), driver / "embedded.provisionprofile")

    stage_qemu(contents, prefix=args.qemu_prefix, source=args.qemu_source, build=args.qemu_build)
    license_dir = contents / "Resources/OpenSourceLicenses"
    project = Path(__file__).resolve().parents[2]
    shutil.copy2(project / "NOTICE", license_dir)
    shutil.copy2(project / "LICENSE.md", license_dir)
    shutil.copytree(project / "LICENSES", license_dir / "LICENSES")
    if args.mesa_licenses:
        shutil.copytree(args.mesa_licenses, license_dir / "Mesa")

    for image in (contents / "Frameworks").iterdir():
        codesign(image, args.identity)
    for helper in helpers.iterdir():
        entitlements = Path(__file__).resolve().parents[1] / "qemu/Entitlements.plist"
        codesign(helper, args.identity, entitlements if helper.name == QEMU_HELPER else None)
    verify_qemu(contents)

    framework = root / "Library/Frameworks/Venus.framework"
    shutil.copytree(args.framework, framework, symlinks=True)
    guest_resources = framework / "Versions/A/Resources/MicroVM"
    guest_resources.mkdir()
    for source in guest_inputs:
        shutil.copy2(source, guest_resources / source.name)
    create_dtb(contents, guest_resources, args.base_dtb)

    codesign(framework, args.identity)
    codesign(driver, args.identity, args.driver_entitlements)
    app = contents.parent
    codesign(app, args.identity, args.app_entitlements)
    command("/usr/bin/codesign", "--verify", "--strict", "--deep", app)
    command("/usr/bin/codesign", "--verify", "--strict", framework)


def package(args):
    if not args.guest:
        raise ValueError("Set VENUS_GUEST_ARTIFACTS_DIR to the Linux guest build's guest/artifacts directory")
    guest_inputs = [input_file(args.guest / filename) for filename in GUEST_ARTIFACTS]
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="venus-package-", dir=output.parent) as temp:
        work = Path(temp)
        root = work / "root"
        build(args, root, guest_inputs)
        components = work / "components.plist"
        with components.open("wb") as stream:
            plistlib.dump([
                {
                    "BundleHasStrictIdentifier": True,
                    "BundleIsRelocatable": False,
                    "BundleIsVersionChecked": False,
                    "BundleOverwriteAction": "upgrade",
                    "RootRelativeBundlePath": bundle,
                }
                for bundle in ("Applications/Venus.app", "Library/Frameworks/Venus.framework")
            ], stream)
        staged = work / "Venus.pkg"
        pkgbuild = [
            "/usr/bin/pkgbuild", "--root", root, "--component-plist", components,
            "--identifier", args.identifier, "--version", args.version, "--install-location", "/",
        ]
        if args.installer_identity:
            pkgbuild += ["--sign", args.installer_identity]
        command(*pkgbuild, staged)
        os.replace(staged, output)
    print(f"Created {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("output", "app", "app-info", "app-entitlements", "driver",
                   "driver-entitlements", "launcher", "framework"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    for option in ("guest", "base-dtb", "app-profile", "driver-profile", "qemu-prefix",
                   "qemu-source", "qemu-build", "mesa-licenses"):
        parser.add_argument(f"--{option}", type=lambda value: Path(value) if value else None)
    for option in ("identity", "installer-identity", "identifier", "version"):
        parser.add_argument(f"--{option}", required=True)
    args = parser.parse_args()
    if not args.qemu_prefix and not (args.qemu_source and args.qemu_build):
        parser.error("provide either a runtime prefix or the QEMU source and build directories")
    package(args)


if __name__ == "__main__":
    main()
