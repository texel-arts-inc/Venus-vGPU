# Preparing a public release

1. Review [source provenance](PROVENANCE.md) and confirm the grant for
   historical files that lack a license notice. Keep all upstream notices.
2. Build from a clean checkout, with separate macOS and Fedora aarch64
   build directories. Keep the NVIDIA installer and any image containing
   it in local build storage.
3. Review the actual package payload, including the contents of the
   guest disk. Record the RPM and firmware versions, license terms, and
   source for the bundled files. Confirm any firmware redistribution
   permissions. Use Venus branding for the modified guest and consult
   the [Fedora trademark guidelines](https://docs.fedoraproject.org/en-US/legal/trademarks/).
4. Attach the exact corresponding source for the distributed GPL
   binaries, including the applied patches, configuration, and build
   and installation scripts. Keep the source download alongside the
   binary. [GPLv2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)
5. Sign with the appropriate DriverKit provisioning and Developer ID
   identities, then notarize and test the installer on a Mac with
   normal system validation. [Apple system extensions](https://developer.apple.com/documentation/systemextensions/)

The exact NVIDIA Linux aarch64 620.70.06 agreement has a conditional
redistribution grant in section 1.1(d). It covers eligible open-source
kernel environments and requires unchanged vendor binaries and delivery
of the agreement to each recipient. Venus uses a separate user-supplied
or opt-in vendor download. This also keeps the NVIDIA guest image out
of the source release. [NVIDIA aarch64 agreement](https://download.nvidia.com/XFree86/Linux-aarch64/620.70.06/license.txt)
