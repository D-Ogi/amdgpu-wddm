# M562: controlled GPU DWM with loss-free DMA trace and classified CPU maps

Unit A, source bc250-win ab2f456, candidate UMD B69AA635 and hosted ICD
3508416F, KMD 0.7.152.1. Exact package hashes are in manifest.json. This repeats
M560 with M561 map buckets and larger ETW buffers: 1024 KiB, minimum 64,
maximum 256. The GDI-only animated red window and half-alpha blue window use
no D3D/Vulkan client rendering. The system UMD router selects only one DWM PID;
other processes keep the immutable CPU UMD. The watchdog bounds the trial.

## Image and execution

All six module samples retain hosted DWM PID 12608 and the intended binaries.
The CPU reference, independent KMD scanout dump and GPU screen capture agree
in all 8000 tested pixels: 1400 red pixels (255,0,0), and 6600 overlap pixels
(127,0,128). This tests composition/blending in controlled regions, not every
pixel of the desktop. Full images remain private.

The decoded ETL header reports zero events lost and zero buffers lost. Decoding
without xperf's lost-event override succeeds. Context-lifetime-aware matching
finds 589 DWM-owned DMA Start/Stop pairs, including 587 on the main rendering
context. There are no unmatched stops, pending starts or duplicate starts.
The verifier checks chronological input and publishes each filtered pair;
full ETL/CSV remain private. Reused context addresses must not be treated as
permanent ownership: that naive approach overcounts this trace by two pairs.

Sampled UMD monitored fences reach submitted=completed=543 at Present 540.
The user-mode witness and independent DxgKrnl DMA completion witness are
complementary; their counters are not claimed to be one-to-one identifiers.

## CPU mapping and copying

The calibrated M559/M561 instrumentation records resource dimensions, requested
box, direction, bindings, runtime/user-pointer status and persistent flags.
No bucket overflow occurs. These are map requests and logical requested bytes,
not a count of actual CPU stores. The published audit lines retain every
sample, including startup and the final capture phase.

One 1920x1200 sampler-view WRITE map appears by sample 192. Its cumulative
count remains one through the final sample 1728. One 1920x1200 READ map first
appears at sample 1728, in the final diagnostic-capture phase. Its attribution
to screen capture is consistent with the harness but is not a recorded call
stack. Neither is hidden or counted as zero. No recurring full-frame image
map occurs in the retained snapshots. There are no persistent image maps or
user-pointer image maps.

For an explicit interior comparison, samples 256 through 1664 bracket progress
from after Present 60 to after Present 540. Their image-map deltas are 219
uploads of 320x240, 219 of 160x120, and 32 of 460x1123. All are nonpersistent
WRITE/discard requests on sampler-view resources, not runtime primary maps.
The third surface's originating window is not identified. Buffer deltas are
retained separately. This interval is not a claim about unobserved teardown.

The final persistent-map buckets are seven 24000-byte descriptor-buffer maps
and one 1 MiB uploader map. Source audit addresses the subsequent-write gap:

- zink_descriptors.c:1663 allocates ZINK_BIND_DESCRIPTOR storage and maps it
  persistently. Its db_map writes use GetDescriptorEXT and descriptor-sized
  memcpy operations (lines 1152-1241 and 1426-1442). ZINK_BIND_DESCRIPTOR is
  the private bit 27 in zink_resource.h; the generic video-DPB name for that
  bit is not its meaning here.
- u_upload_mgr.c creates the 1 MiB vertex/index/constant uploader with
  WRITE|UNSYNCHRONIZED|PERSISTENT|COHERENT. Zink uses it for constants at
  zink_context.c:1802 and staging buffer updates at zink_resource.c:2757.
  The latter's original buffer-map request is counted before suballocation;
  persistence does not bypass that request record. Observed buffer deltas
  have vertex/index/constant bindings. This is a source-path audit, not a
  hardware watchpoint measuring each store.
- DxgiFns.cpp:155-174 imports the runtime allocation/VA and returns before
  the CPU Lock2 branch. radv_wddm2_bo.c:1047 rejects mapping borrowed runtime
  BOs before consulting any cached pointer. The presented imported primary
  therefore has no CPU pointer through these paths.

KMD summaries retain 604 blits and 604 translated sources across 25.637 seconds.
The summary's seed field is truncated. Source inspection shows seed copying
at wddm.c:4699 and row copying at 4762/4769 all reach the Blits increment at
4777 without an intervening return. Stable Blits therefore also excludes
completed seed-copy calls in this interval. Full WDDM uses WddmBuildTable;
the separate display-only PresentDisplayOnly function is not in that table.
Audited file hashes are retained. No claim of zero CPU work or zero uploads
is made: GDI inputs and command/descriptor data still require CPU work.

Together, these measurements and source paths support GPU composition without
a recurring CPU full-frame presentation copy in this bounded workload. They
do not prove arbitrary workloads, every possible map caller, or long-term
stability. The 30-minute M13.4 workload, full cross-process sharing and lifecycle
requirements remain open; this result does not close the complete G0/M13 gate.

## Recovery and evidence handling

The main runner exits successfully and restores baseline UMD 8279AC7F and ICD
9C40083C. The watchdog independently verifies them. CPU DWM is PID 84 after
rollback, both scheduled tasks finish, and the control windows close. No KMD
change, OS reboot or permanent Zink promotion occurs.

Public evidence includes derived ROI/map/ETW records, exact harness sources,
module identities and private-artifact hashes. audit-lines.txt is an explicitly
filtered extraction, not the complete UMD log. Account/machine fields alone
are redacted in run-redacted.txt. Raw images, full ETL/CSV and KMD logs remain
private. Earlier exploratory derived JSON remains untouched; verify-evidence.py
reads the actual ETL loss fields rather than assuming loss from decoder success.
