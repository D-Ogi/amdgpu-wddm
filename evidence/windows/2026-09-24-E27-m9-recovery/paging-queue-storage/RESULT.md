# M424 - Per-DMA private queue storage format

Source and host preparation only. Builders still emit legacy records and
WddmSubmitPagingHardware still allocates/copies a nonpaged job per submission.
The installed M423 KMD0.7.132.1 is not replaced by this development build.

The new queued direct/native record kinds reserve CPU-only job slots in DMA
private storage. Walkers preserve direct/native command meaning while skipping
mutable queue bytes. Direct subranges have a separate 64-byte slot for every
DWORD start; native records allow whole ranges only. This does not by itself
prove Windows lifetime or cancellation behavior.

Validation:

- Actual-source format test passes 37 mixed slots, all 16384 DWORD starts in a
  64KiB record, command isolation and all 131073 private capacities from 0 to
  128KiB. Independent buffers and physical/virtual ranges are covered.
- A copied-source mutation returning the first slot for every subrange fails
  at the distinct-slot assertion, native exit 1. Production source is unchanged.
- Existing actual builder/packet suite: 875868 checks, 0 failures. Builders are
  unchanged, so this is regression coverage, not queued-builder integration.
- Kernel compile and full WDK build pass. Development SYS SHA256:
  9A9F210B746A26515277C575239CCF3ACEF92C23FA757B82D367F443AB8DA13B.
  It retains version132 and was not deployed. Installed M423 SYS:
  E7AF5A02A3DEDA2CAD25E7D6A789FDA3C2406666D537F583D8ACAB14BDA49652.

Local MS contracts and required integration are recorded in the included design.
Runtime builders, admission, completion-before-reuse and stop/cancellation need
integration and tests. Capture fallback and all other full M9 gates remain open.
No hardware experiment or reset was performed for this format preparation.
Logs are copied byte-for-byte; no redaction was necessary.
