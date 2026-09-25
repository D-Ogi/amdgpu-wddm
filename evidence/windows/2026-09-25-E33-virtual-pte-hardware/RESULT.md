# M489 - Native PTE integration and GPU relocation controls

KMD151 integrates M488's virtual-address PTE builder into the gated
CopyPageTableEntries path. BuildPagingBuffer retains whole-range progress;
SubmitCommandVirtual binds the context's current root before queue ownership.
The companion DMA capacity is 64 KiB. Tracked pinned tables retain logical
shadow updates; untracked privileged roots need no construction-time CPU walk.

## Validation

- Actual extracted native DDI construction: 25 checks, zero failures.
- Submission queue and root binding: 550 checks, zero failures.
- Mixed private records: selected-range root rebinding, atomic rejection,
  replay rebinding and unchanged direct commands pass; kernel compile passes.
- Full WDK26100 KMD151 build succeeds.
- Three owned GPU controls update endpoint PTEs before executing the already
  built virtual IB: system-to-VRAM, forward alias overlap and backward alias
  overlap. Each compares all 4096 destination bytes and checks old backing.
  Real completion fences 7, 8 and 9 and all comparisons pass on unit A.
- KMD150's earlier control was skipped because its registry gate was read
  under a fast mutex at APC_LEVEL. KMD151 reads it before acquiring the mutex.
  The skipped run is retained and is not counted as a passing GPU test.
- After enabling the native DDI gate through PnP, release sparse CTS reports
  41 Pass and 1 NotSupported. The 128x128 image-rebind case is too small for
  partial binding. No new selected fault events or dumps; same Windows boot.

Crucial limit: post-CTS telemetry reports native PTE copies gate1 ranges0.
Thus CTS verifies regression behavior but did not exercise the new OS native
copy route. The three owned GPU controls do exercise the virtual builder and
GPU-ordered relocation, but do not establish Windows companion integration.
No claim of complete sparse support, Linux parity or certification follows.

## Identity and reproduction

Frozen KMD151 source and host inputs are in sources.zip; the delta from the
retained KMD149 source is kmd151-from149.patch. Manifest hashes identify every
source. Build driver/kmd/build.ps1 with the workspace WDK26100 toolchain.
The native-route host generator is in experiments/E33-m12-applications;
run driver/shim/test/run_paging.ps1 -VirtualPteRoute in the matching worktree.
Host-current paths in the archive preserve the precise test harness versions.

KMD SHA256: 0D0E61F3314F333DB13B66FA5DB63FD68C9A19DF36F3CCAC8C8F9EBEF2516F64.
Mesa candidate: 0F9FEAE8492B71343B7DBDE6A12DE9E1D95464C3AB20D0ABE45E35E383F7BF2D.
CTS release vulkan-cts-1.4.6.2, f6a29701220f34dd1407513bfe80d74ca7b392ce.
The original system ICD9C40083C is restored after CTS. The gallery remained
visible; these are correctness controls, not performance measurements.

controls.zip retains build logs, deployment/control logs, CTS QPA, module
witnesses, runner scripts and health readback. Device instance identifiers,
LUIDs and UUIDs are redacted where present; original hashes are retained.
No firmware blobs, memory dumps or desktop screenshots are published.

## Next acceptance step

Exercise an actual Windows COPY_PAGE_TABLE_ENTRIES operation and establish
native range-counter growth with a delayed companion wait and content oracle.
The local WDK26100 D3DKMTUpdateGpuVirtualAddress contract permits explicit COPY
operations: one destination reservation and one source reservation per batch.
This is a proposed control, not yet run. Then complete the required sparse
formats and the same-Mesa Linux comparison on unit A.
