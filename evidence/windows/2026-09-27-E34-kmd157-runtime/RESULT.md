# M627: KMD157 runtime callback diagnostic and native control004

Unit A, Windows, 2026-09-27. Exact KMD source
885922a91f37d15316edfe01110c386b56a0e10b, SYS
9B4AEBED0A0D4001E03BC3D39CD6B2E8AC051F98A9BD0A1B150C3EFB9FC0E067.
Transition scripts: 4a68edb. Control binary source4633abe, SHA256
0821C9BDA9A2D72AEF7B8C3185D0ACB729F258A570BA26D3B3031E8DBA604963.
The157 runner differs from the153 runner only in version/hash health pins.

The bounded PnP installer and worker finish with exit0; DWM4596 and OS boot
are retained. The collector finishes161 samples without reader timeout.
At07:15:54Z, healthflags15 and guard0 show durable startup confirmation;
no manual confirmation command was issued. GPU Present remains disabled,
identity diagnostics enabled. CPU UMD8279AC7F and ICD93B1D1FD are retained.

Startup capture contains12 non-BC2A opens with32-byte private data, flags1
or0. Each has GetHandleData=NULL and non-NULL AcquireHandleData and release
token. Control004 also captures one192-byte BC2A open with the same result.
The live pointer field is truncated by the existing log record limit (FFF
or FF remains); do not compare its full address. Its nonzero prefix is
consistent with the live-list lookup succeeding, but the full pointer is
not preserved. This supports using the WDDM2 Acquire/Release pair; this
probe does not yet use that result for the opened allocation binding.
It does not establish a CDD-specific restriction on GetHandleData.

Control004 passes all five dirty-list copies,30 device-accessible residency
checks, zero byte mismatches, final GPU fence34. Largest case verifies
9,371,648 bytes with2,302,800 copied pixels and2,400 packets, including the
untouched stripe. Residency status1 does not identify the physical heap.
No TDR is reported. At closure07:18:49Z: healthflags15,1000MHz/VID116,
66.375C, same DWM and boot. Both transition and control tasks are removed.
This control uses BC2S submissions, not BGP1 Present. Full G0 remains open.

JSON and closure receipts are copied unchanged. Selected text files preserve
original relevant output lines, decoded to UTF-8; source filenames identify
startup excerpts. Unrelated output is omitted, no selected values redacted.
Complete raw logs and startup archive remain in the private workspace under
scratch/g0-hosted/kmd157-transition and gfx-blt-control004-collected.
No new owner visual verdict: the owner explicitly did not watch DWM023.
