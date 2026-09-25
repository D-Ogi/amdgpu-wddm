# M490 - Explicit Windows mapping COPY

Unit A, KMD151/0D0E61F3, unchanged Windows boot. The native control runs
from 16:44:21Z to 16:44:24Z and returns exit0. Its SHA256 is
7667E298702B36BA4370E59F57762D1B40E328C9C630790793944F397A64C94D.

Two D3DDDI_UPDATEGPUVIRTUALADDRESS_COPY operations copy 64 KiB mappings
from different offsets of one resident allocation into a separate reservation.
The first waits behind an unsignaled application fence. After 20 ms the
companion completion remains zero; signaling permits completion. CP DMA_DATA
readback matches the expected 64-bit values in four checks: physical A,
physical B, alias A, then rebound B. Unmapping and teardown also succeed.
This is sampled content evidence, not a comparison of all 64 KiB bytes.

The post-run KMD summary still reports native PTE copies gate1 ranges0 and
no TDR. UPDATE_PAGE_TABLE grows from 11927 to 11960, while operation14 is
absent. Therefore an explicit UMD COPY did not cause the new KMD native PTE
copy path to execute in this run. It does establish the tested API ordering
and sampled alias content on the OS-selected path. The test does not relocate
the physical allocation while waiting and does not settle why Windows chose
this route. M489's owned GPU controls remain separate evidence.

Health retains flags15, generation56484062069 and epoch5. Both boot records
match 2026-09-25T16:50:35.5000000+02:00. No reset or ICD change was needed.

control.zip contains original probe/build/run inputs and readbacks, with
irrelevant device instance identifiers and LUIDs redacted. Original and
published hashes are in manifest.json. No private memory dump is included.
The expected native-route counter increase did not occur; the experiment's
route hypothesis is not confirmed even though its content checks pass.
