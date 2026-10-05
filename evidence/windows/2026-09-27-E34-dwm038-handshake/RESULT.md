# M682: matched handshake under confirmed GPU DWM

Unit A, runner78de33e, exact KMD164/9B9B99D3, UMD5C74BF98, hosted ICD3508416F.
RouterF0C7CEAD is bound to038; composition control7E04F678 rebuilt from the
unchanged source. Probe0DC75F8C/sourcec0edf93 is identical to M681. See manifest
and the corresponding source directory for full hashes, arguments and rollback.

## Handshake result

Confirmed GPU DWM12852 loaded the expected UMD/ICD. Health before the pair:
flags15, ready69396ms, generation163991222646, epoch5, completed1147. Both
641x479 cases ran in interactive console session1, with the post-transition
adapter LUID, --handshake-only --no-open, 500ms pump and1s hold. No device,
source, paging queue, context, resource open or Present was requested.

No-paint PID1760 at15:33:33.040Z and GDI-paint PID5316 at15:33:36.262Z both
return0x00263008 (DWM_S_GDI_REDIRECTION_SURFACE_BLT_VIA_GDI), format0, null
handle and update id0. Both ordinals resolve. Duration0ms is tick-resolution
limited, not proof of zero execution time. Both children and controller exit0.

This matches CPU control M681. The GPU desktop did not offer these windows a
surface for redirected-blt. The prerequisite for token/destination variants
A-D is absent; those variants were not tested or refused by this run. This does
not exclude other GPU presentation routes, different windows or other DWM gates.

## ETW and rendering controls

Loss-free trace15:32:08.4593065Z..15:35:34.0291732Z,137363456 bytes. The selected
4090 rows contain3289 Present184,11 Blit166,389 history171,390 history172,
1 detailed-history215 and10 CddStandardAllocation287 events. Neither probe PID
has a selected event, including Present. Other processes provide the presentation
positive control. Model3 is absent. History counts are per event:171/model7=389,
172/model0=390,215/model2=1, not780 independent tokens. The unfiltered decoder's
"of the probe" heading does not establish ownership; all11 Blits belong topid928.

All ten287 events occur15:32:11.2345393..2601786Z at transition, pid928, type6
TEXTURE_CPUVISIBLE, Flags1, GdiSurfaceFlags0. Sizes1920x1200,320x240,1920x48,
four1x1,920x1009,160x120,200x140. No type1 or641x479/657x518 event, none during
the pair. This is consistent with system-memory redirection but does not directly
measure backing memory or establish the exact reason for the DWM-side response.

The independent DMA analysis matches3248 start/stop pairs to DWM12852 contexts,
with zero pending/unmatched/duplicate starts, matching submission/completion IDs,
no preempted completion, and zero lost buffers/events. All56 UMD fence samples
are caught up; final sample submitted=completed2893, Present2880. KMD node0
147/147 ->2885/2885, zero timeouts/refused. GPU Present submits8 unchanged,
rejected/failed0; CPU Blt/skips/translations0. These counters cover different
boundaries and must not be treated as one-to-one IDs.

Stationary CPU baseline26,584 exact pixels; final primary and independent GDI
captures each37,800 exact pixels (8,000 static plus the whole29,800-pixel cyan
client). Independent host decoding agrees. This is selected-region/frozen-state
validation, not an owner observation or exhaustive live-frame/full-desktop proof.

## Audit limitation and closure

149 map snapshots reconcile totals with classified plus unclassified requests.
Final sample9024 overflows12 entries:12 image-map requests/2,592,632 bytes lack
bucket identity. Persistent calls8, unclassified persistent0. The separate
coverage classifier correctly rejects this trace as incomplete. No no-CPU-copy
acceptance follows. Known full-size WRITE bucket remains1 call/9,216,000 bytes;
full-size READ bucket reaches5/46,080,000 bytes, matching capture count only,
without per-call attribution. Persistent-pointer CPU stores are not measured.

Measured animation167.03659s (160s loop target plus iteration overhead). GPU
process started15:32:13.1228228Z; replacement CPU7204 started15:35:27.6223982Z,
about194.5s later. Thus the overall transition exceeded the three-minute target
by about14.5s despite the shorter animation; future timing must budget startup,
final captures and restoration, not just the loop. Restored receipt15:35:33.859Z.
Collector4000 terminal158 samples with no reader timeout. All four tasks removed,
original probes absent. Fresh15:37:22.930Z closure: exact baseline hashes,
health15,guard0,registry/latched gates0,1000MHz/VID116,66.625C,same boot. Lab free.

Raw ETL, images, archive and full DWM log stay in scratch/g0-hosted/dwm038;
private-artifact-hashes.json identifies them. Selected audit records are exact
lines, not edited measurements. Fifty candidate text files were checked for
identifiers before copying; the only address-pattern hit was the OS-version
field10.00.00.00. No private image or ETL is committed. No permanent promotion.

Next G0 work: resource/lifetime and uploader audit coverage with an explicit
measurement boundary and deliberate-copy positive control. The redirected WSI
investigation remains separate; repeating A-D without a target surface would
not test their intended prerequisite.
