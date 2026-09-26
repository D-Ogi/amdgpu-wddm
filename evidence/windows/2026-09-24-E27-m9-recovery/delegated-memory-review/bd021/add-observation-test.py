from pathlib import Path
r=Path('bc250-win/experiments/E27-m9-inference')
p=r/'paging-route-test-suffix.c';s=p.read_text();marker='static void case_retained_mapping(void)'
new='''// BD-021 observations must preserve OS segment/level identity without changing
// the encoding. Both normal GPU updates and borrowed CPU table initialization count.
static void case_pte_observations(void)
{
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE ptes[3];
 ULONGLONG encoded[3],physical;unsigned level,kind,segment;
 memset(&g_VidMm,0,sizeof(g_VidMm));memset(cpu_storage,0,sizeof(cpu_storage));
 g_VidMm.Ready=1;g_VidMm.Write=1;g_VidMm.SegmentMapping=(unsigned char*)cpu_storage;
 g_VidMm.SegmentPhysical=0x200100000ull;g_VidMm.SegmentLength=sizeof(cpu_storage);
 g_VidMm.Pte.units=BC250_PTE_ADDR_PAGES;g_VidMm.Pte.aperture=BC250_PTE_VM;
 g_VidMm.Pte.system_segment=0;g_VidMm.Pte.vram_segment=1;g_VidMm.Pte.table_segment=3;
 g_VidMm.Pte.vram_base=0x200000000ull;g_VidMm.Pte.vram_size=0x100000;
 g_VidMm.Pte.table_base=g_VidMm.SegmentPhysical;g_VidMm.Pte.table_size=g_VidMm.SegmentLength;
 (void)PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4);
 u.pPageTableEntries=ptes;u.NumPageTableEntries=3;
 for(level=0;level<4;level++)for(kind=0;kind<3;kind++) {
  segment=kind==0?0:(kind==1?1:3);u.PageTableLevel=level;
  ptes[0].Flags=BC250_DXGK_PTE_VALID|BC250_DXGK_PTE_CACHECOHERENT|((u64)segment<<BC250_DXGK_PTE_SEGMENT_SHIFT);
  ptes[0].PageAddress=kind==0?0x123:0;
  ptes[1]=ptes[0];ptes[1].Flags&=~BC250_DXGK_PTE_CACHECOHERENT;
  ptes[2].Flags=~BC250_DXGK_PTE_VALID;ptes[2].PageAddress=~0ull;
  u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical.SegmentId=3;u.PageTableAddress.GpuPhysical.SegmentOffset=4096;
  check(VidMmEncodePageTable(&u,0,3,&physical,encoded),"observation GPU encoding accepts local/system leaf/directory controls");
  check(g_VidMm.EncodedCoherent[level][kind]==1 && g_VidMm.EncodedNoncoherent[level][kind]==1 && !g_VidMm.EncodedSnoopMismatch[level][kind],"GPU observation preserves segment and level; invalid entry excluded");
  check((encoded[0]&AMDGPU_PTE_SNOOPED) && !(encoded[1]&AMDGPU_PTE_SNOOPED) && encoded[2]==0,"observations do not change requested snoop policy");
  u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_table;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"observation CPU initialization accepts same controls");
  check(g_VidMm.EncodedCoherent[level][kind]==2 && g_VidMm.EncodedNoncoherent[level][kind]==2 && !g_VidMm.EncodedSnoopMismatch[level][kind],"CPU observation includes borrowed local tables by input identity");
 }
 u.PageTableLevel=0;u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical.SegmentId=3;u.PageTableAddress.GpuPhysical.SegmentOffset=4096;
 check(VidMmEncodePageTable(&u,1,1,&physical,encoded) && g_VidMm.EncodedCoherent[0][2]==2 && g_VidMm.EncodedNoncoherent[0][2]==3,"encoding slice counts only selected noncoherent table entry");
 u.Flags.Repeat=1;
 check(VidMmEncodePageTable(&u,1,2,&physical,encoded) && g_VidMm.EncodedCoherent[0][2]==4,"Repeat counts two selected output entries without indexing beyond source");
 encoded[0]^=AMDGPU_PTE_SNOOPED;VidMmCountEncoding(&u,1,2,encoded);
 check(g_VidMm.EncodedSnoopMismatch[0][2]==1 && !g_VidMm.EncodedSnoopMismatch[0][0] && !g_VidMm.EncodedSnoopMismatch[0][1],"mismatched snoop witness is attributed to local table, not system/application");
}

'''
assert s.count(marker)==1;s=s.replace(marker,new+marker);p.write_text(s)
p=r/'generate-paging-route-test.py';s=p.read_text();s=s.replace(r'\tcase_retained_mapping();',r'\tcase_pte_observations();\n\tcase_retained_mapping();',1);p.write_text(s)
