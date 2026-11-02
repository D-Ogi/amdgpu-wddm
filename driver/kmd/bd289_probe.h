// BD-289 diagnostic, lab flavour only: is the CPU present blit's source mapping (wddm.c WddmPresentBlitOwned,
// MmMapIoSpaceEx of the source range) still mapped at each later step of the same present, and do two CPU present
// blits run at the same time?
//
// The crash dump of 2026-11-02 08:24Z (tester.35 KMD 9D4E0C65) shows every PTE of that source mapping zero at the
// copy, while the mapping call had returned a VA. Between that call and the copy, the only unmap in the same
// thread is DcnUnmapScanout inside DcnScanoutMapping (dcn.c), which releases the old flip target mapping. These
// probes find the step at which the source mapping stops being valid. They do not repair anything.
//
// Steps, in the order a present reaches them:
//   MAP      right after the source MmMapIoSpaceEx (wddm.c)
//   UNMAP    inside DcnScanoutMapping, right after DcnUnmapScanout released the old flip target (dcn.c)
//   REMAP    inside DcnScanoutMapping, right after the new flip target MmMapIoSpaceEx (dcn.c)
//   SCANOUT  right after DcnScanoutMapping returned (wddm.c)
//   DEST     right after the destination MmMapIoSpaceEx (wddm.c)
//   COPY     right before the row copy (wddm.c)
//   END      right before the source MmUnmapIoSpace (wddm.c)
// Each step records one state byte (BD289_STATE_*): the first page, the second page and the last page of the
// source by MmIsAddressValid, and whether MmGetPhysicalAddress of the first page is the physical address that
// was mapped. COPY and END also count the invalid pages of the whole source range.
//
// Only the thread that claimed the probe (Bd289ProbeBegin) writes into it. A remap or a step from another thread
// while a probe is open is counted as foreign: a concurrent unmap would show there.
//
// r3: concurrency of the whole blit (WddmPresentBlit). Arrive/Depart count the blits that are on their way, before
// any lock; Enter/Leave count the blits inside the section that a lock guards (the connector operation mutex when
// the connector is enabled, the scan-out mutex of the BD-289 fix in every case). Enter records whether the blit
// holds a lock. A remap while another blit is inside is counted (remaps-concurrent). Every bd289 line also goes
// into a ring of its own (32 lines kept from the start of the driver load, 96 rotating), which the summary of
// `bc250kmd_cli log summary only` writes out with the counters, so that a busy log ring cannot overwrite it.
// IRQL: PASSIVE_LEVEL, as the present blit (MmIsAddressValid and MmGetPhysicalAddress allow up to DISPATCH_LEVEL).
#pragma once

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)

#define BD289_STEP_MAP      0u
#define BD289_STEP_UNMAP    1u
#define BD289_STEP_REMAP    2u
#define BD289_STEP_SCANOUT  3u
#define BD289_STEP_DEST     4u
#define BD289_STEP_COPY     5u
#define BD289_STEP_END      6u
#define BD289_STEPS         7u

#define BD289_STATE_FIRST    0x01u      // MmIsAddressValid(first page)
#define BD289_STATE_SECOND   0x02u      // MmIsAddressValid(first page + PAGE_SIZE), or set when the range is one page
#define BD289_STATE_LAST     0x04u      // MmIsAddressValid(last page)
#define BD289_STATE_PHYSICAL 0x08u      // the first page translates to the physical address that was mapped
#define BD289_STATE_REACHED  0x80u      // the step ran in this present
#define BD289_STATE_GOOD     (BD289_STATE_FIRST | BD289_STATE_SECOND | BD289_STATE_LAST | BD289_STATE_PHYSICAL)

#define BD289_INFO_LINES 40             // the first probes, every remap, the summary at each power of two probes
#define BD289_LOST_LINES 40             // the probes at which the source stopped being mapped

// Claims the probe for this present. FALSE: another present holds it (counted), so this present is not probed.
BOOLEAN Bd289ProbeBegin(_In_ const void* Map, SIZE_T Length, ULONGLONG Physical, ULONG Protect);
// Records one step. Returns FALSE when the source is not fully mapped at this step (the caller of COPY skips the
// copy, so that the lab keeps its log instead of a second 0x0A).
BOOLEAN Bd289ProbeStep(ULONG Step);
// DcnScanoutMapping, before DcnUnmapScanout: the old flip target mapping and the new target. From every thread.
void Bd289ProbeRemap(_In_opt_ const void* OldMap, ULONGLONG OldPhysical, SIZE_T OldLength,
                     ULONGLONG NewPhysical, SIZE_T NewLength);
// DcnScanoutMapping, after the new MmMapIoSpaceEx (NULL when it failed).
void Bd289ProbeRemapped(_In_opt_ const void* NewMap);
// Writes the lines and releases the probe. Returns TRUE when the source may be unmapped: every page is still
// mapped to the physical range this present mapped. FALSE: its PTEs were already released or reused, and a
// second MmUnmapIoSpace would release PTEs that this present no longer owns.
BOOLEAN Bd289ProbeEnd(void);

// WddmPresentBlit: Arrive before any lock, Enter after the locks (Locked: the blit holds the connector operation
// mutex or the scan-out mutex), Leave before the locks are released, Depart after.
void Bd289BlitArrive(void);
void Bd289BlitEnter(BOOLEAN Locked);
void Bd289BlitLeave(void);
void Bd289BlitDepart(void);
// WddmSummary: the counters and the bd289 ring, into the summary block. PASSIVE_LEVEL.
void Bd289ProbeSummary(void);

#endif
