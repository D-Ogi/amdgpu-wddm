// System physical address of a VRAM page to the MC address an SDMA packet wants.
//
// VidMmTranslate (vidmm.c) returns a system physical address: the PTE walker adds vram_base, which
// vram.c stores as VramPhysical (facts M31, amdgpu's vram_base_offset). bc250_sdma_emit_fill and
// emit_copy_linear take the other number, the MC address, which is what gpumem.c stores as entry->Mc
// (VramMcBase + offset) and what M95's sdmacopy actually ran. They are a fixed distance apart on this
// part: MC = VramMcBase + (physical - VramPhysical). Feeding the physical one to SDMA is what E24 run
// 007b page-faulted on (facts M113): UTCL2 tried to translate 0x271C62000 and nacked it.
//
// No WDK header. Host-tested by driver/kmd/test/run_paging_mc.ps1 and compiled into the driver as-is.
#pragma once

// *Mc is zeroed on refusal. bytes is the length the packet will cover from physical, so a start that
// fits and an end that does not is a refusal rather than a packet that runs off the carve-out.
int PagingPhysicalToMc(unsigned long long physical, unsigned long long bytes,
                       unsigned long long vramPhysical, unsigned long long vramMc,
                       unsigned long long vramLength, unsigned long long* mc);
