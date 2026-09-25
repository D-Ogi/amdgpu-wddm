# M483: native sparse controls isolate initial-hole failure

Unit A, 2026-09-25. KMD 0.7.147.1 / SHA256 5FCB554A.
M12.1 is not accepted and Vulkan sparse capabilities remain disabled.

## Ordinary candidate regression
Mesa05e6c962 candidate255534CA completes the same 80 release CTS basic cases
as M480: 75 Pass, 5 NotSupported, 0 Fail. Every case has matching QPA, exit0
and loader/candidate module witnesses. Final readback verifies all five input
hashes, the same boot, and no selected new fault events or dumps. The temporary
candidate registration was restored to system ICD9C40083C. This is a selected
control, not full conformance or Linux parity.

## Native Windows controls
The native probes use real D3DKMT calls and the candidate's extracted mapping
helpers, independently of Vulkan feature advertisement.

- Initial ReserveGpuVirtualAddress + MapGpuVirtualAddress Zero succeeds and its
  paging fence retires. This first test does not read the hole.
- Application wait holds the mapping boundary at0 until CPU release; mapping
  completes at2 and unmap at4. This proves API ordering, not hole contents.
- CP COPY_DATA reads physical A=13579BDF2468ACE0 and B=FEDCBA9876543210.
  Mapping the virtual range to A returns A; rebinding to the second allocation
  page returns B. All four exact content comparisons pass; unmap fence6 retires.
- A separate initial-hole test passes both physical controls, then hangs on its
  first hole read. No content comparison or successful result exists for that
  read. Windows records VIDEO_TDR_FAILURE0x116 and automatically reboots.
  No smart-plug power transition was used.

## Offline diagnosis and test correction
The retained full kernel dump is private and is not included in this repository.
Its size/hash identity is in audit.json. Offline CDB with the matching147 PDB
identifies Bc250WddmResetFromTimeout and failure C0000001. Driver log extraction
shows graphics sequence23666 timing out after500ms; the paging node has20827
submissions and20827 completions. ResetFromTimeout reports that GPU memory
access is not proven stopped. These observations do not identify the exact
faulting PTE. The kernel bitmap dump does not contain the required VRAM pages.

The old test helper incorrectly accepted UINT64_MAX as a completed monitored
fence (requested3, seen18446744073709551615 after2281ms). It now rejects that
sentinel. Earlier normal values and exact content comparisons remain valid.
The corrected reusable probe builds with warnings as errors; it has not yet
been rerun on the lab. Default mode checks bound data; --holes is explicit.

## Next gate and reproduction
Implement native Zero/PRT semantics against the GFX10 AMD definitions and the
Microsoft ZeroInPteSupported contract for every table level. Then test initial
and unmapped holes, GPU shader loads/stores, sparse images, and scalar aliases.
Do not infer unsupported hardware from this Windows-only failure.
Linux parity and privilege-correct queued PTE resolution remain open.

Use prepare_native_sparse_control.py with the pinned Mesa source to generate
the helper include and build command. controls.zip preserves the four original
probe sources, pre-correction kmtprobe source, compiler output, native outputs,
CTS outputs and driver-only dump log. UUID/LUID/report identifiers are redacted
where present; manifest.json records original and published hashes.
PROVENANCE: Mesa mapping helpers and AMD packet definitions are MIT.
