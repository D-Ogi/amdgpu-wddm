# BD-025: runtime firmware metadata in private caps

2026-09-24, source and host validation only. No lab/deployment/commit/version changes.
Candidate 137's frozen source is unchanged; these edits belong to a later candidate.

Reachability confirmed: WddmQueryAdapterInfo's UMDRIVERPRIVATE branch copied the generated
historical umd_caps_blob verbatim. The blob comes from the measured Linux fixture, including
SMC 0x00580600. The fixture itself remains unchanged and correct as historical evidence.

Implementation:
- Shared driver/contract/bc250_umd_firmware.h contains the existing 64-byte wire structure,
  without pulling Linux DRM declarations into kernel translation units. No ABI layout change.
- psp.c ReadFirmwareMetadata obtains all seven version/feature pairs from the exact file
  buffers passed to LayOut and then staged. Each parser call rechecks the existing supported
  image bounds/version policy. No files are reopened.
- A complete successful PSP LOAD publishes the metadata alongside Loaded while GartLock is
  held. Partial load does not publish. Unload clears Loaded and metadata. Stop detaches the PSP
  pointer under GartLock, so later queries cannot read the retained/freed object.
- PspReadFirmware copies only a loaded, non-quarantined snapshot under GartLock; no hardware
  or file I/O. It releases GartLock before the separate SMU cache accessor is called.
- SmuOwnerStart asks GetSmuVersion once through Message and the existing transport while
  holding the owner's exclusive lock and marking the current caller as owner. Failed query
  leaves owner offline; no legacy writer fallback. Successful start caches the raw firmware
  version. SmuOwnerStop clears it. SmuReadFirmwareVersion only copies that cache while online.
- UMDRIVERPRIVATE keeps the measured hardware template and overlays the current firmware
  section at offset 1208. Invalid/not-ready caches return failure without copying the template
  or substituting zeros as valid versions. Existing input/size checks remain.

Important scope: this makes caps dependent on successful firmware load and native owner
startup. A diagnostic full-WDDM configuration without them no longer returns historical
firmware versions as if current. The current production startup performs these stages before
normal UMD use; future lab validation must verify that ordering at actual query time.
Other historical hardware-template fields are outside BD-025.

References:
- Local ref/ddi-display/d3dkmddi.md:9924, DXGKDDI_QUERYADAPTERINFO, WDK 10.0.26100 declaration:
  PASSIVE_LEVEL, private output supplied for UMDRIVERPRIVATE, failure is reportable.
- Linux v6.18 commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449, AMD MIT sources:
  amdgpu/amdgpu_kms.c:245/269/320 firmware query fields;
  amdgpu/amdgpu_rlc.c:289 RLC header version/feature;
  pm/swsmu/smu_cmn.c:948 GetSmuVersion and cached smc_fw_version;
  pm/swsmu/smu11/cyan_skillfish_ppt.c:71 maps GetSmuVersion;
  local imported driver/amdgpu-import/smu_v11_8_ppsmc.h:36 gives message definition.
  No new upstream implementation was copied.

Validation:
- Native actual-source owner host test: 30773 checks, zero failures (native-smu.log).
  Includes cached P3-style version, stop/restart to P5-style version, rejected version query
  leaving owner offline, subsequent successful retry, and no message on cached version reads.
  Existing four-thread clock/read synchronization and stop/join controls still pass.
- Metadata test extracts actual ReadFirmwareMetadata, PspReadFirmware, load completion
  publication and UMDRIVERPRIVATE branch. Links actual parser and reads the local eight
  firmware files without copying bytes into artifacts. 34 checks, zero failures
  (metadata-v4.log). All seven pairs match the historical known image set. Modified in-memory
  ME version/MEC2 feature and changed simulated SMU version reach output; other caps bytes
  remain unchanged. Partial/unloaded/quarantined/stopped states refuse; no hardware callbacks.
- Dropping runtime firmware overlay: 34 checks, 2 failures (stale-caps-v4.log).
- Replacing SMU cached reply with 0x00580600: 30773 checks, 2 failures (stale-smu.log).
- Original caps/template ABI test and kernel compile: pass (caps.log), including 64-byte
  structure and offsetof(firmware)==1208. Measured fixture/blob not regenerated.
- Existing actual firmware-preflight ownership test: 32 checks, zero failures (preflight.log).
- Full WDK KMD build passes (kmd-build.log), scratch/build/bd025/kmd. This developer binary
  includes other concurrently present work, is not a release artifact and must not be deployed.
- git diff --check passes with CRLF normalization warnings only.
- Initial test harness compile failure (shadowed mock smuVersion) and missing linker stubs
  are retained in metadata.log/metadata-v2.log; corrected harness passes above.

Changed files for this task:
  driver/contract/bc250_umd_private.h
  driver/contract/bc250_umd_firmware.h (new)
  driver/kmd/firmware_metadata.h (new)
  driver/kmd/psp.c
  driver/kmd/smu.c
  driver/kmd/smu.h
  driver/kmd/umd_caps.h
  driver/kmd/wddm.c (one include and UMDRIVERPRIVATE branch only)
  driver/kmd/test/umd_caps_test.c
  driver/kmd/test/firmware_metadata_test.c (new)
  driver/kmd/test/generate-firmware-metadata-test.py (new)
  driver/kmd/test/run_firmware_metadata.ps1 (new)
  driver/shim/test/smu_native_test.c
No bc250kmd.h changes. BD-026 parser gate and unrelated dirty work preserved.

Proposed BD-025 status: FIXED (source/host, not deployed).
Proposed append-only comment:
- 2026-09-24 review: Confirmed production UMDRIVERPRIVATE returned the historical firmware
  words from generated umd_caps_blob. It now overlays metadata cached from the exact PSP
  file snapshot only after successful full loading, plus GetSmuVersion queried once through
  the native owner at start. QueryAdapterInfo only copies caches; unloaded/offline/quarantined
  states refuse without historical fallback. Fixture and ABI remain unchanged. 30773 native
  owner checks, 34 actual metadata/query checks, 32 preflight checks and caps/layout tests pass;
  stale-version mutations fail two checks each; full WDK build passes. Not deployed.

Parent should archive applicable sources/logs before replacing this scratch path with evidence.
