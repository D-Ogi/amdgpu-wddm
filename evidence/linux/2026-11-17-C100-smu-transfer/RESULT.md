# C099 buffer lifetime and C100 SMU transfers

Unit A, stock P3 firmware, Linux 6.18.52-0-lts, 2026-11-17.
The network-only boot had no amdgpu module or IVRS table.
The kernel build ID was `d073d7b7da060ba3dc9d883f76438aa242cd28b1`.
The trials used the same diagnostic module, SHA-256
`c105d26517bb3a29f12efe8c2d154d7cb5da9d4b3fe53acf27883848a4f55bbf`.

PROVENANCE: the diagnostic provider is original MIT-licensed code that uses the Linux DMA API.

## Scope and source records

These are archived measurements and source excerpts, not a deployment kit.
The archive omits firmware blobs and the compiled kernel module.
`SHA256.json` records each copied file and the omitted module hash.
All copied files except two have the same bytes as the retained originals, with no
redactions.
Two excerpts changed after the trial and were not run again:
`dma-controller-C100.py` and `dpm-preflight.py`.
Each one names the directory of a support kit on the unit.
That directory name had a prefix that does not belong in a public repository.
The archived excerpts have `bc250` in place of the prefix.
The digests of the two excerpts before this change are
`9524aa9d5d8ac6adaa0c9fd24431faa6b2affc9ab5de8fc5c6ef2c0c0c730765` and
`48c3dcd0bc53788ade998065cc8daa051f1945903558a14f7d0576dd4642408e`.
`PREFLIGHT_SHA` in the scripts is the digest of `dpm-preflight.py` as it ran.
External support kits remain dependencies of the archived source.
Do not run these sources as a new hardware trial.

The C100 runtime manifest was
`8747930f1aa36e4ed2e5cd189c74d146a66a11a95aed3726abf7ae4d72a9b927`.
The `source_sha256` fields in the receipts identify the generated remote commands.
The manifest and module hashes identify the trial code more precisely than the repository commit that archives it.

## C099: buffer lifetime

The trial ran at 15:21:24 UTC.
A fresh load allocated two coherent buffers, filled them and returned their snapshots.
Before address exposure, ordinary module unload succeeded.
A second fresh load tested address exposure with a failed descriptor copyout.
The client then received SIGKILL.
After a new client opened the provider, both complete canary buffers were intact.
The module reference count remained 1 and ordinary unload was refused.

All five child commands exited 0.
The client phases passed 48 assertions.
Temperature ranged from 73.375 C to 73.500 C.
The controller took 0.533 s.
No SMU request or DMA transfer ran in this trial.
A separate process test passed 17 checks on the same Linux kernel.

See `dma-lifecycle-execute-C099-01.json`, its validation file and `dma-process-test-C099-02.json`.
The kernel reported the expected taint from an unsigned external module.
No forced load or forced unload was used.

## C100: two known words

The trial ran at 15:37:53 UTC after a cold reset.
Queue 0 returned the expected version and three increment controls.
Queue 2 setup00 returned the expected descriptor.
The worker filled both kernel buffers before either address was exposed.
The restore04 request returned a fresh `(1,3)` result.

| Source offset in firmware SRAM | Destination | Expected and observed word | Full 4096-byte snapshot SHA-256 |
| --- | --- | --- | --- |
| `0` | Slot 0 | `0x00580600` | `947c92f709f0d85caede0fc80f4a651a30a127b46fd9cf31d320de2b29d933ab` |
| `0xEBC` | Slot 1 | `0xAD004136` | `68edfe9442d99052148f74aae8a72810151dcbe0f87d9bae9a6dd92d1d13f9aa` |

Each LOAD request returned `(1,2)`, which remains a recorded refusal.
Each following sub14 transfer returned a fresh `(1,3)`.
The destinations were distinct, page-aligned addresses in the checked 32-bit DMA route.
The worker compared the complete page, including the unchanged 4092-byte canary tail.
The two full-page hashes match the expected words followed by their respective canaries.
The receipt retains the hashes and checked fields, not raw page dumps.

The raw log contains 2885 events.
Review paired 203 complete SMN operations and 669 complete BAR operations.
Each INDEX2 selection read back the requested selector before the DATA2 operation.
Each command had a cleared response and checked arguments before submission.
This excludes acceptance of a response left by the previous command in these transactions.

The worker and controller exited 0.
Temperature ranged from 73.500 C to 73.875 C.
The controller took 0.913488 s.
Both buffers remained owned by the kernel until the cold reset.

See `dma-transfer-execute-C100-01.json` and `dma-transfer-VALIDATION-C100-01.json`.
The raw receipt SHA-256 is
`043dc0b9cca76d3cf3bd2c813da001eb1815906d4dbf40ba72cadf2b738ba77b`.

## Recovery and limits

After C100, clean shutdown and an AC cycle removed the retained allocations.
The original next-boot configuration was restored at 15:42:53 UTC.
The postboot check at 15:43:18 UTC found no provider, provider device, amdgpu module or old RAM trial.
Temperature was 71.875 C.
The restore and postboot receipts are included.

C099 measures the tested buffer ownership behavior.
C100 measures the two specified data observations.
Neither result establishes arbitrary SRAM access or a general hardware DMA-drain guarantee.
The LOAD refusals do not establish an accepted normal LOAD state.
No queue 5 request, unlock command or VCN activation ran.
These results do not identify a VCN power command.
