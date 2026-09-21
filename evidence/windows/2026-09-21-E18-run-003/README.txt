E18 runs 002 and 003, 2026-09-21: VidMm's page tables written in the hardware's format, and witnessed
=====================================================================================================

Run 002: bc250kmd 0.7.11 (commit cc6096b, .sys sha256 prefix adf079dd8c75a763), EnableGpuVa = 1, EnableVramWrite = 1.
  23:33:43  gate open. ring-b2-233415.log, ring-summary5-233421.log: 1028 CPU_VIRTUAL calls (kernel pointers,
            0xFFFF9A0080F60000 onwards, one page apart) and 41 GPU_PHYSICAL calls; 280643 entries written, 0 refused,
            0 bad calls; presents and fences as in stage A, no TDR, no bugcheck.
            Translations in the log: directory 0x21 / 0x1FD732 -> 0x46DFFC001; host page 0x1 / 0x18E20D ->
            0x18E20D073; VRAM leaf 0x21 / 0x204 -> 0x270ACE071, which is the shared primary: physical 0x270ACE000 =
            VRAM offset 0xACE000 = the address SetVidPnSourceAddress gets (MC 0xF400ACE000).
            Witness (bc250kmd_cli vtable, read by physical address): the paging process's root at VRAM 0x8CA000 has
            entry 0 = 0x2708CB001 and the walk reaches 512 leaves 0x2708CD071.. . The CDD process's root (VRAM
            0x1FDFFF000) was refused by the read escape: ACCESS_DENIED above BAR0's reach (fixed in 0.7.12).

Between the runs: installing 0.7.12 over a RUNNING full table failed - Kernel-PnP 225, "dwm.exe stopped the removal",
  device left in CM_PROB_FAILED_POST_START and "pending system reboot" (install-x-233945, state-pre-b3-234019,
  gate-x-234109). Restart at 23:42:20 (gates closed): display-only 0.7.12.1, stage 61 (state-boot-234406).
  Procedure from now on: close the gate (reload the display-only table) BEFORE installing a package.

Run 003: bc250kmd 0.7.12 (commit 24c9ba5, .sys sha256 prefix f34f0988f12561fd, package-manifest.txt), same gates.
  23:44:45  gate open, same counts as run 002 (ring-b3-234518.log, ring-summary5-234523.log).
  23:45:30  kmtprobe (tools/win/kmtprobe, D3DKMT only, no context): kmtprobe-234530.txt - CreateDevice,
            CreatePagingQueue, CreateAllocation2 (64 KB), MapGpuVirtualAddress at 0x10000000 "honoured", MakeResident,
            Lock2, 8192 pattern qwords, teardown, "all steps passed". walk-234530.txt walked the CDD root (the only
            one in the log: a client without a context gets no SetRootPageTable) and found nothing at that VA, rightly.
  23:47:05  second kmtprobe with the scan witness (scan-234705.txt): below the CDD's tables VidMm built a new chain,
            root VRAM 0x1FDFF3000 -> 0x1FDFF2000 -> level 1 at 0x1FDFF1000, entry 128 = 0x46DFF0001 -> level 0 at
            0x1FDFF0000 with 16 leaves 0x271C62071 .. 0x271C71071 for VA 0x10000000 .. 0x1000F000. The page of leaf 0
            (VRAM 0x1C62000) reads 0x4243323530420000, ..01, ..02; the page of leaf 15 reads 0x4243323530421E00 =
            pattern + 15 * 512. (The scan prints the VA once in decimal, "0x268435456": a format slip of the script.)
