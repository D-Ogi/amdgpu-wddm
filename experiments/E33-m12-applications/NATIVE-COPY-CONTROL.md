# Explicit Windows mapping-copy control

Hypothesis: explicit D3DDDI_UPDATEGPUVIRTUALADDRESS_COPY operations exercise
KMD151 COPY_PAGE_TABLE_ENTRIES, unlike the MAP_PROTECT CTS path (M489).

Use native_sparse_control.c --dma --copy-map. First verify physical A and B
with the existing CP DMA_DATA readback oracle. Copy one 64 KiB mapping from
A into a separate reserved range, behind an unsignaled application fence.
Observe the companion fence remains zero after 20 ms, signal the application
fence, wait for completion and read the aliased word. Repeat with source B,
then unmap and tear down. No reads from holes are part of this control.

Require the exact A/B values, bounded real fences, unchanged KMD151 identity,
1000 MHz/VID116 below 85 C, and no owner STOP. Save KMD summaries before and
after. A content pass without an increased native-copy range count does not
establish execution of the new DDI path. Stop on first failure or timeout.

Reference: local ref/ddi-display/d3dukmdt.md and d3dkmthk.md, WDK26100,
D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION.Copy and
D3DKMTUpdateGpuVirtualAddress. Each call has one source reservation and one
destination reservation, which may differ. This test does not yet induce
OS relocation of a tile pool or establish full M12.1 acceptance.
