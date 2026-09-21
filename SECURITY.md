# Security and authenticity

A GPU driver runs in the Windows kernel. The BC-250 community has already seen fake "drivers" that were malware (`docs/prior-art.md`). A license does not stop people who ignore licenses, so this project relies on verifiability.

## What counts as genuine

- The source in the author's repository, and releases cut from it by the author (D-Ogi).
- Every release lists SHA-256 checksums of its files, and the checksum file is signed with the author's key. The key fingerprint is published in the repository README once the first release exists.
- The project does not distribute Windows images, activators, "all-in-one" installers, modified BIOS files or AMD firmware. Anything bundling our name with those is not ours.

## What users should do

- Build from source, or verify the signature and checksums before installing anything.
- Never disable driver signature enforcement for a binary whose source you cannot see. During development this project uses Windows test signing with a locally generated certificate; a test-signed driver from a stranger is exactly what kernel malware looks like.
- Treat any "BC-250 Windows driver" executable from chats, file hosts or video descriptions as hostile.

## Use of the project name

The license grants copyright permissions only. It does not grant permission to present a modified or repackaged version as the original. Forks must use a different name and must keep the `Required Notice` lines from `NOTICE`.

## Reporting a vulnerability or an impersonation

Open a private security advisory on the repository hosting service, or contact the author through the address in the commit history. Please do not open a public issue for an exploitable bug in kernel code.

## Safety of the tools in this repository

- `tools/diagusb` writes to two GPU registers only (documented in its README) and has a strictly read-only mode.
- `tools/diagusb/write_usb.ps1` erases a disk. It refuses to run without an explicit disk number, a matching device name, and an acknowledgement switch. Read it before running it.
- Nothing in this repository flashes firmware or BIOS.
