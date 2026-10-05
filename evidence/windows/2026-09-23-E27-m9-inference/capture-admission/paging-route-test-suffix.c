static PAGING_PT_SHADOW_SLOT route_shadow_slots[4];

static void case_kmd_routes(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 BC250_DEVICE dev={{0x200000000ll},0x100000000ull,0x100000,NULL,0,0,0,0};
 BC250_GFX gfx={1,{0,0},&g_adev,0,NULL,0};BC250_PAGING_STREAM st;
 u32 buf[256];u64 out;unsigned written;static u64 scratch[16];
 struct amdgpu_vmhub*hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"actual route window configured");
 memset(&st,0,sizeof(st));st.Device=&dev;st.Gfx=&gfx;st.Root=4096;st.Payload=buf;st.CommandOffset=128;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 translated=0x12345123ull;isSystem=1;
 check(PagingResolve(&st,0,64,&out) && out==(translated|PAGING_SYSTEM_ADDRESS),"actual resolver preserves system physical address and offset");
 check(PagingEmit(&st,buf,256,out,0x100000200ull,64,&written)==0 && written==83,"system to local builds mapped transfer");
 check(buf[8+3]==97 && buf[33+3]==(u32)(gfx.PagingWindow.mc+0x123) && buf[33+5]==0x200,"actual route marker includes prior command offset and uses source window");
 check(PagingEmit(&st,buf,256,0x100000200ull,out,64,&written)==0 && buf[33+5]==(u32)(gfx.PagingWindow.mc+4096+0x123),"local to system uses destination window");
 check(PagingEmit(&st,buf,256,out,out,64,&written)==0 && buf[4]!=0 && buf[6]!=0,"system to system maps two separate slots");
 st.Fill=1;st.Pattern=0xABCD1234;out=(0x12345200ull|PAGING_SYSTEM_ADDRESS);
 check(PagingEmit(&st,buf,256,0,out,64,&written)==0 && written==81 && buf[33]==SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL) && buf[36]==st.Pattern,"system fill uses mapped transaction and preserves pattern");
 st.Fill=0;isSystem=0;translated=0x200000200ull;
 check(PagingResolve(&st,0,64,&out) && out==0x100000200ull,"local resolver retains physical-to-MC conversion");
 check(PagingEmit(&st,buf,256,out,out+64,64,&written)==0 && written==7,"local/local keeps direct packet path");
 isSystem=1;translated=0x1000000000000000ull;
 check(!PagingResolve(&st,0,64,&out),"unrepresentable system page rejected instead of truncation");
 translated=0x12345000;gfx.PagingWindowReady=0;check(!PagingResolve(&st,0,64,&out),"unavailable aperture rejected");
 {
  unsigned done=0,next,dw,totalDw=0,passes=0;int result;
  gfx.PagingWindowReady=1;fragmented=1;isSystem=1;st.CommandOffset=0;st.Fill=0;
  do {
   st.Payload=buf;
   result=PagingStreamBuild(&st,PagingResolve,PagingEmit,0,0x10000,0x30000,5*4096,done,buf,180,&dw,&next);
   check(result==PagingStreamMore || result==PagingStreamDone,"actual mapped page stream publishes valid partial batch");
   check(next>done && next<=5*4096 && dw==83*((next-done)/4096),"mapped multipass preserves data and command coordinates");
   if(dw==166) check(buf[8+3]==1 && buf[83+8+3]==250,"consecutive transactions use distinct command-position marker ranges");
   totalDw+=dw;done=next;passes++;
  }while(result==PagingStreamMore && passes<10);
  check(done==5*4096 && totalDw==5*83 && passes==3,"five scattered page pairs covered exactly in three bounded batches");
  fragmented=0;
 }
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_kmd_flush(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub*hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 u32 buf[32];ULONG written;BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 unsigned offset;NTSTATUS status;
 dev.Gfx=&gfx;gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 test_expected_vmid=1;memset(buf,0xCC,sizeof(buf));
 status=GfxPagingBuildFlush(&dev,1,buf,0,sizeof(buf),&written,&unsupported);
 check(status==STATUS_SUCCESS && written==15 && unsupported==BC250PagingSupported,"actual KMD flush builds VMID1 packet");
 check(buf[12]==2 && buf[13]==2 && all_pattern(buf+15,17,0xCCCCCCCCu),"actual KMD flush targets app VMID not GART");
 for(offset=0;offset<=4096;offset+=4) {
  unsigned cap=PagingStreamCapacity(64,offset,65536,1024,ring.funcs->align_mask,bc250_sdma_fence_size(&ring,AMDGPU_FENCE_FLAG_INT));
  memset(buf,0xCC,sizeof(buf));
  status=GfxPagingBuildFlush(&dev,1,buf,offset,64,&written,&unsupported);
  check(cap>=16 ? status==STATUS_SUCCESS && written==15 : status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && written==0,"flush accounts for earlier DMA commands and outer fence");
  if(cap<16)check(all_pattern(buf,32,0xCCCCCCCCu),"insufficient flush publishes no partial packet");
 }
 gfx.PagingReady=0;written=123;
 check(GfxPagingBuildFlush(&dev,1,buf,0,128,&written,&unsupported)==STATUS_DEVICE_NOT_READY && !written && unsupported==BC250PagingNotReady,"not-ready remains explicit helper failure");
 dev.Gfx=NULL;
 check(GfxPagingBuildFlush(&dev,1,buf,0,128,&written,&unsupported)==STATUS_DEVICE_NOT_READY && !written,"missing engine refuses flush");
 check(GfxPagingBuildFlush(&dev,0,buf,0,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"application flush refuses GART VMID");
 check(GfxPagingBuildFlush(&dev,16,buf,0,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"invalid VMID rejected");
 check(GfxPagingBuildFlush(&dev,1,buf,1,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"unaligned DMA offset rejected");
 check(!flush_lock_depth && !flush_region_depth,"all flush exits balance lifetime lock/region");
 test_expected_vmid=0;memset(hub,0,sizeof(*hub));
}

static void case_kmd_updates(void)
{
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update={0};DXGK_PTE ptes[512];
 u32 buf[1056];ULONGLONG encoded[512],physical;static u64 scratch[16];
 ULONG written,next;unsigned i;BC250_WDDM_PAGING_UNSUPPORTED unsupported;NTSTATUS rc;
 dev.VramPhysical.QuadPart=0x200000000ll;dev.VramMcBase=0x100000000ull;dev.VramLength=0x1000000;dev.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.SegmentMapping=(unsigned char*)cpu_storage;g_VidMm.Ready=1;g_VidMm.Write=1;
 (void)PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4);g_VidMm.SegmentPhysical=0x200000000ull;g_VidMm.SegmentLength=0x1000000;
 g_VidMm.Pte.units=BC250_PTE_ADDR_PAGES;g_VidMm.Pte.aperture=BC250_PTE_VM;
 g_VidMm.Pte.system_segment=0;g_VidMm.Pte.vram_segment=1;
 g_VidMm.Pte.vram_base=g_VidMm.SegmentPhysical;g_VidMm.Pte.vram_size=g_VidMm.SegmentLength;
 update.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;update.NumPageTableEntries=512;
 update.PageTableAddress.GpuPhysical.SegmentId=1;update.PageTableAddress.GpuPhysical.SegmentOffset=4096;
 update.pPageTableEntries=ptes;
 for(i=0;i<512;i++){ptes[i].Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);ptes[i].PageAddress=100+i;}
 check(VidMmEncodePageTable(&update,0,512,&physical,encoded) && physical==0x200001000ull,"actual encoder translates full PTE source and table physical address");
 {
  DXGK_PTE saved=ptes[0];
  ptes[0].Flags=BC250_DXGK_PTE_VALID|BC250_DXGK_PTE_CACHECOHERENT;ptes[0].PageAddress=0x123;
  check(VidMmEncodePageTable(&update,0,1,&physical,encoded) && g_VidMm.EncodedCoherentSystem==1 &&
        !g_VidMm.EncodedUncachedSystem && !g_VidMm.EncodedCoherencyMismatch && (encoded[0]&AMDGPU_PTE_SNOOPED),
        "actual encoder records coherent system leaf with matching snoop");
  ptes[0].Flags=BC250_DXGK_PTE_VALID;
  check(VidMmEncodePageTable(&update,0,1,&physical,encoded) && g_VidMm.EncodedUncachedSystem==1 &&
        !g_VidMm.EncodedCoherencyMismatch && !(encoded[0]&AMDGPU_PTE_SNOOPED),
        "actual encoder records noncoherent system leaf without snoop");
  ptes[0]=saved;
  check(VidMmEncodePageTable(&update,0,512,&physical,encoded),"restore local update oracle after coherency controls");
 }
 memset(buf,0xCC,sizeof(buf));g_VidMm.GpuCalls=0;
 rc=GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported);
 check(rc==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && next==480 && written==974,"actual builder publishes480entries then requests next buffer");
 check(buf[1]==0x1000 && buf[2]==1 && buf[3]==959,"PTE table destination converted to MC");
 for(i=0;i<480;i++)if(buf[4+2*i]!=(u32)encoded[i] || buf[5+2*i]!=(u32)(encoded[i]>>32))break;
 check(i==480 && !g_VidMm.GpuCalls,"queued first batch preserves values without CPU table write");
 check(buf[967]==1 && all_pattern(buf+974,82,0xCCCCCCCCu),"first update barrier marker and tail bounded");
 rc=GfxPagingBuildUpdate(&dev,&update,buf,0,65536,next,&written,&next,&unsupported);
 check(rc==STATUS_SUCCESS && next==512 && written==78 && buf[1]==0x1F00 && buf[3]==63,"second batch resumes table offset by480entries");
 for(i=0;i<32;i++)if(buf[4+2*i]!=(u32)encoded[480+i] || buf[5+2*i]!=(u32)(encoded[480+i]>>32))break;
 check(i==32,"second batch covers exactly remaining entries");
 memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildUpdate(&dev,&update,buf,4048,65536,0,&written,&next,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && !next && all_pattern(buf,1056,0xCCCCCCCCu),"insufficient accumulated ring budget writes nothing");
 update.NumPageTableEntries=2;update.StartIndex=17;update.Flags.Repeat=1;
 check(GfxPagingBuildUpdate(&dev,&update,buf,128,65536,0,&written,&next,&unsupported)==STATUS_SUCCESS && next==2 && buf[1]==0x1088 && buf[4]==buf[6] && buf[5]==buf[7] && buf[11]==97,"Repeat and StartIndex preserved, marker includes command offset");
 update.Flags.Repeat=0;update.StartIndex=0;update.NumPageTableEntries=512;
 ptes[511].Flags=BC250_DXGK_PTE_VALID | (2ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_INVALID_PARAMETER && !written && all_pattern(buf,1056,0xCCCCCCCCu),"invalid final source entry prevents first batch publication");
 ptes[511].Flags=ptes[0].Flags;
 gfx.PagingCpuBootstrap=1;gfx.PagingReady=0;
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_SUCCESS && !written && next==512 && g_VidMm.GpuCalls==1,"pre-RUN bootstrap uses immediate CPU update");
 gfx.PagingCpuBootstrap=0;
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_DEVICE_NOT_READY && !written && !next && g_VidMm.GpuCalls==1,"after bootstrap close not-ready cannot fall back to CPU");
 update.StartIndex=1;
 check(!VidMmEncodePageTable(&update,0,1,&physical,encoded),"encoder rejects table extent overflow");
 update.StartIndex=0;update.PageTableLevel=4;
 check(!VidMmEncodePageTable(&update,0,1,&physical,encoded),"encoder rejects invalid page-table level");
 check(!flush_lock_depth && !flush_region_depth,"PTE update exits balance lifetime locks");
}

static void case_cpu_updates(void)
{
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE ptes[512];
 BC250_DEVICE dev={0};BC250_GFX gfx={0};u32 output[16];
 ULONGLONG encoded[512],physical;ULONG written,next;BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 unsigned i;long long priorWritten;int maps;ULONGLONG shadowEntry;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical.SegmentId=1;
 u.PageTableAddress.GpuPhysical.SegmentOffset=4096;u.NumPageTableEntries=2;u.StartIndex=5;u.pPageTableEntries=ptes;
 for(i=0;i<512;i++){ptes[i].Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);ptes[i].PageAddress=100+i;}
 memset(cpu_table,0xCC,4096);priorWritten=g_VidMm.Written;
 check(VidMmEncodePageTable(&u,0,2,&physical,encoded),"CPU control encoded");
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && cpu_table[5]==encoded[0] && cpu_table[6]==encoded[1] && g_VidMm.Written==priorWritten+2,"actual CPU physical update writes requested entries");
 check(cpu_table[4]==0xCCCCCCCCCCCCCCCCull && cpu_table[7]==0xCCCCCCCCCCCCCCCCull && !cpu_map_live,"CPU update bounds and lifetime preserved");
 memset(cpu_table,0xCC,4096);g_VidMm.SegmentMapping=NULL;priorWritten=g_VidMm.Written;
 check(VidMmUpdatePageTable(&u)==STATUS_DEVICE_NOT_READY && g_VidMm.Written==priorWritten && !cpu_map_live,"missing retained mapping refuses without write");
 dev.Gfx=&gfx;gfx.PagingCpuBootstrap=1;
 check(GfxPagingBuildUpdate(&dev,&u,output,0,sizeof(output),0,&written,&next,&unsupported)==STATUS_DEVICE_NOT_READY && !written && !next,"bootstrap preserves zero progress after missing retained mapping");
 g_VidMm.SegmentMapping=(unsigned char*)cpu_storage;maps=cpu_map_calls;u.NumPageTableEntries=512;u.StartIndex=0;
 ptes[511].Flags=BC250_DXGK_PTE_VALID | (2ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_map_calls==maps,"late invalid PTE rejected before destination mapping");
 for(i=0;i<512;i++)if(cpu_table[i]!=0xCCCCCCCCCCCCCCCCull)break;
 check(i==512,"late invalid PTE leaves complete destination unchanged");
 ptes[511].Flags=ptes[0].Flags;u.NumPageTableEntries=3;u.StartIndex=9;u.Flags.Repeat=1;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_table;
 cache_attribute_calls=0;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && cpu_table[9]==encoded[0] && cpu_table[10]==encoded[0] && cpu_table[11]==encoded[0] && cpu_map_calls==maps && !cpu_map_live,"CPU_VIRTUAL Repeat uses provided pointer without map/unmap");
 check(cache_attribute_calls==1 && cache_attribute_physical==g_VidMm.SegmentPhysical+4096,
       "borrowed table cache query uses validated physical identity, not CPU VA or MC address");
 check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,9,&shadowEntry)==PAGING_PT_OK && shadowEntry==encoded[0],"CPU_VIRTUAL initialization registers physical identity and encoded entry");
 check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,8,&shadowEntry)==PAGING_PT_MISSING,"partial initialization does not invent adjacent PTEs");
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical.SegmentId=1;u.PageTableAddress.GpuPhysical.SegmentOffset=4096;
 ptes[0].PageAddress=77;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,9,&shadowEntry)==PAGING_PT_OK && shadowEntry==cpu_table[9],"immediate bootstrap updates known logical table with CPU table");
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_table;
 // Fill registration capacity, then ensure a new physical table cannot be partly written.
 { unsigned slot;ULONGLONG value=0;
   for(slot=2;slot<=4;slot++)check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+slot*4096,0,1,&value,1)==PAGING_PT_OK,"fill small host registration capacity");
   u.PageTableAddress.CpuVirtual=cpu_storage+5*512;cpu_storage[5*512+9]=0xCCCCCCCCCCCCCCCCull;
   check(VidMmUpdatePageTable(&u)==STATUS_INSUFFICIENT_RESOURCES && cpu_storage[5*512+9]==0xCCCCCCCCCCCCCCCCull,"registration full refuses before CPU destination write");
 }
 g_VidMm.Write=0;
 check(VidMmUpdatePageTable(&u)==STATUS_DEVICE_NOT_READY,"closed CPU write gate does not claim success");
 g_VidMm.Write=1;u.Flags.Use64KBPages=1;
 check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER,"unsupported CPU large-page mode rejected");
 check(VidMmUpdatePageTable(NULL)==STATUS_INVALID_PARAMETER,"NULL CPU update refused");
 check(!cpu_map_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"failure paths release maps and snapshot/builder locks");
}

static void case_retained_mapping(void)
{
 BC250_DEVICE d={0};ULONGLONG pa,base=0x200000000ull;BOOLEAN system;int calls;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;d.VramLength=sizeof(cpu_storage);
 cpu_map_fail=0;cpu_write_setting=1;calls=cpu_map_calls;
 shadow_pool_fail=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_INSUFFICIENT_RESOURCES && !g_VidMm.Ready && !shadow_pool_live && cpu_map_calls==calls,"shadow storage failure before mapping/readiness");
 shadow_pool_fail=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS && g_VidMm.Ready && g_VidMm.Write && cpu_map_live==1 && cpu_map_calls==calls+1,"actual VidMm start acquires one retained segment mapping");
 memset(cpu_storage,0,sizeof(cpu_storage));
 cpu_storage[512]=(base+8192)|7;cpu_storage[1024]=(base+12288)|7;cpu_storage[1536]=(base+16384)|7;
 cpu_storage[2048]=(base+24576)|bc250_pte_vm_flags(1,1,0,0);
 calls=cpu_map_calls;
 check(VidMmTranslateRetained(base+4096,0x123,&pa,&system) && pa==base+24576+0x123 && !system,"actual retained walk resolves local leaf and offset");
 cpu_storage[2048]=0x100000|bc250_pte_vm_flags(1,1,1,1);
 check(VidMmTranslateRetained(base+4096,0x234,&pa,&system) && pa==0x100234 && system,"retained walk resolves system leaf without mapping host data");
 cpu_storage[1024]=0;
 check(!VidMmTranslateRetained(base+4096,0,&pa,&system) && !pa,"retained walk refuses missing directory");
 check(!VidMmTranslateRetained(base+sizeof(cpu_storage),0,&pa,&system),"retained walk bounds root against segment");
 check(cpu_map_calls==calls && cpu_map_live==1,"walk performs no dynamic map/unmap");
 VidMmStop();
 check(!g_VidMm.SegmentMapping && !g_VidMm.Ready && !g_VidMm.Write && !cpu_map_live && !shadow_pool_live && !g_VidMm.Shadow.Slots,"stop drops retained mapping and shadow after drain");
 check(!VidMmTranslateRetained(base+4096,0,&pa,&system),"post-stop translation refused");
 cpu_map_fail=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_INSUFFICIENT_RESOURCES && !g_VidMm.Ready && !g_VidMm.Write && !g_VidMm.SegmentMapping && !shadow_pool_live,"mapping resource failure occurs at start before readiness");
 cpu_map_fail=0;calls=cpu_map_calls;
 check(VidMmStart(&d,4096,sizeof(cpu_storage),1)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls,"invalid segment extent rejected before mapping");
 cpu_write_setting=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS && !g_VidMm.SegmentMapping && !g_VidMm.Write,"closed write gate retains diagnostic start without mapping");
 VidMmStop();cpu_write_setting=1;
 check(!cpu_map_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"mapping lifecycle balances ownership");
}

/* Construction ordering acceptance, also run by the default routing suite. */
static void case_queued_pte_ordering(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_STREAM stream={0};DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte,ptes[512];
 DXGKARG_BUILDPAGINGBUFFER build={0};
 u32 commands[1056],priv[1100],dma[1056];ULONGLONG base=0x200000000ull,pa,entry;BOOLEAN system;
 PAGING_U64 mc=0;ULONG written,next;unsigned i;
 BC250_WDDM_PAGING_UNSUPPORTED unsupported;static u64 scratch[16];
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;
 d.VramLength=sizeof(cpu_storage);d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"ordering probe start");
 memset(cpu_storage,0,sizeof(cpu_storage));
 // Actual immediate CPU initialization registers all four pinned table identities.
 pte.Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;pte.PageAddress=i==4?6:i+1;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"register initialized paging hierarchy");
 }
 stream.Device=&d;stream.Gfx=&gfx;stream.Root=base+4096;use_retained_walk=1;
 check(PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"positive control logical resolver sees initialized page6");
 pte.PageAddress=7;u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
 u.PageTableAddress.GpuPhysical.SegmentId=1;u.PageTableAddress.GpuPhysical.SegmentOffset=16384;
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),0,&written,&next,&unsupported)==STATUS_SUCCESS && next==1 && written==16,"actual GPU update builder stages remap to page7");
 check(PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"unpublished candidate cannot change construction state");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 memset(dma,0xCC,sizeof(dma));
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);
 build.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES+written*4-1;build.UpdatePageTable=u;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER,"short private space refuses publication");
 check(build.pDmaBuffer==dma && build.pDmaBufferPrivateData==priv && all_pattern(dma,1056,0xCCCCCCCCu) && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"private refusal leaves DMA pointers, bytes and logical state untouched");
 build.DmaBufferPrivateDataSize=sizeof(priv);build.DmaSize=written*4-1;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER,"short DMA space refuses publication");
 build.DmaSize=sizeof(dma);build.DmaBufferGpuVirtualAddress=~0ull-16;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"private address overflow cannot advance logical state");
 build.DmaBufferGpuVirtualAddress=0;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,2)==STATUS_INVALID_PARAMETER && !priv[0] && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"invalid logical batch invalidates candidate without advance");
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_SUCCESS,"actual publication helper commits valid batch");
 check(build.pDmaBuffer==dma+written && build.DmaSize==sizeof(dma)-written*4 && build.pDmaBufferPrivateData==(unsigned char*)priv+PAGING_PRIVATE_HEADER_BYTES+written*4 && memcmp(dma,commands,written*4)==0,"publication copies exact command bytes and advances both capacities");
 check(PagingResolve(&stream,0,4096,&mc),"logical resolver accepts published update before GPU executes");
 printf("QUEUED_UPDATE: wanted MC=0x%llX, builder resolved MC=0x%llX\n",d.VramMcBase+28672,(unsigned long long)mc);
 check(mc==d.VramMcBase+28672,"REQUIRED: following transfer construction resolves queued page7");
 check(VidMmTranslateRetained(base+4096,0,&pa,&system) && pa==base+24576 && !system,"GPU-visible table remains page6 until GPU executes queued update");
 cpu_storage[2048]=(u64)commands[4]|((u64)commands[5]<<32);
 check(VidMmTranslateRetained(base+4096,0,&pa,&system) && pa==base+28672,"modeled GPU payload application catches up with construction view");
 // Full update must expose only each accepted multipass prefix.
 for(i=0;i<512;i++){ptes[i].Flags=pte.Flags;ptes[i].PageAddress=6+(i&1);}
 u.NumPageTableEntries=512;u.pPageTableEntries=ptes;
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),0,&written,&next,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && next==480,"stage first480-entry logical batch");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);build.DmaBufferPrivateDataSize=sizeof(priv);build.UpdatePageTable=u;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_SUCCESS,"publish first multipass prefix");
 check(PagingPtShadowRead(&g_VidMm.Shadow,base+16384,479,&entry)==PAGING_PT_OK && PagingPtShadowRead(&g_VidMm.Shadow,base+16384,480,&entry)==PAGING_PT_MISSING,"first prefix cannot expose not-yet-built suffix");
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),480,&written,&next,&unsupported)==STATUS_SUCCESS && next==512,"stage remaining32-entry batch");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);build.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmPublishPagingRecord(&build,written,TRUE,480,next)==STATUS_SUCCESS,"publish second multipass prefix");
 for(i=0;i<512;i++)check(PagingResolve(&stream,(PAGING_U64)i*4096,4096,&mc) && mc==d.VramMcBase+(6+(i&1))*4096,"logical whole-table multipass translation");
 // An unregistered application page must not consume a pinned paging-table slot.
 u.NumPageTableEntries=1;u.pPageTableEntries=&pte;u.PageTableAddress.GpuPhysical.SegmentOffset=5*4096;
 i=g_VidMm.Shadow.Used;
 check(VidMmCommitPagingUpdate(&u,0,1)==STATUS_SUCCESS && g_VidMm.Shadow.Used==i,"ordinary application PTE updates do not register logical tables");
 check(!VidMmTranslateRetainedPaging(base+5*4096,0,&pa,&system),"unknown logical root never falls back to live VRAM");
 use_retained_walk=0;VidMmStop();
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"ordering probe cleans lifetime state");
}

static void case_copy_range_builder(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};static u64 scratch[16];
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE range={0};BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 u32 buf[64];ULONG written;ULONGLONG src,dst;unsigned cap;
 d.Gfx=&gfx;d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.VramLength=0x100000;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 range.NumPageTableEntries=512;range.SrcPageTableAddress=0x10000;range.DstPageTableAddress=0x20000;
 translated=0x200004000ull;isSystem=0;translationOk=1;fragmented=0;use_retained_walk=0;
 memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildCopyRange(&d,0x200001000ull,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_SUCCESS && written==34 && src==translated && dst==translated,"actual copy-range builder uses retained staging and reports resolved physical identity");
 check(buf[3]==0x4000 && buf[4]==1 && buf[5]==0x80000 && buf[6]==1 && buf[22]==0x4000,"copy-range builder converts resolved PA to MC, not raw GPU VA");
 check(all_pattern(buf+34,30,0xCCCCCCCCu),"range builder preserves output tail");
 range.NumPageTableEntries=2;range.SrcStartPteIndex=3;range.DstStartPteIndex=7;translation_by_offset=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,128,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_SUCCESS && src==translated+24 && dst==translated+56 && buf[1]==15 && buf[3]==0x4018 && buf[22]==0x4038 && buf[10]==97 && buf[27]==98,"copy-range indices and command-offset marker sequence are independent");
 translation_by_offset=0;range.NumPageTableEntries=512;range.SrcStartPteIndex=range.DstStartPteIndex=0;
 for(cap=0;cap<192;cap+=4) {
  memset(buf,0xCC,sizeof(buf));
  check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,cap,&written,&src,&dst,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && !src && !dst && all_pattern(buf,64,0xCCCCCCCCu),"range capacity refusal publishes no commands or identities");
 }
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,3984,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written,"accumulated ring bytes plus outer fence bound copy range");
 range.SrcStartPteIndex=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER,"source index plus count cannot exceed advertised table");
 range.SrcStartPteIndex=0;range.DstPageTableAddress++;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER,"copy table GPU VA must have documented64KiB alignment");
 range.DstPageTableAddress--;translationOk=0;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER && unsupported==BC250PagingNoTranslation,"missing logical translation refuses range");
 translationOk=1;isSystem=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER && unsupported==BC250PagingSystemMemory,"current local-only page-table segment requires local copy addresses");
 isSystem=0;gfx.PagingCopyStaging.size=0;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_DEVICE_NOT_READY && unsupported==BC250PagingNotReady,"missing staging resource refuses before output");
 check(!flush_lock_depth && !flush_region_depth,"range builder balances lifetime lock on all exits");
}

static void case_copy_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};static u64 scratch[16];
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE ranges[40];DXGKARG_BUILDPAGINGBUFFER b={0};
 u32 dma[1100],priv[1500];u64 values[41],entry;unsigned i,prior,passes=0;
 const u64 physical=0x200004000ull;
 d.Gfx=&gfx;d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.VramLength=0x100000;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 translated=physical;isSystem=0;translationOk=1;fragmented=0;use_retained_walk=0;translation_by_offset=1;
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=g_VidMm.Write=1;
 (void)PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4);
 for(i=0;i<41;i++)values[i]=100+i;
 check(PagingPtShadowApply(&g_VidMm.Shadow,physical,0,41,values,1)==PAGING_PT_OK,"copy publication initialized source/destination control");
 memset(ranges,0,sizeof(ranges));
 for(i=0;i<40;i++) {
  ranges[i].NumPageTableEntries=1;ranges[i].SrcPageTableAddress=0x10000;ranges[i].DstPageTableAddress=0x20000;
  ranges[i].SrcStartPteIndex=i;ranges[i].DstStartPteIndex=i+1;
 }
 b.CopyPageTableEntries.NumRanges=40;b.CopyPageTableEntries.pRanges=ranges;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.DmaBufferGpuVirtualAddress=~0ull-16;
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER && b.MultipassOffset==0 && b.pDmaBuffer==dma && !priv[0],"copy private header refusal preserves progress and invalidates record");
 check(PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_OK && entry==101 && all_pattern(dma,1100,0xCCCCCCCCu),"refused copy leaves logical state and DMA unchanged");
 b.DmaBufferGpuVirtualAddress=0;b.DmaSize=192;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && b.MultipassOffset==1 && b.DmaSize==56 && b.DmaBufferWriteOffset==0,"short buffer accepts exactly one whole range and preserves input offset");
 check(PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_OK && entry==100 && PagingPtShadowRead(&g_VidMm.Shadow,physical,2,&entry)==PAGING_PT_OK && entry==102,"accepted prefix visible and unaccepted suffix unchanged");
 while(b.MultipassOffset<40 && passes++<5) {
  NTSTATUS status;
  prior=b.MultipassOffset;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  b.DmaBufferWriteOffset=64;
  status=WddmBuildPagingCopies(&d,1,&b);
  check(b.MultipassOffset>prior && b.DmaBufferWriteOffset==64,"copy multipass makes progress and restores caller offset");
  check(status==(b.MultipassOffset==40?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER),"copy list reports insufficient until all ranges accepted");
  check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,(unsigned)(sizeof(dma)-b.DmaSize),0,NULL,NULL),"accepted copy records cover exact contiguous command span");
  for(i=1;i<=40;i++)check(PagingPtShadowRead(&g_VidMm.Shadow,physical,i,&entry)==PAGING_PT_OK && entry==(i<=b.MultipassOffset?100:100+i),"dependent range sees prior accepted logical copy across buffer boundary");
 }
 check(b.MultipassOffset==40 && passes==2,"live ring limit forces list into two further buffers");
 check(VidMmCommitPagingCopy(physical+1,physical+8,1)==STATUS_INVALID_PARAMETER,"unaligned physical copy identity refused");
 check(VidMmCommitPagingCopy(physical+4088,physical+8,2)==STATUS_INVALID_PARAMETER,"logical copy cannot cross table boundary");
 check(VidMmCommitPagingCopy(physical+4096,physical+8,1)==STATUS_SUCCESS && PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_MISSING,"unknown source clears registered destination knowledge");
 check(VidMmCommitPagingCopy(physical,physical+8192,1)==STATUS_SUCCESS && g_VidMm.Shadow.Used==1,"ordinary application destination never registered by copy");
 g_VidMm.Ready=0;
 check(VidMmCommitPagingCopy(physical,physical+8,1)==STATUS_DEVICE_NOT_READY,"post-stop copy commit refuses");
 b.MultipassOffset=0;b.CopyPageTableEntries.NumRanges=1;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_DEVICE_NOT_READY && b.MultipassOffset==0 && b.pDmaBuffer==dma && !priv[0] && b.DmaBufferWriteOffset==64 && all_pattern(dma,1100,0xCCCCCCCCu),"failed logical commit cannot publish DMA or advance copy progress");
 b.CopyPageTableEntries.NumRanges=0;
 check(WddmBuildPagingCopies(&d,0,&b)==STATUS_SUCCESS && b.MultipassOffset==0,"empty range list is a completed no-op");
 b.CopyPageTableEntries.NumRanges=1;b.CopyPageTableEntries.pRanges=NULL;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER,"nonempty range list requires array");
 b.CopyPageTableEntries.NumRanges=0;b.MultipassOffset=1;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER,"out-of-list progress refused");
 check(!cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"copy publication exits balance locks");
 translation_by_offset=0;
}

static void case_copy_real_walk(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE ranges[2]={{0}};DXGKARG_BUILDPAGINGBUFFER b={0};
 static u64 scratch[16];u64 staging[512],pa,expected;BOOLEAN system;
 u32 dma[256],priv[300];unsigned i;const u64 base=0x200000000ull;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;
 d.VramLength=sizeof(cpu_storage);d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"real-copy walker startup");
 memset(cpu_storage,0,sizeof(cpu_storage));
 pte.Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=7;i++) {
  u.PageTableLevel=i<4?4-i:0;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  pte.PageAddress=i<4?i+1:i==5?7:i==6?5:4;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"real-copy CPU initialization registers table identities");
 }
 u.PageTableAddress.CpuVirtual=cpu_storage+4*512;
 for(i=1;i<=3;i++) {
  u.StartIndex=i*16;pte.PageAddress=i+3;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"real-copy maps64KiB-aligned VAs to distinct tables");
 }
 expected=cpu_storage[7*512];use_retained_walk=1;
 check(VidMmTranslatePaging(base+4096,0x30000,&pa,&system) && pa==base+6*4096 && !system,"positive control source VA initially names physical page6");
 ranges[0].NumPageTableEntries=1;ranges[0].SrcPageTableAddress=0x20000;
 ranges[0].DstPageTableAddress=0x10000;ranges[0].DstStartPteIndex=48;
 ranges[1].NumPageTableEntries=1;ranges[1].SrcPageTableAddress=0x30000;
 ranges[1].DstPageTableAddress=0x20000;ranges[1].DstStartPteIndex=1;
 b.CopyPageTableEntries.NumRanges=2;b.CopyPageTableEntries.pRanges=ranges;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPagingCopies(&d,base+4096,&b)==STATUS_SUCCESS && b.MultipassOffset==2,"actual list builder accepts two address-dependent copies");
 check(dma[3]==5*4096 && dma[22]==4*4096+48*8,"first command copies remap PTE into actual leaf table");
 check(dma[34+3]==7*4096 && dma[34+22]==5*4096+8,"second command resolves new source physical page7 through actual logical walker");
 check(VidMmTranslatePaging(base+4096,0x30000,&pa,&system) && pa==base+7*4096,"logical hierarchy reflects accepted remap before execution");
 check(PagingPtShadowRead(&g_VidMm.Shadow,base+5*4096,1,&pa)==PAGING_PT_OK && pa==expected,"second accepted copy metadata comes from newly mapped source page");
 check(VidMmTranslateRetained(base+4096,0x30000,&pa,&system) && pa==base+6*4096 && cpu_storage[5*512+1]==0,"live GPU tables unchanged by command construction");
 // Independent data execution model: decode the two COPY_LINEAR addresses per
 // transaction, using staging as a separate host allocation. Not a GPU emulator.
 for(i=0;i<2;i++) {
  const u32*packet=dma+i*34;u64 source=((u64)packet[4]<<32)|packet[3];
  u64 destination=((u64)packet[23]<<32)|packet[22];unsigned bytes=packet[1]+1;
  check(bytes==8 && source>=d.VramMcBase && source-d.VramMcBase<=sizeof(cpu_storage)-bytes && destination>=d.VramMcBase && destination-d.VramMcBase<=sizeof(cpu_storage)-bytes,"interpreted copy addresses within host VRAM model");
  if(bytes==8 && source>=d.VramMcBase && source-d.VramMcBase<=sizeof(cpu_storage)-bytes && destination>=d.VramMcBase && destination-d.VramMcBase<=sizeof(cpu_storage)-bytes) {
   memcpy(staging,(unsigned char*)cpu_storage+(size_t)(source-d.VramMcBase),bytes);
   memcpy((unsigned char*)cpu_storage+(size_t)(destination-d.VramMcBase),staging,bytes);
  }
 }
 check(VidMmTranslateRetained(base+4096,0x30000,&pa,&system) && pa==base+7*4096 && cpu_storage[5*512+1]==expected,"modeled execution makes live tables agree with accepted construction state");
 VidMmStop();use_retained_walk=0;
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"real-copy integration balances mapping and locks");
}

static void case_separate_table_extent(void)
{
 BC250_DEVICE d={0};DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 D3DGPU_PHYSICAL_ADDRESS address={0};u64 physical;BOOLEAN system;unsigned i;int calls;
 const u64 base=0x200000000ull,table=base+0x100000;
 d.FullWddm=d.VramEnabled=d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;d.VramLength=0x200000;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStartLayout(&d,0,0x100000,1,0x100000,sizeof(cpu_storage),3)==STATUS_SUCCESS,"disjoint table layout starts");
 check(last_map_physical==table && last_map_bytes==sizeof(cpu_storage) && g_VidMm.SegmentLength==sizeof(cpu_storage),"retained CPU mapping covers only table storage");
 memset(cpu_storage,0,sizeof(cpu_storage));u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  pte.Flags=BC250_DXGK_PTE_VALID | ((u64)(i<4?3:1)<<BC250_DXGK_PTE_SEGMENT_SHIFT);pte.PageAddress=i<4?i+1:7;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"separate hierarchy CPU initialization");
 }
 check(VidMmTranslateRetainedPaging(table+4096,0,&physical,&system) && physical==base+7*4096 && !system,"logical walker traverses table segment to application leaf");
 check(VidMmTranslateRetained(table+4096,0,&physical,&system) && physical==base+7*4096,"live walker traverses retained table map to application leaf");
 address.SegmentId=1;address.SegmentOffset=4096;
 check(!VidMmRootPhysical(&address,&physical),"application segment cannot be table root");
 address.SegmentId=3;check(VidMmRootPhysical(&address,&physical) && physical==table+4096,"table segment root uses independent origin");
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical=address;u.PageTableLevel=0;
 {u64 entry;check(VidMmEncodePageTable(&u,0,1,&physical,&entry) && physical==table+4096,"queued table destination uses table origin");}
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_storage+4*512;u.StartIndex=16;
 pte.Flags=BC250_DXGK_PTE_VALID | (3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);pte.PageAddress=4;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && VidMmTranslateRetainedPaging(table+4096,0x10000,&physical,&system) && physical==table+4*4096,"paging leaf can map table storage as data");

 {u64 before=cpu_storage[4*512+16];unsigned used=g_VidMm.Shadow.Used;
  override_cpu_physical=base+4096;
  check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_storage[4*512+16]==before && g_VidMm.Shadow.Used==used,"CPU borrowed application page refused before write/registration");
  override_cpu_physical=table+sizeof(cpu_storage);
  check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_storage[4*512+16]==before && g_VidMm.Shadow.Used==used,"CPU borrowed page at table end refused before write/registration");
  override_cpu_physical=table+1;
  check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_storage[4*512+16]==before,"unaligned physical table identity refused");
  override_cpu_physical=0;
 }
 VidMmStop();check(!cpu_map_live && !shadow_pool_live,"separate table stop releases retained resources");
 calls=cpu_map_calls;
 check(VidMmStartLayout(&d,0,0x100000,1,0x80000,4096,3)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls && !shadow_pool_live,"overlap refused before reservation");
 check(VidMmStartLayout(&d,0,0x100000,1,0x100000,4096,1)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls,"same ID cannot describe distinct ranges");
 check(VidMmStartLayout(&d,0,0x100000,1,0x200000,4096,3)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls,"table extent outside VRAM refused");
 check(!cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"separate table checks balance locks");
}

static void case_wddm_memory_layout(void)
{
 BC250_DEVICE d={0};u64 offset,length,tableOffset,tableLength;unsigned gib;
 d.VramEnabled=1;d.Post.Pitch=7680;d.Post.Height=1200;layout_fb_offset=0;layout_fb_known=1;
 for(gib=1;gib<=32;gib++) {
  u64 available;
  d.VramLength=(u64)gib<<30;
  check(WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"supported model VRAM yields table layout");
  check(offset>=7680ull*1200 && offset-7680ull*1200<65536,"whole framebuffer excluded with64KiB alignment");
  check(!(offset&65535) && !(length&65535) && !(tableOffset&65535) && !(tableLength&65535),"all advertised segment extents64KiB aligned");
  check(tableOffset==offset+length && tableOffset+tableLength==d.VramLength-BC250_VRAM_TOP_RESERVED,"application/table ranges disjoint and end before reserved tail");
  available=d.VramLength-BC250_VRAM_TOP_RESERVED-offset;
  check(tableLength==((available/32)&~65535ull) && length+tableLength==available,"table capacity follows explicit1/32 budget without hidden overlap");
 }
 d.VramEnabled=0;check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength) && !offset && !length && !tableOffset && !tableLength,"closed gate publishes no layout");
 d.VramEnabled=1;d.VramLength=BC250_VRAM_TOP_RESERVED;check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"no usable bytes refuses layout");
 d.VramLength=BC250_VRAM_TOP_RESERVED+16*1024*1024;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength) && tableLength==4*1024*1024 && length==12*1024*1024,"small layout uses explicit4MiB table minimum");
 d.VramLength=BC250_VRAM_TOP_RESERVED+4*1024*1024;
 check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"table minimum cannot consume application segment entirely");
 layout_fb_known=1;layout_fb_offset=~0ull-8;d.VramLength=8ull<<30;
 check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"foreign framebuffer extent rejected without wrap");
 layout_fb_offset=0;
}

static void case_wddm_segment_queries(void)
{
 BC250_DEVICE d={0};DXGKARG_QUERYADAPTERINFO q={0};DXGK_QUERYSEGMENTOUT4 out={0};
 DXGK_QUERYPAGETABLELEVELDESCIN in={0};DXGK_PAGE_TABLE_LEVEL_DESC level;
 enum {stride=sizeof(DXGK_SEGMENTDESCRIPTOR4)+16};u64 storage[(stride*3+7)/8];
 DXGK_SEGMENTDESCRIPTOR4 *app,*aperture,*tables;u64 offset,length,tableOffset,tableLength;unsigned i,j;
 d.VramEnabled=1;d.VramLength=8ull<<30;d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.Post.Pitch=7680;d.Post.Height=1200;
 layout_fb_known=1;layout_fb_offset=0;q.OutputDataSize=sizeof(out);q.pOutputData=&out;out.PagingBufferSize=0xABCD;
 check(WddmQuerySegment4(&d,&q)==STATUS_DEVICE_NOT_READY && !g_ApertureOffered && !out.NbSegment,
       "segment query cannot advertise unprepared aperture geometry");
 {
  struct amdgpu_bo bo={0};
  int calls=gart_capture_calls;
  g_adev.gmc.gart_start=0x300000000ull;g_adev.gmc.gart_size=512ull<<20;
  bo.gpu_addr=0x400000000ull;g_adev.gart.bo=&bo;g_adev.gart.table_size=1ull<<20;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_SUCCESS &&
        d.WddmAperture.mc==0x304002000ull && d.WddmAperture.table==0x400020010ull &&
        gart_capture_calls==calls+1 && !gart_capture_lock && !gart_capture_unlocked,
        "actual capture copies nonzero GART geometry under owner lock without requiring enabled hardware");
  gart_capture_status=STATUS_DEVICE_NOT_READY;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_DEVICE_NOT_READY && !d.WddmAperture.bytes &&
        !d.WddmAperture.mc && !d.WddmAperture.table && !gart_capture_lock,
        "failed GART setup clears cached aperture and balances lock");
  gart_capture_status=STATUS_SUCCESS;g_adev.gart.bo=NULL;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_DEVICE_NOT_READY && !d.WddmAperture.bytes &&
        !gart_capture_lock,"missing GART table refuses capture");
  g_adev.gart.bo=&bo;g_adev.gart.table_size=8;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_DEVICE_NOT_READY && !d.WddmAperture.bytes,
        "short actual GART table cannot advertise OS aperture");
  g_adev.gart.table_size=1ull<<20;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_SUCCESS,"capture restored for descriptor controls");
  g_adev.gart.bo=NULL;
 }
 check(WddmQuerySegment4(&d,&q)==STATUS_SUCCESS && out.NbSegment==3 && out.PagingBufferSize==0xABCD,"count-only query advertises three segments without touching unrelated fields");
 memset(storage,0xCC,sizeof(storage));out.SegmentDescriptorStride=stride;out.pSegmentDescriptor=storage;
 check(WddmQuerySegment4(&d,&q)==STATUS_SUCCESS && g_ApertureOffered,"second query fills three descriptors");
 app=(DXGK_SEGMENTDESCRIPTOR4*)storage;aperture=(DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)storage+stride);tables=(DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)storage+2*stride);
 check(WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"query uses valid shared layout");
 check((u64)app->CpuTranslatedAddress.QuadPart==0x200000000ull+offset && (u64)app->BaseAddress.QuadPart==d.VramMcBase+offset && app->Size==length,"application descriptor addresses match shared layout");
 check((u64)tables->CpuTranslatedAddress.QuadPart==0x200000000ull+tableOffset && tables->Size==tableLength && !tables->Flags.DirectFlip && !tables->Flags.Aperture && tables->Flags.CpuVisible,"table descriptor owns separate local non-scanout extent");
 check((u64)aperture->BaseAddress.QuadPart==0x304002000ull &&
       aperture->CommitLimit==d.WddmAperture.bytes,
       "advertised OS aperture uses captured nonzero GART base plus reserved offset");
 check(aperture->Flags.Aperture && aperture->Size==BC250_WDDM_APERTURE_BYTES && out.PagingBufferSegmentId==0,"aperture ID2 and OS paging-buffer placement preserved");
 for(i=0;i<3;i++)for(j=sizeof(DXGK_SEGMENTDESCRIPTOR4);j<stride;j++)check(((UCHAR*)storage)[i*stride+j]==0xCC,"descriptor stride padding untouched");
 q.pInputData=&in;q.InputDataSize=sizeof(in);q.pOutputData=&level;q.OutputDataSize=sizeof(level);
 for(i=0;i<4;i++){in.LevelIndex=i;check(WddmPageTableLevelDesc(&d,&q)==STATUS_SUCCESS && level.PageTableSegmentId==3 && level.PagingProcessPageTableSegmentId==3 && level.PageTableSizeInBytes==4096,"all hierarchy levels use table segment3");}
 q.pOutputData=&out;q.OutputDataSize=sizeof(out);out.NbSegment=2;
 check(WddmQuerySegment4(&d,&q)==STATUS_INVALID_PARAMETER,"short descriptor count refused");
 out.NbSegment=3;out.SegmentDescriptorStride=sizeof(DXGK_SEGMENTDESCRIPTOR4)-1;
 check(WddmQuerySegment4(&d,&q)==STATUS_INVALID_PARAMETER,"short descriptor stride refused");
 d.VramEnabled=0;out.NbSegment=0;
 check(WddmQuerySegment4(&d,&q)==STATUS_SUCCESS && !out.NbSegment && !g_ApertureOffered,"closed VRAM gate advertises no segments");
}

static void case_probe_physical_copy(void)
{
 BC250_DEVICE d={0};DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 ULONG words[BC250_IB_PROBE_DWORDS];u64 leaf,physical;BOOLEAN system;unsigned i;int maps,copies;
 const u64 base=0x200000000ull;
 d.FullWddm=d.VramEnabled=d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;d.VramLength=sizeof(cpu_storage);cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"probe initialization");
 memset(cpu_storage,0,sizeof(cpu_storage));g_VidMm.Pte.system_limit=0x1000000;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  pte.Flags=BC250_DXGK_PTE_VALID | ((u64)(i<4?1:0)<<BC250_DXGK_PTE_SEGMENT_SHIFT);pte.PageAddress=i<4?i+1:0x123;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"probe hierarchy CPU initialized");
 }
 maps=cpu_map_calls;probe_copy_calls=probe_copy_fail=probe_copy_short=0;
 check(VidMmProbeIb(base+4096,16,&leaf,&physical,&system,words) && physical==0x123010 && system && leaf && probe_copy_address==physical && probe_copy_bytes==sizeof(words),"probe copies exact VA offset with OS physical-memory API");
 check(words[0]==0xABC00000u && words[239]==0xABC000EFu && cpu_map_calls==maps,"probe returns payload without extra physical mappings");
 check(VidMmProbeIb(base+4096,4092,&leaf,&physical,&system,words) && probe_copy_bytes==4 && words[0]==0xABC00000u && words[1]==0,"probe at page end copies only one DWORD");
 probe_copy_short=1;
 check(!VidMmProbeIb(base+4096,0,&leaf,&physical,&system,words) && !leaf && !physical && !system && words[0]==0 && words[239]==0,"short copy clears all partially returned data");
 probe_copy_short=0;probe_copy_fail=1;
 check(!VidMmProbeIb(base+4096,0,&leaf,&physical,&system,words) && words[0]==0 && !physical,"failed OS copy cannot masquerade as valid zero snapshot");
 probe_copy_fail=0;copies=probe_copy_calls;
 check(!VidMmProbeIb(base+4096,1,&leaf,&physical,&system,words) && probe_copy_calls==copies,"unaligned diagnostic address refuses before OS copy");
 VidMmStop();check(!VidMmProbeIb(base+4096,0,&leaf,&physical,&system,words) && probe_copy_calls==copies,"post-stop probe refuses before dereference/copy");
 check(!cpu_map_live && !shadow_pool_live && !flush_lock_depth && !flush_region_depth,"probe and stop release mapping and locks");
}

static void case_mdl_address(void)
{
 struct {MDL mdl;PFN_NUMBER pages[4];} m={{4*4096,0},{0x123,0x789,0x456,0xABC}};
 u64 address;unsigned page;
 for(page=0;page<4;page++) {
  check(GfxPagingMdlAddress(&m.mdl,0,(u64)page*4096,4096,&address) && address==(m.pages[page]<<12),"MDL fragmented page progress uses each PFN");
  check(GfxPagingMdlAddress(&m.mdl,page,4,8,&address) && address==(m.pages[page]<<12)+4,"MDL first-page index distinct from byte offset");
 }
 check(!GfxPagingMdlAddress(&m.mdl,4,0,1,&address) && !address,"MDL first-page outside extent refuses");
 check(!GfxPagingMdlAddress(&m.mdl,0,4095,2,&address),"MDL one-slice resolver refuses page crossing");
 check(!GfxPagingMdlAddress(&m.mdl,1,~0ull,1,&address),"MDL offset addition overflow refuses");
 m.mdl.ByteOffset=13;m.mdl.ByteCount=4096;
 check(!GfxPagingMdlAddress(&m.mdl,0,0,1,&address),"partial MDL first page prefix is not described memory");
 check(GfxPagingMdlAddress(&m.mdl,0,13,4083,&address) && address==(m.pages[0]<<12)+13,"partial MDL first page uses exact described byte interval");
 check(GfxPagingMdlAddress(&m.mdl,1,0,13,&address) && address==(m.pages[1]<<12),"partial MDL last page bounds account for initial byte offset");
 check(!GfxPagingMdlAddress(&m.mdl,1,0,14,&address) && !address,"partial MDL final byte overrun refuses");
 check(!GfxPagingMdlAddress(&m.mdl,1,13,1,&address),"MDL end is exclusive");
 m.pages[0]=1ull<<52;
 check(!GfxPagingMdlAddress(&m.mdl,0,13,1,&address) && !address,"MDL overflowing PFN refuses before address use");
 m.mdl.ByteOffset=4096;
 check(!GfxPagingMdlAddress(&m.mdl,0,4096,1,&address),"malformed MDL byte offset refuses");
 m.mdl.ByteOffset=0;m.mdl.ByteCount=0;
 check(!GfxPagingMdlAddress(&m.mdl,0,0,1,&address),"empty MDL refuses");
 check(!GfxPagingMdlAddress(NULL,0,0,1,&address) && !address,"NULL MDL clears output");
 check(!GfxPagingMdlAddress(&m.mdl,0,0,1,NULL),"NULL output refuses");
}

static void case_physical_stream(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub*hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 struct {MDL mdl;PFN_NUMBER pages[4];} src={{4*4096,0},{0x123,0x789,0x456,0xABC}};
 struct {MDL mdl;PFN_NUMBER pages[4];} dst={{4*4096,0},{0xFED,0x987,0x654,0x321}};
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT source={&src.mdl,0,3*4096,1},destination={&dst.mdl,0,3*4096,0};
 BC250_PAGING_ENDPOINT local={NULL,0x100000200ull,3*4096,0};
 u32 buf[257];static u64 scratch[16];ULONG written;ULONGLONG next,done=0;
 unsigned i,pass=0;NTSTATUS status;
 dev.Gfx=&gfx;dev.VramMcBase=0x100000000ull;dev.VramLength=0x100000;
 gfx.PagingReady=1;gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"physical stream GART window initialized");
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 do {
  memset(buf,0xCC,sizeof(buf));
  status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,128,180*4,done,&written,&next);
  check(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,"physical two-MDL stream accepted");
  check(next>done && next<=3*4096 && written==83*((next-done)/4096),"physical stream exact page progress");
  for(i=0;i<written;i+=83) {
   unsigned page=(unsigned)(done/4096)+i/83;
   u64 spte=((u64)buf[i+5]<<32)|buf[i+4],dpte=((u64)buf[i+7]<<32)|buf[i+6];
   check((spte&AMDGPU_PTE_ADDR_MASK)==(src.pages[page+1]<<12),"source MDL uses its own PFNs and FirstPage");
   check((dpte&AMDGPU_PTE_ADDR_MASK)==(dst.pages[page]<<12),"destination same offset selects different MDL PFNs");
   check(buf[i+36]==(u32)gfx.PagingWindow.mc && buf[i+38]==(u32)(gfx.PagingWindow.mc+4096),"physical copy uses distinct source and destination GART slots");
   check(buf[i+11]==1+3*(32+i),"physical markers include prior DMA command offset");
  }
  check(buf[180]==0xCCCCCCCCu,"physical stream respects command capacity canary");
  done=next;pass++;
 }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<4);
 check(done==3*4096 && pass==2,"physical fragmented transfer completed in two buffers");
 status=GfxPagingBuildPhysical(&dev,&source,&local,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==83 && next==64 && buf[38]==(u32)local.Address,"MDL to local keeps already-MC destination");
 status=GfxPagingBuildPhysical(&dev,&local,&destination,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==83 && buf[36]==(u32)local.Address,"local to MDL keeps already-MC source");
 status=GfxPagingBuildPhysical(&dev,&local,&local,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==7 && next==64,"physical local pair uses direct packet path");
 status=GfxPagingBuildPhysical(&dev,NULL,&destination,TRUE,64,0x87654321,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==81 && buf[36]==0x87654321,"physical MDL fill needs no source and preserves pattern");
 status=GfxPagingBuildPhysical(&dev,NULL,&local,TRUE,64,0x87654321,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==5 && next==64,"physical local fill uses direct packet path");
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,0,82*4,4096,&written,&next);
 check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && next==4096,"physical insufficient capacity preserves input progress");
 // A small buffer must not publish a valid prefix of an invalid total extent.
 memset(buf,0xCC,sizeof(buf));
 source.Length=4*4096;destination.Length=4*4096;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,4*4096,0,buf,0,96*4,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next && all_pattern(buf,257,0xCCCCCCCCu),"whole MDL range accounts for FirstPage before first packet");
 source.Length=3*4096;destination.Length=3*4096;
 dst.mdl.ByteCount=3*4096-1;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,0,96*4,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next && all_pattern(buf,257,0xCCCCCCCCu),"destination short final MDL byte refuses before first packet");
 dst.mdl.ByteCount=3*4096;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,0,96*4,0,&written,&next);
 check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && written==83 && next==4096,"exact MDL end permits bounded first batch");
 dst.mdl.ByteCount=4*4096;
 local.Address=dev.VramMcBase+dev.VramLength-4096;
 memset(buf,0xCC,sizeof(buf));
 status=GfxPagingBuildPhysical(&dev,&source,&local,FALSE,8192,0,buf,0,96*4,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next && all_pattern(buf,257,0xCCCCCCCCu),"whole local range crossing VRAM end refuses before first packet");
 check(GfxPagingEndpointValid(&dev,&local,4096),"exact local VRAM end is valid");
 check(!GfxPagingEndpointValid(&dev,&local,4097),"local VRAM end plus one byte refuses");
 local.Address=dev.VramMcBase-1;
 check(!GfxPagingEndpointValid(&dev,&local,1),"local address below VRAM base refuses");
 local.Address=~0ull-4095;dev.VramMcBase=local.Address;dev.VramLength=8192;
 check(!GfxPagingEndpointValid(&dev,&local,8192),"local interval arithmetic overflow refuses");
 local.Address=PAGING_SYSTEM_ADDRESS-4096;dev.VramMcBase=local.Address;
 check(!GfxPagingEndpointValid(&dev,&local,8192),"local interval cannot cross system marker bit");
 dev.VramMcBase=0x100000000ull;dev.VramLength=0x100000;local.Address=0x100000200ull;
 src.mdl.ByteOffset=13;source.FirstPage=0;
 check(!GfxPagingEndpointValid(&dev,&source,64),"physical endpoint zero progress cannot cover undescribed MDL prefix");
 source.FirstPage=1;
 check(GfxPagingEndpointValid(&dev,&source,3*4096),"MDL first page skips partial prefix while preserving valid tail");
 src.mdl.ByteOffset=0;
 src.pages[2]=1ull<<40;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"unrepresentable second source PFN rolls back tentative first packet");
 src.pages[2]=0x456;
 destination.Length=63;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"endpoint extent preflight refuses overrun");
 destination.Length=3*4096;local.Address=dev.VramMcBase+dev.VramLength-32;
 status=GfxPagingBuildPhysical(&dev,&source,&local,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"local endpoint cannot cross device VRAM end");
 gfx.PagingWindowReady=0;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"physical system transfer requires reserved GART window");
 gfx.PagingReady=0;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"physical builder refuses unready engine");
 check(!flush_region_depth && !flush_lock_depth,"physical builder balances lifetime lock on all paths");
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_physical_fill_ddi(void)
{
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT endpoint;DXGKARG_BUILDPAGINGBUFFER b={0};
 ULONGLONG app,appLength,table,tableLength,moved,total,done,address;
 ULONG token;unsigned mode,passes;u32 dma[32],priv[64];NTSTATUS status;
 dev.Gfx=&gfx;dev.VramMcBase=0x100000000ull;dev.VramLength=16ull<<30;dev.VramEnabled=1;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 layout_fb_known=0;
 check(WddmMemoryLayout(&dev,&app,&appLength,&table,&tableLength),"physical fill obtains advertised layout");
 check(WddmLocalPagingEndpoint(&dev,1,dev.VramMcBase+app,28,64,&endpoint) && endpoint.Address==dev.VramMcBase+app+28,"local endpoint adds byte offset without doubling MC base");
 check(!WddmLocalPagingEndpoint(&dev,1,dev.VramMcBase+table,0,64,&endpoint),"application endpoint cannot name table segment");
 check(!WddmLocalPagingEndpoint(&dev,3,dev.VramMcBase+app,0,64,&endpoint),"table endpoint cannot name application segment");
 check(WddmLocalPagingEndpoint(&dev,3,dev.VramMcBase+table,0,tableLength,&endpoint),"table endpoint accepts exact advertised extent");
 check(!WddmLocalPagingEndpoint(&dev,3,dev.VramMcBase+table,0,tableLength+1,&endpoint),"table endpoint refuses reserved tail");
 check(!WddmLocalPagingEndpoint(&dev,2,0,0,64,&endpoint),"aperture endpoint is not reinterpreted as VRAM");
 check(!WddmLocalPagingEndpoint(&dev,1,dev.VramMcBase+app,~0ull,64,&endpoint),"segment byte offset overflow refuses");
 for(mode=0;mode<2;mode++) {
  total=mode ? (1ull<<32)+8192+36 : 3*4096+36;
  token=mode ? 1u<<20 : 0;done=mode ? (1ull<<32)-28 : 0;passes=0;
  address=dev.VramMcBase+app+28;
  do {
   ULONGLONG expected=4096-((address+done)&4095);
   if(expected>total-done)expected=total-done;
   memset(&b,0,sizeof(b));memset(dma,0xCC,sizeof(dma));memset(priv,0xCC,sizeof(priv));
   b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
   b.DmaBufferWriteOffset=128;b.DmaBufferGpuVirtualAddress=0x700000000ull;b.MultipassOffset=token;
   b.Fill.FillSize=(SIZE_T)total;b.Fill.FillPattern=0xA5C31234;b.Fill.Destination.SegmentId=1;
   b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)address;
   status=WddmBuildPhysicalFill(&dev,&b,&moved);
   check(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,"physical Fill DDI adapter accepts batch");
   check(moved==expected && moved>0 && b.MultipassOffset==token+1,"fill token counts page slices including partial first/last");
   check((((u64)dma[2]<<32)|dma[1])==address+done && dma[3]==0xA5C31234 && dma[4]+1==expected,"published fill packet has independent expected address pattern and byte count");
   check(b.DmaSize==16*4-20 && b.pDmaBuffer==dma+5 && b.DmaBufferWriteOffset==128,"fill publication advances remaining DMA without changing input command offset");
   check(all_pattern(dma+5,27,0xCCCCCCCCu),"fill command tail canary intact");
   check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,20,0,NULL,NULL),"fill private record covers exactly the published command");
   done+=moved;token=b.MultipassOffset;passes++;
   check((status==STATUS_SUCCESS)==(done==total),"fill completion only after exact final byte");
  }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && passes<8);
  check(done==total && passes==(mode?3u:4u),"unaligned and >4GiB resumed fill finish in expected page slices");
 }
 b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.MultipassOffset=0;b.Fill.FillSize=64;b.Fill.Destination.SegmentId=3;
 b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)(dev.VramMcBase+table+64);
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=1;g_VidMm.Write=1;
 g_VidMm.SegmentPhysical=(u64)dev.VramPhysical.QuadPart+table;g_VidMm.SegmentLength=tableLength;
 check(PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4)==PAGING_PT_OK,"table fill shadow initialized");
 {
  u64 old=0x123456789ABCDEF0ull,read;
  check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,8,1,&old,1)==PAGING_PT_OK,"table fill known entry registered");
  b.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES;
  check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved &&
        PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,8,&read)==PAGING_PT_OK && read==old,
        "private refusal leaves logical table fill unpublished");
  b.DmaBufferPrivateDataSize=sizeof(priv);
 }

 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_SUCCESS && moved==64 && b.MultipassOffset==1 &&
       (((u64)dma[2]<<32)|dma[1])==dev.VramMcBase+table+64,"physical table-segment fill preserves full MC destination");
 {
  u64 read;
  check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,8,&read)==PAGING_PT_OK && read==0xA5C31234A5C31234ull,"accepted table fill updates logical known entry before later builds");
  check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,15,&read)==PAGING_PT_OK && read==0xA5C31234A5C31234ull,"full unknown PTE covered by fill becomes known");
 }
 b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.MultipassOffset=0;b.DmaBufferGpuVirtualAddress=~0ull-8;b.Fill.FillPattern=0x11112222;
 memset(dma,0xCC,sizeof(dma));
 {
  u64 read;
  check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset &&
        PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,8,&read)==PAGING_PT_OK && read==0xA5C31234A5C31234ull &&
        all_pattern(dma,32,0xCCCCCCCCu),"private header address refusal leaves table shadow and DMA untouched");
 }
 b.DmaBufferGpuVirtualAddress=0x700000000ull;g_VidMm.Ready=0;
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_DEVICE_NOT_READY && !moved && !b.MultipassOffset && !priv[0] &&
       b.pDmaBuffer==dma && all_pattern(dma,32,0xCCCCCCCCu),"shadow commit refusal invalidates private record without DMA or progress publication");
 memset(&g_VidMm,0,sizeof(g_VidMm));
 b.Fill.Destination.SegmentId=1;
 // Refusals must not advance DMA or the external token.
 b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.MultipassOffset=0;b.Fill.FillSize=8192;b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)(dev.VramMcBase+table-4096);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset && b.pDmaBuffer==dma && all_pattern(dma,32,0xCCCCCCCCu),"whole application fill cannot cross into table segment");
 b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)(dev.VramMcBase+app+28);b.Fill.FillSize=64;
 b.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES;
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset && b.pDmaBuffer==dma,"private capacity refusal retains fill token and DMA pointer");
 b.DmaBufferPrivateDataSize=sizeof(priv);b.MultipassOffset=2;
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_INVALID_PARAMETER && !moved,"fill token beyond final slice refuses");
 b.MultipassOffset=0;b.Fill.FillSize=65;
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_INVALID_PARAMETER && !moved,"non-DWORD fill refuses rather than truncates");
 layout_fb_known=1;
}

static void case_virtual_fill_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[256],priv[300];
 u64 app,len,table,tableLen,moved,read,zero=0,physical;BOOLEAN system;
 ULONG written;NTSTATUS status;unsigned pass;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramLength=16ull<<30;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&app,&len,&table,&tableLen),"virtual fill layout positive control");
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=1;g_VidMm.Write=1;
 g_VidMm.SegmentPhysical=(u64)d.VramPhysical.QuadPart+table;g_VidMm.SegmentLength=tableLen;
 check(PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4)==PAGING_PT_OK,"virtual fill logical storage ready");
 check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,0,1,&zero,1)==PAGING_PT_OK,"virtual fill table identity registered");
 translated=g_VidMm.SegmentPhysical;translation_by_offset=1;isSystem=0;translationOk=1;
 b.FillVirtual.FillSizeInBytes=8184;b.FillVirtual.DestinationVirtualAddress=0x40004;
 b.FillVirtual.FillPattern=0x12345678;b.DmaBufferWriteOffset=128;
 for(pass=0;pass<2;pass++) {
  memset(dma,0xCC,sizeof(dma));b.pDmaBuffer=dma;b.DmaSize=64;
  b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildVirtualFill(&d,4096,&b,&moved);
  check(status==(pass?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && moved==4092,"virtual fill publishes one partial page per small buffer");
  check(b.MultipassOffset==(pass?2u:1u) && b.DmaBufferWriteOffset==128,"virtual fill preserves slice resume and restores command offset");
  check((((u64)dma[2]<<32)|dma[1])==d.VramMcBase+table+(pass?0:4) && dma[3]==0x12345678 && dma[4]==4091,"virtual fill packet uses captured physical table identity and byte count");
  check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,20,0,NULL,NULL),"virtual fill accepted private record covers command");
  check(all_pattern(dma+5,251,0xCCCCCCCCu),"virtual fill leaves command tail untouched");
  check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,0,&read)==PAGING_PT_OK &&
    read==(pass?0x1234567812345678ull:0x1234567800000000ull),"virtual fill updates correct half before subsequent construction");
 }
 b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=128;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildVirtualFill(&d,4096,&b,&moved)==STATUS_SUCCESS && moved==8184 && b.MultipassOffset==2 &&
       b.DmaBufferWriteOffset==128 && b.DmaSize==88,"two virtual page slices share one accumulated DMA buffer");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,40,0,NULL,NULL),"two virtual fill records cover contiguous command offsets");
 b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=64;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.FillVirtual.FillSizeInBytes=4;b.FillVirtual.FillPattern=0x99999999;b.DmaBufferGpuVirtualAddress=~0ull-8;
 check(WddmBuildVirtualFill(&d,4096,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset &&
       PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,0,&read)==PAGING_PT_OK && read==0x1234567812345678ull,"virtual fill publication refusal preserves logical contents and progress");
 b.DmaBufferGpuVirtualAddress=0;translationOk=0;
 check(WddmBuildVirtualFill(&d,4096,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset,"virtual fill translation refusal is not empty success");
 translationOk=1;
 check(GfxPagingBuildFillPage(&d,4096,0x40004,4096,0,dma,0,sizeof(dma),&written,&physical,&system)==STATUS_INVALID_PARAMETER && !written,"fill-page builder rejects crossing a VA page");
 translated=(u64)d.VramPhysical.QuadPart+app;
 b.FillVirtual.DestinationVirtualAddress=0x40004;b.FillVirtual.FillSizeInBytes=(1ull<<32)+8192;
 b.MultipassOffset=1u<<20;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
 b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildVirtualFill(&d,4096,&b,&moved)==STATUS_SUCCESS && moved==8196 &&
       b.MultipassOffset==(1u<<20)+3 && b.pDmaBuffer==dma+15,
       "virtual Fill resumes above4GiB with exact three-slice tail and no length truncation");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,60,0,NULL,NULL),
       "wide virtual Fill publishes all tail records with contiguous offsets");

 check(GfxPagingBuildFillPage(&d,4096,0x40004,4,0,dma,0,sizeof(dma),&written,&physical,&system)==STATUS_SUCCESS &&
       written==5 && physical==translated+4 && !system,"fill-page builder returns exact local CPU physical identity");
 {
  static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
  struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 scratch[16];
  hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
  g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
  gfx.PagingWindowReady=1;
  check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"virtual fill system aperture initialized");
  isSystem=1;translated=0x12345000;
  check(GfxPagingBuildFillPage(&d,4096,0x40004,4,0x11223344,dma,128,sizeof(dma),&written,&physical,&system)==STATUS_SUCCESS &&
        written==81 && physical==0x12345004 && system && dma[36]==0x11223344,"fill-page builder returns exact system identity with GART fill packets");
  memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;isSystem=0;
 }
 translation_by_offset=0;layout_fb_known=1;memset(&g_VidMm,0,sizeof(g_VidMm));
}

static void case_virtual_fill_real_walk(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[64],priv[96];
 u64 app,appLen,table,tableLen,base,root,pa,moved,encoded;BOOLEAN system;
 unsigned i;NTSTATUS status;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramLength=16ull<<30;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&app,&appLen,&table,&tableLen),"fill real-walker advertised split layout");
 cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
 check(VidMmStartLayout(&d,app,appLen,1,table,tableLen,3)==STATUS_SUCCESS,"fill real-walker startup");
 base=(u64)d.VramPhysical.QuadPart+table;root=base+4096;
 memset(cpu_storage,0,sizeof(cpu_storage));
 pte.Flags=BC250_DXGK_PTE_VALID | (3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;pte.PageAddress=i+1;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"fill real-walker directory initialization");
 }
 u.PageTableLevel=0;u.PageTableAddress.CpuVirtual=cpu_storage+4*512;
 u.StartIndex=16;pte.PageAddress=4;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"fill first VA maps its own leaf table");
 u.StartIndex=17;pte.PageAddress=5;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"fill second VA initially maps another page");
 encoded=cpu_storage[4*512+17];use_retained_walk=1;
 check(VidMmTranslatePaging(root,0x10000,&pa,&system) && !system && pa==base+4*4096,"logical first destination positive control");
 check(VidMmTranslatePaging(root,0x11000,&pa,&system) && !system && pa==base+5*4096,"logical second destination positive control");
 check(VidMmTranslateRetained(root,0x11000,&pa,&system) && pa==base+5*4096,"live second destination positive control");
 b.FillVirtual.DestinationVirtualAddress=0x10000;b.FillVirtual.FillSizeInBytes=8192;b.FillVirtual.FillPattern=0;
 b.pDmaBuffer=dma;b.DmaSize=0;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);b.DmaBufferWriteOffset=64;
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildVirtualFill(&d,root,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset,
       "no command capacity prevents self-table fill publication");
 check(VidMmTranslatePaging(root,0x11000,&pa,&system) && pa==base+5*4096 && all_pattern(dma,64,0xCCCCCCCCu),
       "unpublished fill leaves actual logical mapping and DMA untouched");
 b.DmaSize=sizeof(dma);
 status=WddmBuildVirtualFill(&d,root,&b,&moved);
 check(status==STATUS_INVALID_PARAMETER && moved==4096 && b.MultipassOffset==1,
       "accepted self-table fill makes next real translation fail after one-page prefix");
 check(b.pDmaBuffer==dma+5 && b.DmaSize==sizeof(dma)-20 && b.DmaBufferWriteOffset==64,
       "later translation refusal preserves first fill command and restores input offset");
 check((((u64)dma[2]<<32)|dma[1])==d.VramMcBase+table+4*4096 && dma[3]==0 && dma[4]==4095,
       "self-table fill packet retains resolved first physical target");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,20,0,NULL,NULL),
       "accepted self-table fill private record covers prefix exactly");
 check(!VidMmTranslatePaging(root,0x11000,&pa,&system),"actual logical walker sees accepted PTE invalidation");
 check(VidMmTranslateRetained(root,0x11000,&pa,&system) && pa==base+5*4096 && cpu_storage[4*512+17]==encoded,
       "construction leaves live table unchanged before GPU execution");
 // Independent host application of the decoded first command, not GPU execution.
 {
  u64 mc=((u64)dma[2]<<32)|dma[1],offset=mc-d.VramMcBase-table;
  unsigned bytes=dma[4]+1;
  check(offset==4*4096 && bytes==4096,"decoded self-table fill bounds fit host memory model");
  if(offset==4*4096 && bytes==4096)memset((unsigned char*)cpu_storage+(size_t)offset,0,bytes);
 }
 check(!VidMmTranslateRetained(root,0x11000,&pa,&system),"live walker observes invalidation after modeled fill execution");
 use_retained_walk=0;layout_fb_known=1;VidMmStop();
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,
       "real-walker fill test drains storage and balances locks");
}

static void case_prepare_transfer(void)
{
 BC250_DEVICE d={0};DXGKARG_BUILDPAGINGBUFFER b={0};BC250_PAGING_ENDPOINT source,destination;
 struct {MDL mdl;PFN_NUMBER pages[4];} m={{4*4096,0},{0x123,0x789,0x456,0xABC}};
 struct {MDL mdl;PFN_NUMBER pages[4];} n={{4*4096,0},{0xFED,0x987,0x654,0x321}};
 u64 app,len,table,tableLen,progress,address;unsigned flag;
 d.VramEnabled=1;d.VramLength=16ull<<30;d.VramMcBase=0x100000000ull;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&app,&len,&table,&tableLen),"Transfer preparation shared segment layout");
 b.Transfer.TransferSize=8192;b.Transfer.TransferOffset=28;b.Transfer.MdlOffset=1;
 b.Transfer.Source.SegmentId=0;b.Transfer.Source.pMdl=&m.mdl;
 b.Transfer.Destination.SegmentId=1;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app+64);
 check(WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && !progress && source.Mdl==&m.mdl &&
       !source.Address && source.FirstPage==1 && destination.Address==d.VramMcBase+app+92 && !destination.Mdl,
       "MDL-to-local preparation separates page offset from segment byte offset");
 check(GfxPagingMdlAddress(source.Mdl,source.FirstPage,0,4,&address) && address==m.pages[1]*4096,
       "TransferOffset is not added to prepared MDL address");
 b.MultipassOffset=1;
 for(flag=0;flag<32;flag++) {
  BOOLEAN ok;b.Transfer.Flags.Value=flag;
  ok=WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress);
  check((flag&3)?!ok && !source.Mdl && !source.Length && !destination.Length && !progress:
        ok && progress==4004,"Transfer flags preserve slice resume and refuse swizzle variants");
 }
 b.Transfer.Flags.Value=0x80000000u;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && !source.Length && !destination.Length && !progress,
       "reserved Transfer flag refuses without prepared output");
 b.Transfer.Flags.Value=28;b.Transfer.Destination.SegmentId=0;b.Transfer.Destination.pMdl=&n.mdl;
 b.Transfer.TransferOffset=0xffffffffu;
 check(WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && progress==4096 &&
       source.Mdl==&m.mdl && destination.Mdl==&n.mdl && source.FirstPage==1 && destination.FirstPage==1,
       "two distinct MDLs ignore segment-only offset and repeated start/end flags");
 b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app+64);b.Transfer.TransferOffset=28;
 check(WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && source.Address==d.VramMcBase+app+92 &&
       destination.Mdl==&n.mdl && progress==4004,"local-to-MDL applies byte offset only to source");
 b.Transfer.Destination.SegmentId=3;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+table+128);
 check(WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && progress==3940 &&
       destination.Address==d.VramMcBase+table+156,"local pair uses independent page phases and already-MC bases");
 b.Transfer.Destination.SegmentId=2;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && !source.Length && !destination.Length,
       "unimplemented aperture endpoint refuses instead of pretending it is VRAM");
 b.Transfer.Destination.SegmentId=0;b.Transfer.Destination.pMdl=&n.mdl;b.Transfer.MdlOffset=3;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress),"MDL whole transfer beyond described tail refuses");
 b.Transfer.MdlOffset=1;b.Transfer.Destination.pMdl=NULL;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress),"NULL system MDL refuses rather than selecting local address zero");
 b.Transfer.Destination.pMdl=&n.mdl;b.MultipassOffset=100;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress),"out-of-range Transfer slice token refuses");
 layout_fb_known=1;
}

static void case_copy_slice_identity(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 scratch[16];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT src={0},dst={0};BC250_PAGING_COPY_SLICE slice;
 struct {MDL mdl;PFN_NUMBER pages[3];} m={{3*4096,0},{0x123,0x987,0x456}};
 struct {MDL mdl;PFN_NUMBER pages[3];} n={{3*4096,0},{0xABC,0xDEF,0x321}};
 u32 buffer[256];ULONG written;NTSTATUS status;
 d.Gfx=&gfx;d.VramMcBase=0x100000000ull;d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=1ull<<20;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;gfx.PagingWindowReady=1;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"copy slice aperture initialized");
 src.Address=d.VramMcBase+1;src.Length=4096;dst.Address=d.VramMcBase+3;dst.Length=4096;
 status=GfxPagingBuildCopyPage(&d,&src,&dst,0,17,buffer,128,sizeof(buffer),&written,&slice);
 check(status==STATUS_SUCCESS && written==34 && slice.Bytes==17 && !slice.SourceSystem && !slice.DestinationSystem &&
       slice.SourcePhysical==0x200000001ull && slice.DestinationPhysical==0x200000003ull,"local overlap slice returns identities used by staged packet");
 check(buffer[3]==1 && buffer[5]==0x80000 && buffer[20]==0x80000 && buffer[22]==3 && buffer[10]==97 && buffer[27]==98,
       "copy slice uses reserved stage and command-position markers");
 memset(buffer,0xCC,sizeof(buffer));
 check(GfxPagingBuildCopyPage(&d,&src,&dst,0,17,buffer,128,47*4,&written,&slice)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
       !written && !slice.Bytes && !slice.SourcePhysical && !slice.DestinationPhysical && all_pattern(buffer,256,0xCCCCCCCCu),"short staged slice clears identities and emits nothing");
 gfx.PagingCopyStaging.size=0;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,0,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER && !slice.Bytes,"missing stage refuses overlap");
 dst.Address=d.VramMcBase+8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==7 &&
       slice.SourcePhysical==0x200000005ull && slice.DestinationPhysical==0x200002004ull,"disjoint local copy keeps direct path and progress-adjusted identities");
 src.Mdl=&m.mdl;src.FirstPage=1;src.Address=0;src.Length=8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==83 &&
       slice.SourceSystem && !slice.DestinationSystem && slice.SourcePhysical==0x987004 && slice.DestinationPhysical==0x200002004ull,
       "MDL source identity reflects FirstPage plus byte progress");
 dst.Mdl=&n.mdl;dst.FirstPage=0;dst.Address=0;dst.Length=8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4096,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==83 &&
       slice.SourceSystem && slice.DestinationSystem && slice.SourcePhysical==0x456000 && slice.DestinationPhysical==0xDEF000,
       "two fragmented MDLs return distinct captured PFNs at next page");
 src.Mdl=NULL;src.Address=d.VramMcBase+1;src.Length=8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==83 &&
       !slice.SourceSystem && slice.DestinationSystem && slice.SourcePhysical==0x200000005ull && slice.DestinationPhysical==0xABC004,
       "local-to-MDL slice returns direct source and system destination identities");
 src.Mdl=&m.mdl;src.Address=0;
 n.pages[1]=m.pages[2];gfx.PagingCopyStaging.size=4096;memset(buffer,0xCC,sizeof(buffer));
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4096,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==100 && slice.SourcePhysical==slice.DestinationPhysical &&
       slice.SourceSystem && slice.DestinationSystem && buffer[38]==0x80000 && buffer[53]==0x80000,
       "physical system alias stages through private VRAM while GART mappings stay active");
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4095,2,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER && !written,"copy slice cannot cross either page boundary");
 check(GfxPagingBuildCopyPage(&d,&src,&dst,~0ull,2,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER && !written,"copy slice byte-progress overflow refuses");
 translationOk=1;translation_by_offset=1;translated=0x200000000ull;isSystem=0;fragmented=0;
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50003,17,FALSE,buffer,128,sizeof(buffer),&written,&slice)==STATUS_SUCCESS &&
       written==34 && slice.SourcePhysical==0x200000001ull && slice.DestinationPhysical==0x200000003ull &&
       !slice.SourceSystem && !slice.DestinationSystem && slice.Bytes==17,
       "different virtual pages sharing local physical page use staged captured identities");
 check(buffer[3]==1 && buffer[22]==3,"virtual local alias packet agrees with returned identity");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50080,17,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS &&
       written==7 && slice.DestinationPhysical==0x200000080ull && buffer[5]==0x80,
       "virtual disjoint local ranges retain direct copy");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50080,17,TRUE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS &&
       written==34,"virtual caller can force snapshot for whole-transfer ordering");
 isSystem=1;translated=0x123000;
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50003,17,FALSE,buffer,128,sizeof(buffer),&written,&slice)==STATUS_SUCCESS &&
       written==100 && slice.SourceSystem && slice.DestinationSystem &&
       slice.SourcePhysical==0x123001 && slice.DestinationPhysical==0x123003,
       "virtual system alias captures PFNs while mapped staging preserves source");
 memset(buffer,0xCC,sizeof(buffer));
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50003,17,FALSE,buffer,0,111*4,&written,&slice)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
       !written && !slice.Bytes && !slice.SourcePhysical && all_pattern(buffer,256,0xCCCCCCCCu),
       "virtual mapped staging refusal publishes no identity or payload");
 check(GfxPagingBuildVirtualCopyPage(&d,0,0x40001,0x50003,17,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual copy requires paging root");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40FFF,0x50003,2,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual copy bounds source page");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50FFF,2,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual copy bounds destination page");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,~0ull,0x50003,1,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual copy rejects address wrap");
 translationOk=0;
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50003,17,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual unresolved copy cannot report stale identities");
 translationOk=1;translation_by_offset=0;isSystem=0;translated=0;
 check(!flush_lock_depth && !flush_region_depth,"copy slice lifetime lock balanced on success and refusal");
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_transfer_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[256],priv[300];static u64 scratch[16];
 static unsigned char memory[32768],expected[32768];
 u64 app,len,table,tableLen,moved,total=0,value=0x123456789ABCDEF0ull,zero=0,read;
 unsigned pass=0,i;NTSTATUS status;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramLength=16ull<<30;d.VramMcBase=0x100000000ull;d.VramPhysical.QuadPart=0x200000000ll;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+24576;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;layout_fb_known=0;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(WddmMemoryLayout(&d,&app,&len,&table,&tableLen),"Transfer publication advertised layout");
 b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app+1);
 b.Transfer.Destination.SegmentId=1;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app+4097);
 b.Transfer.TransferSize=8192;b.DmaBufferWriteOffset=64;
 for(i=0;i<sizeof(memory);i++)memory[i]=(unsigned char)(i*13+i/129);
 memcpy(expected,memory,sizeof(memory));memmove(expected+4097,expected+1,8192);
 do {
  b.pDmaBuffer=dma;b.DmaSize=48*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildPhysicalTransfer(&d,&b,&moved);
  check(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,"physical Transfer accepts staged overlap prefix");
  check(moved==(pass==0?1u:pass==1?4096u:4095u) && b.MultipassOffset==pass+1 && b.DmaBufferWriteOffset==64,
        "physical overlap resumes in reverse slice order with exact progress");
  check(b.pDmaBuffer==dma+34 && dma[3]==(pass==0?8192u:pass==1?4096u:1u) &&
        dma[22]==dma[3]+4096 && dma[5]==24576 && dma[20]==24576,"reverse Transfer stages even individually disjoint slices");
  check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,34*4,0,NULL,NULL),
        "Transfer private record exactly covers accepted command prefix");
  if(dma[3]<16384 && dma[22]<16384 && dma[1]<4096 && dma[18]<4096) {
   memcpy(memory+dma[5],memory+dma[3],dma[1]+1);
   memcpy(memory+dma[22],memory+dma[20],dma[18]+1);
  }
  total+=moved;pass++;
 }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<5);
 check(total==8192 && pass==3 && memcmp(memory,expected,16384)==0,"decoded reverse multi-buffer Transfer matches whole-range memmove");
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=1;g_VidMm.Write=1;
 g_VidMm.SegmentPhysical=(u64)d.VramPhysical.QuadPart+table;g_VidMm.SegmentLength=tableLen;
 check(PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4)==PAGING_PT_OK,"Transfer table shadow ready");
 check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,0,1,&value,1)==PAGING_PT_OK,"Transfer source metadata registered");
 check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,0,1,&zero,1)==PAGING_PT_OK,"Transfer destination metadata registered");
 memset(&b,0,sizeof(b));b.Transfer.Source.SegmentId=3;b.Transfer.Destination.SegmentId=3;
 b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+table);
 b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+table+4096);
 b.Transfer.TransferOffset=1;b.Transfer.TransferSize=7;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES;
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset &&
       PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,0,&read)==PAGING_PT_OK && !read,
       "refused Transfer capacity leaves logical destination unchanged");
 b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==7 && b.MultipassOffset==1 &&
       PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,0,&read)==PAGING_PT_OK && read==(value&~255ull),
       "published unaligned table Transfer preserves untouched byte and commits source knowledge");
 b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 g_VidMm.Ready=0;memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_DEVICE_NOT_READY && !moved && !b.MultipassOffset &&
       !priv[0] && b.pDmaBuffer==dma && all_pattern(dma,256,0xCCCCCCCCu),
       "Transfer shadow commit refusal invalidates record without DMA publication");
 g_VidMm.Ready=1;
 {
  static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
  struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
  struct {MDL mdl;PFN_NUMBER pages[3];} mdl={{3*4096,0},{0x123,0x789,0x456}};
  struct {MDL mdl;PFN_NUMBER pages[3];} other={{3*4096,0},{0xABC,0xDEF,0x321}};
  hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
  gfx.PagingWindowReady=1;
  check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"Transfer publication GART window initialized");
  b.Transfer.Source.SegmentId=0;b.Transfer.Source.pMdl=&mdl.mdl;
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==7 &&
        PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,0,&read)==PAGING_PT_MISSING,
        "MDL-to-table Transfer clears unknown external bytes instead of retaining stale PTE");
  memset(&b,0,sizeof(b));b.Transfer.Source.pMdl=&mdl.mdl;b.Transfer.Destination.SegmentId=1;
  b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app);
  b.Transfer.TransferOffset=28;b.Transfer.TransferSize=8192;pass=0;total=0;
  do {
   b.pDmaBuffer=dma;b.DmaSize=96*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
   status=WddmBuildPhysicalTransfer(&d,&b,&moved);
   check(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,"MDL-to-local Transfer publishes mapped slice");
   check(moved==(pass%2?28u:4068u) && b.MultipassOffset==pass+1 && b.pDmaBuffer==dma+83,
         "mixed Transfer uses both endpoint page boundaries and resumes exactly");
   total+=moved;pass++;
  }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<6);
  check(total==8192 && pass==4,"fragmented MDL-to-local transfer covers all bytes in four batches");
  memset(&b,0,sizeof(b));b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app);
  b.Transfer.Destination.pMdl=&mdl.mdl;b.Transfer.TransferOffset=28;b.Transfer.TransferSize=17;
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==17 && b.MultipassOffset==1 &&
        dma[36]==(u32)(d.VramMcBase+app+28) && dma[38]==(u32)(gfx.PagingWindow.mc+4096),
        "local-to-MDL Transfer publishes exact local source and mapped destination");
  b.MultipassOffset=0;b.Transfer.Source.SegmentId=0;b.Transfer.Source.pMdl=&other.mdl;
  b.Transfer.TransferSize=8192;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  memset(dma,0xCC,sizeof(dma));
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192 && b.MultipassOffset==2,
        "disjoint fragmented MDLs publish complete multi-page transfer");
  other.pages[0]=mdl.pages[1];other.pages[1]=mdl.pages[0];
  b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  memset(dma,0xCC,sizeof(dma));
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset && all_pattern(dma,256,0xCCCCCCCCu),
        "swapped physical-page cycle requests a larger buffer without emitting a prefix");
  memset(hub,0,sizeof(*hub));
 }
 memset(&g_VidMm,0,sizeof(g_VidMm));layout_fb_known=1;g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_virtual_transfer_real_walk(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0},zeros[512]={0};
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[128],priv[180];static u64 scratch[16];
 u64 app,appLen,table,tableLen,base,root,pa,moved,encoded;BOOLEAN system;
 unsigned i;NTSTATUS status;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramLength=16ull<<30;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&app,&appLen,&table,&tableLen),"Transfer real-walker advertised layout");
 cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
 check(VidMmStartLayout(&d,app,appLen,1,table,tableLen,3)==STATUS_SUCCESS,"Transfer real-walker startup");
 base=(u64)d.VramPhysical.QuadPart+table;root=base+4096;
 memset(cpu_storage,0,sizeof(cpu_storage));
 pte.Flags=BC250_DXGK_PTE_VALID | (3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;pte.PageAddress=i+1;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer directory initialization");
 }
 u.PageTableLevel=0;u.PageTableAddress.CpuVirtual=cpu_storage+4*512;
 u.StartIndex=16;pte.PageAddress=4;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer first destination maps leaf table");
 u.StartIndex=17;pte.PageAddress=5;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer second destination maps data page");
 u.StartIndex=32;pte.PageAddress=6;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer source maps known zero page");
 u.StartIndex=33;pte.PageAddress=6;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer next source maps known zero page");
 u.StartIndex=48;pte.PageAddress=5;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer multipass first destination");
 u.StartIndex=49;pte.PageAddress=7;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer multipass second destination");
 u.StartIndex=0;u.NumPageTableEntries=512;u.pPageTableEntries=zeros;
 for(i=5;i<=7;i++) {
  u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer data page registered with known bytes");
 }
 encoded=cpu_storage[4*512+17];use_retained_walk=1;
 check(VidMmTranslatePaging(root,0x20000,&pa,&system) && !system && pa==base+6*4096,
       "Transfer source actual walker positive control");
 check(VidMmTranslatePaging(root,0x11000,&pa,&system) && !system && pa==base+5*4096,
       "Transfer destination actual walker positive control");
 b.TransferVirtual.SourceVirtualAddress=0x20000;b.TransferVirtual.DestinationVirtualAddress=0x30000;
 b.TransferVirtual.TransferSizeInBytes=8192;b.DmaBufferWriteOffset=64;
 for(i=0;i<2;i++) {
  b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildVirtualTransfer(&d,root,&b,&moved);
  check(status==(i?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && moved==4096 &&
        b.MultipassOffset==i+1,"virtual Transfer resumes exactly one page per DMA buffer");
  check(b.pDmaBuffer==dma+7 && b.DmaBufferWriteOffset==64 &&
        PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,28,0,NULL,NULL),
        "virtual Transfer private records cover exact published command");
  check((((u64)dma[4]<<32)|dma[3])==d.VramMcBase+table+6*4096 &&
        (((u64)dma[6]<<32)|dma[5])==d.VramMcBase+table+(i?7:5)*4096,
        "virtual Transfer packet follows independent real page translations");
 }
 b.TransferVirtual.DestinationVirtualAddress=0x10000;b.MultipassOffset=0;
 b.pDmaBuffer=dma;b.DmaSize=0;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildVirtualTransfer(&d,root,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset,
       "no capacity prevents virtual self-table copy");
 check(VidMmTranslatePaging(root,0x11000,&pa,&system) && pa==base+5*4096 && all_pattern(dma,128,0xCCCCCCCCu),
       "unpublished copy preserves actual logical mapping and DMA");
 b.DmaSize=sizeof(dma);b.DmaBufferGpuVirtualAddress=~0ull-8;
 check(WddmBuildVirtualTransfer(&d,root,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset &&
       VidMmTranslatePaging(root,0x11000,&pa,&system) && pa==base+5*4096,
       "private-header refusal precedes virtual table commit");
 b.DmaBufferGpuVirtualAddress=0;
 status=WddmBuildVirtualTransfer(&d,root,&b,&moved);
 check(status==STATUS_INVALID_PARAMETER && moved==4096 && b.MultipassOffset==1,
       "accepted virtual self-table copy invalidates next real translation after one prefix");
 check(b.pDmaBuffer==dma+7 && b.DmaSize==sizeof(dma)-28 && b.DmaBufferWriteOffset==64 &&
       PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,28,0,NULL,NULL),
       "later translation refusal preserves published copy and restores input offset");
 check(!VidMmTranslatePaging(root,0x11000,&pa,&system),"logical walker observes accepted copy before next construction");
 check(VidMmTranslateRetained(root,0x11000,&pa,&system) && pa==base+5*4096 && cpu_storage[4*512+17]==encoded,
       "copy construction leaves live table untouched");
 {
  u64 src=(((u64)dma[4]<<32)|dma[3])-d.VramMcBase-table;
  u64 dst=(((u64)dma[6]<<32)|dma[5])-d.VramMcBase-table;
  unsigned bytes=dma[1]+1;
  check(src==6*4096 && dst==4*4096 && bytes==4096,"decoded copy fits independent host data model");
  if(src==6*4096 && dst==4*4096 && bytes==4096)
   memcpy((unsigned char*)cpu_storage+(size_t)dst,(unsigned char*)cpu_storage+(size_t)src,bytes);
 }
 check(!VidMmTranslateRetained(root,0x11000,&pa,&system),"live walker observes copied invalidation after modeled execution");
 use_retained_walk=0;VidMmStop();
 // Resume near the end of a wide request without simulating a4GiB GPU run.
 translated=(u64)d.VramPhysical.QuadPart+app;translation_by_offset=1;translationOk=1;isSystem=0;
 gfx.PagingCopyStaging.mc=d.VramMcBase+app+0x80000;gfx.PagingCopyStaging.size=4096;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 memset(&b,0,sizeof(b));b.TransferVirtual.SourceVirtualAddress=0x20000;
 b.TransferVirtual.DestinationVirtualAddress=0x30000;b.TransferVirtual.TransferSizeInBytes=(1ull<<32)+17;
 b.MultipassOffset=1u<<20;b.DmaBufferWriteOffset=128;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
 b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildVirtualTransfer(&d,4096,&b,&moved)==STATUS_SUCCESS && moved==17 &&
       b.MultipassOffset==(1u<<20)+1 && b.pDmaBuffer==dma+34,
       "virtual Transfer resumes beyond4GiB with exact staged tail");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,136,0,NULL,NULL),
       "wide virtual Transfer tail has valid private coverage");
 translation_by_offset=0;layout_fb_known=1;g_adev.sdma.fence_mem.cpu=NULL;
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,
       "real-walker Transfer test drains storage and balances locks");
}

static void case_aperture_partition(void)
{
 PAGING_WINDOW window;PAGING_APERTURE aperture,invalid;
 unsigned long long mc,table;unsigned i;
 const unsigned long long start=0x300000000ull,root=0x400000000ull,limit=0x1000000000000ull;
 check(PagingWindowInit(start,512ull<<20,root,1ull<<20,&window),"OS partition temporary window positive control");
 check(PagingApertureInit(start,512ull<<20,root,1ull<<20,&aperture),"OS aperture fits existing512MiB GART");
 check(aperture.mc==start+67117056 && aperture.table==root+131088 &&
       aperture.bytes==268435456,"OS aperture independently calculated MC and PTE extent");
 check(aperture.mc>=window.mc+8192 && aperture.table>=window.table+16 &&
       aperture.mc>=start+67108864,"OS partition excludes driver allocations and both temporary slots");
 check(PagingApertureRange(&aperture,0,65536,&mc,&table) &&
       mc==aperture.mc && table==aperture.table,"complete advertised OS range is representable");
 check(PagingApertureRange(&aperture,65535,1,&mc,&table) &&
       mc==start+335548416 && table==root+655368,"last OS page maps final reserved PTE");
 for(i=0;i<65536;i+=997) {
  check(PagingApertureRange(&aperture,i,65536-i,&mc,&table) &&
        mc==start+67117056+(unsigned long long)i*4096 &&
        table==root+131088+(unsigned long long)i*8,
        "OS range coordinates remain independent across aperture");
 }
 check(!PagingApertureRange(&aperture,65535,2,&mc,&table) && !mc && !table,"OS page count cannot overrun final PTE");
 check(!PagingApertureRange(&aperture,65536,1,&mc,&table) && !mc && !table,"OS first page cannot equal exclusive end");
 check(!PagingApertureRange(&aperture,0,0,&mc,&table) && !mc && !table,"empty aperture operation refused");
 check(!PagingApertureRange(&aperture,~0ull,1,&mc,&table) && !mc && !table,"OS page offset wrap refused");
 check(!PagingApertureRange(&aperture,1,~0ull,&mc,&table) && !mc && !table,"OS page count wrap refused");
 check(!PagingApertureRange(&aperture,0,1,&mc,&mc) && !mc,"aliased output locations refused");
 check(!PagingApertureInit(start,335552511,root,1ull<<20,&invalid) &&
       !invalid.mc && !invalid.table && !invalid.bytes,"short GART cannot partially advertise OS extent");
 check(!PagingApertureInit(start,512ull<<20,root,655375,&invalid) &&
       !invalid.bytes,"short PTE table cannot back entire OS extent");
 check(!PagingApertureInit(start+1,512ull<<20,root,1ull<<20,&invalid) && !invalid.bytes,"unaligned GART base refused");
 check(!PagingApertureInit(start,512ull<<20,root+1,1ull<<20,&invalid) && !invalid.bytes,"unaligned PTE base refused");
 check(PagingApertureInit(limit-335552512,335552512,limit-655376,655376,&invalid) &&
       invalid.mc+invalid.bytes==limit &&
       invalid.table+(invalid.bytes/4096)*8==limit,"48bit exclusive ends accepted exactly");
 check(!PagingApertureInit(limit-335552512+4096,335552512,root,1ull<<20,&invalid) &&
       !invalid.bytes,"48bit MC overrun refused");
 check(!PagingApertureInit(start,512ull<<20,limit-655376+8,655376,&invalid) &&
       !invalid.bytes,"48bit PTE overrun refused");
 invalid=aperture;invalid.bytes=0;
 check(!PagingApertureRange(&invalid,0,1,&mc,&table) && !mc && !table,"uninitialized OS extent refused");
}

static void case_aperture_ddi(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};struct amdgpu_bo bo={0};
 struct {MDL mdl;PFN_NUMBER pages[520];} m;
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[2048],priv[2080];static u64 scratch[16];
 static u64 logical[PAGING_APERTURE_BYTES/4096];
 u64 table,mc,value;unsigned i,pass,count,done;NTSTATUS status;
 d.Gfx=&gfx;gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.gmc.gart_start=0x300000000ull;g_adev.gmc.gart_size=512ull<<20;
 bo.gpu_addr=0x400000000ull;g_adev.gart.bo=&bo;g_adev.gart.table_size=1ull<<20;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x500000000ull;
 check(PagingApertureInit(g_adev.gmc.gart_start,g_adev.gmc.gart_size,bo.gpu_addr,g_adev.gart.table_size,&d.WddmAperture),
       "aperture DDI uses captured advertised geometry");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,(unsigned)(PAGING_APERTURE_BYTES/4096)),"logical aperture fixture");
 g_VidMm.Ready=TRUE;g_VidMm.Write=TRUE;b.Operation=DXGK_OPERATION_MAP_APERTURE_SEGMENT;
 // Failed private-header publication must not expose a new planned mapping.
 m.pages[1]=0x12311;
 m.mdl.ByteOffset=7;m.mdl.ByteCount=520*4096-14;
 for(i=0;i<520;i++)m.pages[i]=0x12300+i*17;
 m.pages[0]=~0ull; // skipped MDL page is deliberately unrepresentable
 b.MapApertureSegment.SegmentId=2;b.MapApertureSegment.OffsetInPages=13;
 b.MapApertureSegment.NumberOfPages=12;b.MapApertureSegment.pMdl=&m.mdl;
 b.MapApertureSegment.MdlOffset=1;b.MapApertureSegment.Flags.CacheCoherent=1;b.DmaBufferWriteOffset=64;
 for(pass=0;pass<2;pass++) {
  done=b.MultipassOffset;count=pass?3:9;
  memset(dma,0xCC,sizeof(dma));b.pDmaBuffer=dma;b.DmaSize=48*4;
  b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildAperture(&d,&b,FALSE);
  check(status==(pass?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) &&
        b.MultipassOffset==done+count,"map aperture resumes by accepted PTE count");
  check(PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc+(13+done)*4096,4096,&value) && value==m.pages[1+done]*4096,"accepted map publishes actual physical page identity");
  check(PagingApertureRange(&d.WddmAperture,13+done,count,&mc,&table) &&
        (((u64)dma[2]<<32)|dma[1])==table && dma[3]==2*count-1,
        "map packet addresses exact advertised PTE subrange");
  for(i=0;i<count;i++) {
   value=((u64)dma[5+2*i]<<32)|dma[4+2*i];
   check((value&AMDGPU_PTE_ADDR_MASK)==m.pages[1+done+i]*4096 &&
         (value&AMDGPU_PTE_SNOOPED) && (value&AMDGPU_PTE_SYSTEM) && (value&AMDGPU_PTE_VALID),
         "map preserves fragmented MDL PFNs, MdlOffset and coherent flags");
  }
  check(dma[4+2*count+3]==49 && dma[14+2*count+1]==TEST_REQ_ID &&
        dma[14+2*count+12]==1,"map orders fresh marker then GART invalidate");
  check(b.DmaBufferWriteOffset==64 && b.pDmaBuffer==dma+29+2*count &&
        PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,(29+2*count)*4,0,NULL,NULL),
        "map publishes matching DMA/private records without changing input command offset");
 }
 b.MapApertureSegment.Flags.CacheCoherent=0;b.MapApertureSegment.NumberOfPages=1;b.MultipassOffset=0;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_SUCCESS && !(dma[4]&AMDGPU_PTE_SNOOPED),
       "noncoherent map clears snoop while retaining explicit PFN");
 b.MapApertureSegment.NumberOfPages=519;b.MultipassOffset=0;
 for(pass=0;pass<3;pass++) {
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  count=pass==2?7:256;
  check(WddmBuildAperture(&d,&b,FALSE)==(pass==2?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) &&
        b.MultipassOffset==(pass==2?519:(pass+1)*256) && b.pDmaBuffer==dma+29+2*count,
        "large MDL map uses bounded stack batches and includes partial final MDL page");
 }
 b.MapApertureSegment.NumberOfPages=1;b.MultipassOffset=0;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));b.DmaBufferGpuVirtualAddress=~0ull-8;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset &&
       b.pDmaBuffer==dma && all_pattern(dma,2048,0xCCCCCCCCu),"aperture header refusal cannot advance mapping progress");
 b.DmaBufferGpuVirtualAddress=0;b.DmaSize=31*4;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !b.MultipassOffset &&
       all_pattern(dma,2048,0xCCCCCCCCu),"aperture insufficient complete reservation writes no DMA");
 b.DmaSize=sizeof(dma);b.MapApertureSegment.Flags.Value=2;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"reserved map flag refused");
 b.MapApertureSegment.Flags.Value=0;b.MapApertureSegment.NumberOfPages=520;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"map preflights whole MDL page span");
 b.MapApertureSegment.NumberOfPages=1;b.MapApertureSegment.OffsetInPages=65536;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"map cannot escape advertised aperture");
 b.MapApertureSegment.OffsetInPages=13;m.pages[1]=~0ull;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset &&
       all_pattern(dma,2048,0xCCCCCCCCu),"unrepresentable PFN cannot become a truncated successful mapping");
 {
  ULONGLONG before=0,after=0;
  b.Operation=DXGK_OPERATION_MAP_APERTURE_SEGMENT;
  b.MapApertureSegment.OffsetInPages=13;b.MapApertureSegment.MdlOffset=1;
  b.MapApertureSegment.NumberOfPages=12;b.MapApertureSegment.pMdl=&m.mdl;
  check(PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc+13*4096,4,&before),"old mapping exists before publication refusal");
  m.pages[1]=0x77777;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
  b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=0;
  check(WddmPublishPagingRecord(&b,8,FALSE,0,1)==STATUS_INVALID_PARAMETER &&
        PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc+13*4096,4,&after) && before==after,
        "private header refusal leaves aperture mapping unchanged");
  b.DmaBufferPrivateDataSize=sizeof(priv);
 }
 m.pages[1]=0x12311;d.WddmAperture.table+=8;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"live geometry mismatch prevents writes to stale PTE range");
 d.WddmAperture.table-=8;
 memset(&b,0,sizeof(b));b.Operation=DXGK_OPERATION_UNMAP_APERTURE_SEGMENT;b.UnmapApertureSegment.SegmentId=2;b.UnmapApertureSegment.OffsetInPages=65534;
 b.UnmapApertureSegment.NumberOfPages=2;b.UnmapApertureSegment.DummyPage.QuadPart=0x98765000;
 b.DmaBufferWriteOffset=128;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
 b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 { u64 mapped[2]={0x11111000,0x22222000};
   check(PagingApertureStateMap(&g_VidMm.Aperture,65534,2,mapped,~4095ull),"seed actual mapping before unmap"); }
 check(WddmBuildAperture(&d,&b,TRUE)==STATUS_SUCCESS && b.MultipassOffset==2 &&
       b.pDmaBuffer==dma+33 && (dma[4]&AMDGPU_PTE_VALID) && dma[4]==dma[6] && dma[5]==dma[7] &&
       ((((u64)dma[5]<<32)|dma[4])&AMDGPU_PTE_ADDR_MASK)==0x98765000,
       "unmap final aperture pages repeat supplied valid dummy PTE instead of zero");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,33*4,0,NULL,NULL),
       "unmap publishes ordered packet with exact private coverage");
 b.MultipassOffset=0;b.UnmapApertureSegment.DummyPage.QuadPart++;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildAperture(&d,&b,TRUE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"unaligned dummy physical page refused");
 check(!PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc+65534ull*4096,4,&value),"accepted unmap invalidates logical access despite valid hardware dummy PTE");
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
 check(!flush_lock_depth && !flush_region_depth,"aperture map/unmap engine lifetime locks balance");
 memset(hub,0,sizeof(*hub));g_adev.gart.bo=NULL;g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_aperture_state_lifetime(void)
{
 BC250_DEVICE d={0};int before=shadow_pool_live;u64 physical;
 d.FullWddm=d.VramEnabled=d.VramWriteEnabled=1;
 d.VramPhysical.QuadPart=0x200000000ull;d.VramLength=0x200000;
 cpu_map_fail=0;cpu_write_setting=1;
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"lifetime aperture geometry");
 check(VidMmStartLayout(&d,0,0x100000,1,0x100000,sizeof(cpu_storage),3)==STATUS_SUCCESS,
       "actual VidMm start prepares table and aperture storage");
 check(shadow_pool_live==before+2 && g_VidMm.Aperture.count==65536 &&
       !PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc,4,&physical),
       "new aperture storage is owned and initially unmapped");
 VidMmStop();
 check(shadow_pool_live==before && !g_VidMm.Aperture.entries && !g_VidMm.Ready,
       "actual VidMm stop releases logical storage under its CPU-reader lock");
}

// Decode actual map/copy packets into independent sparse physical backing.
// This checks byte content across DDI resumes, not GPU ordering/cache behavior.
static void case_indirect_multipass_bytes(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 scratch[16],logical[65536];
 static unsigned char memory[6][4096],expected[6][4096];
 const u64 physical[6]={0x100123000ull,0x300789000ull,0x200456000ull,
                        0x400ABC000ull,0x600DEF000ull,0x500321000ull};
 struct {MDL mdl;PFN_NUMBER pages[3];} source={{12288,0},{0}},destination={{12288,0},{0}};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned mode,limit,i,j,pass;u32 dma[1024],priv[1200];
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"byte replay temporary window");
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"byte replay permanent aperture");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,65536),"byte replay logical state");
 g_VidMm.Ready=g_VidMm.Write=TRUE;
 check(PagingApertureStateMap(&g_VidMm.Aperture,0,3,physical,~4095ull) &&
       PagingApertureStateMap(&g_VidMm.Aperture,10,3,physical+3,~4095ull),"byte replay fragmented high physical pages");
 for(i=0;i<3;i++){source.pages[i]=physical[i]>>12;destination.pages[i]=physical[i+3]>>12;}
 for(mode=0;mode<4;mode++)for(limit=0;limit<2;limit++) {
  DXGKARG_BUILDPAGINGBUFFER b={0};u64 moved,total=0;NTSTATUS status;
  unsigned srcStart=(mode&1)?17:0,dstStart=(mode&2)?31:0;
  for(i=0;i<6;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  memcpy(expected,memory,sizeof(memory));
  for(i=0;i<8192;i++)expected[3+(dstStart+i)/4096][(dstStart+i)%4096]=memory[(srcStart+i)/4096][(srcStart+i)%4096];
  if(mode&1){b.Transfer.Source.SegmentId=2;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.WddmAperture.mc+srcStart);}
  else b.Transfer.Source.pMdl=&source.mdl;
  if(mode&2){b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.WddmAperture.mc+10*4096+dstStart);}
  else b.Transfer.Destination.pMdl=&destination.mdl;
  b.Transfer.TransferSize=8192;b.DmaBufferWriteOffset=128;
  pass=0;
  do {
   u64 srcPhys,dstPhys,srcMc,dstMc;unsigned sp=6,dp=6,so,doff,bytes,prior=b.MultipassOffset;
   b.pDmaBuffer=dma;b.pDmaBufferPrivateData=priv;
   b.DmaSize=limit?sizeof(dma):96*4;
   b.DmaBufferPrivateDataSize=limit?PAGING_PRIVATE_HEADER_BYTES+96*4:sizeof(priv);
   memset(dma,0xCC,sizeof(dma));memset(priv,0xCC,sizeof(priv));
   status=WddmBuildPhysicalTransfer(&d,&b,&moved);
   check((status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && moved &&
         b.MultipassOffset==prior+1 && b.DmaBufferWriteOffset==128 && b.pDmaBuffer==dma+83,
         "indirect byte replay accepts exactly one slice with DMA or private capacity limit");
   if(b.pDmaBuffer!=dma+83 || !moved)break;
   check(PagingPrivateVisit(priv,(unsigned)((limit?PAGING_PRIVATE_HEADER_BYTES+96*4:sizeof(priv))-b.DmaBufferPrivateDataSize),128,83*4,0,NULL,NULL),
         "indirect byte replay accepted private record covers exact command range");
   srcPhys=((u64)dma[4]|((u64)dma[5]<<32))&0x0000FFFFFFFFF000ull;
   dstPhys=((u64)dma[6]|((u64)dma[7]<<32))&0x0000FFFFFFFFF000ull;
   srcMc=(u64)dma[36]|((u64)dma[37]<<32);dstMc=(u64)dma[38]|((u64)dma[39]<<32);
   so=(unsigned)(srcMc-gfx.PagingWindow.mc);doff=(unsigned)(dstMc-gfx.PagingWindow.mc-4096);bytes=dma[34]+1;
   for(i=0;i<6;i++){if(srcPhys==physical[i])sp=i;if(dstPhys==physical[i])dp=i;}
   check(dma[0]==SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) && dma[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) &&
         sp<3 && dp>=3 && dp<6 && so<4096 && doff<4096 && bytes<=4096-so && bytes<=4096-doff && bytes==moved,
         "indirect byte replay decodes high PFNs and bounded copy offsets/count");
   if(sp>=3 || dp<3 || dp>=6 || so>=4096 || doff>=4096 || bytes>4096-so || bytes>4096-doff)break;
   memcpy(memory[dp]+doff,memory[sp]+so,bytes);
   check(all_pattern(dma+83,1024-83,0xCCCCCCCCu),"indirect byte replay leaves unused DMA tail untouched");
   total+=moved;pass++;
  }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<8);
  check(status==STATUS_SUCCESS && total==8192 && pass>=2 && pass<=5 && !memcmp(memory,expected,sizeof(memory)),
        "MDL/aperture multi-buffer copy matches independent byte oracle including untouched boundaries");
 }
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_aperture_transfer(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 scratch[16],logical[65536];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT src={0},dst={0};BC250_PAGING_COPY_SLICE slice;
 struct {MDL mdl;PFN_NUMBER pages[2];} m={{8192,0},{0x123,0x987}};
 DXGKARG_BUILDPAGINGBUFFER b={0};u64 pages[2]={0x123000,0x987000},progress,moved;
 u32 dma[2048],priv[2080];ULONG written;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;gfx.PagingWindowReady=1;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"transfer temporary window");
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"transfer permanent aperture");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,65536),"transfer logical state");
 g_VidMm.Ready=g_VidMm.Write=TRUE;
 check(PagingApertureStateMap(&g_VidMm.Aperture,0,2,pages,~4095ull),"transfer maps fragmented physical pages");
 b.Transfer.Source.SegmentId=2;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;
 b.Transfer.Destination.SegmentId=1;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+0x20000);
 b.Transfer.TransferSize=8192;
 check(WddmPreparePhysicalTransfer(&d,&b,&src,&dst,&progress) && src.Aperture && !src.Mdl && !dst.Aperture,
       "segment2 preflight chooses aperture rather than VRAM base");
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4096,4096,dma,0,sizeof(dma),&written,&slice)==STATUS_SUCCESS &&
       slice.SourceSystem && slice.SourcePhysical==0x987000 && !slice.DestinationSystem,
       "fragmented second aperture page resolves to exact system physical identity");
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192 && b.MultipassOffset==2,
       "actual aperture-to-local DDI publishes both page transactions");
 b.MultipassOffset=0;b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+0x20000);
 b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192 && b.MultipassOffset==2,
       "actual local-to-aperture DDI publishes both page transactions");
 b.MultipassOffset=0;b.Fill.Destination.SegmentId=2;
 b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;b.Fill.FillSize=8192;b.Fill.FillPattern=0xBC250;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPhysicalFill(&d,&b,&moved)==STATUS_SUCCESS && moved==8192,
       "aperture fill resolves both mapped system pages through same endpoint route");
 {u64 otherPages[2]={0xAAA000,0xBBB000};
  check(PagingApertureStateMap(&g_VidMm.Aperture,10,2,otherPages,~4095ull),"map disjoint aperture destination");
  b.MultipassOffset=0;b.Transfer.Source.SegmentId=2;
  b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;
  b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.WddmAperture.mc+10*4096);
  b.Transfer.TransferSize=8192;
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192 && b.MultipassOffset==2,
        "disjoint fragmented aperture-to-aperture transfer is admitted");
  m.pages[0]=0xAAA;m.pages[1]=0xBBB;
  b.MultipassOffset=0;b.Transfer.Destination.SegmentId=0;b.Transfer.Destination.pMdl=&m.mdl;
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192,
        "disjoint fragmented aperture-to-MDL transfer is admitted");
  m.pages[0]=0x123;m.pages[1]=0x987;
 }
 src.Aperture=TRUE;src.Mdl=NULL;src.Address=d.WddmAperture.mc;src.Length=8192;
 dst.Aperture=FALSE;dst.Mdl=&m.mdl;dst.Address=0;dst.FirstPage=0;dst.Length=8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,0,17,dma,0,sizeof(dma),&written,&slice)==STATUS_SUCCESS && written==100 &&
       slice.SourceSystem && slice.DestinationSystem && slice.SourcePhysical==slice.DestinationPhysical,
       "aperture/MDL physical alias uses existing VRAM staging transaction");
 pages[0]=0x555000;check(PagingApertureStateMap(&g_VidMm.Aperture,0,1,pages,~4095ull),"publish planned remap");
 check(GfxPagingBuildCopyPage(&d,&src,&dst,0,17,dma,0,sizeof(dma),&written,&slice)==STATUS_SUCCESS &&
       slice.SourcePhysical==0x555000 && slice.DestinationPhysical==0x123000 && written==83,
       "new planned identity used without reading unexecuted hardware GART");
 check(PagingApertureStateUnmap(&g_VidMm.Aperture,1,1),"remove second page");
 b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+0x20000);
 b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;
 b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset &&
       all_pattern(dma,2048,0xCCCCCCCCu),"whole aperture range checked before publishing a mapped prefix");
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

// Replay actual emitted copy packets against an initial byte snapshot. System
// endpoints come from the emitted PTEs; the direct VRAM endpoint is cycle scratch.
static void case_permutation_packets(unsigned partial)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 fence[16],logical[65536];
 static unsigned char memory[5][4096],expected[4][4096];
 const u64 physical[4]={0x100123000ull,0x300789000ull,0x200456000ull,0x400ABC000ull};
 const unsigned permutations[5][3]={{1,0,2},{1,2,0},{0,1,2},{1,0,3},{1,0,1}};
 struct {MDL mdl;PFN_NUMBER pages[3];} source={{12288,0},{0}},destination={{12288,0},{0}};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT src={0},dst={0};
 unsigned mode,plan,i,j,k,nextPage;u32 dma[1024],priv[1050];ULONG written;NTSTATUS status;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"permutation temporary window");
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"permutation aperture");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,65536),"permutation logical state");
 g_VidMm.Ready=g_VidMm.Write=TRUE;
 for(plan=0;plan<5;plan++)for(mode=0;mode<4;mode++) {
  u64 sourcePages[3],destinationPages[3],moved;
  const unsigned partialActions[2][5]={{6,6,0,7,6},{7,8,0,9,9}};
  unsigned start=partial==2?17:0,bytes=partial?8192+17:12288;
  unsigned actions=partial?partialActions[partial-1][plan]:(plan==0 || plan==4)?3:plan==2?0:4;
  if(partial==2 && mode!=3)continue;
  DXGKARG_BUILDPAGINGBUFFER b={0};
  for(i=0;i<3;i++) {
   sourcePages[i]=physical[plan>=3 && i==2?0:i];
   source.pages[i]=sourcePages[i]>>12;
   destinationPages[i]=physical[permutations[plan][i]];
   destination.pages[i]=destinationPages[i]>>12;
  }
  check(PagingApertureStateMap(&g_VidMm.Aperture,0,3,sourcePages,~4095ull) &&
        PagingApertureStateMap(&g_VidMm.Aperture,10,3,destinationPages,~4095ull),"permutation captures source/destination physical identities");
  memset(&src,0,sizeof(src));memset(&dst,0,sizeof(dst));src.Length=dst.Length=bytes;
  if(mode&1){src.Aperture=TRUE;src.Address=d.WddmAperture.mc+start;}else src.Mdl=&source.mdl;
  if(mode&2){dst.Aperture=TRUE;dst.Address=d.WddmAperture.mc+10*4096+start;}else dst.Mdl=&destination.mdl;
  if(actions) {
   memset(dma,0xCC,sizeof(dma));
   status=GfxPagingBuildPageGraph(&d,&src,&dst,bytes,dma,128,83*4,0,&written,&nextPage);
   check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && all_pattern(dma,1024,0xCCCCCCCCu),
         "whole cycle capacity preflight writes no prefix");
  }
  if(mode&1){b.Transfer.Source.SegmentId=2;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)src.Address;}
  else b.Transfer.Source.pMdl=src.Mdl;
  if(mode&2){b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)dst.Address;}
  else b.Transfer.Destination.pMdl=dst.Mdl;
  b.Transfer.TransferSize=bytes;b.DmaBufferWriteOffset=128;
  if(actions)for(i=0;i<2;i++) {
   b.pDmaBuffer=dma;b.DmaSize=i?sizeof(dma):83*4;
   b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=i?PAGING_PRIVATE_HEADER_BYTES+83*4:sizeof(priv);
   memset(dma,0xCC,sizeof(dma));memset(priv,0xCC,sizeof(priv));
   status=WddmBuildPhysicalTransfer(&d,&b,&moved);
   check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset &&
         b.pDmaBuffer==dma && b.pDmaBufferPrivateData==priv && b.DmaBufferWriteOffset==128 &&
         all_pattern(dma,1024,0xCCCCCCCCu),"DDI alias DMA/private shortage publishes no partial cycle or progress");
  }
  for(i=0;i<5;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  memcpy(expected,memory,sizeof(expected));
  for(i=0;i<bytes;i++) {
   unsigned page=(start+i)/4096,offset=(start+i)%4096;
   expected[permutations[plan][page]][offset]=memory[plan>=3 && page==2?0:page][offset];
  }
  memset(dma,0xCC,sizeof(dma));
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildPhysicalTransfer(&d,&b,&moved);
  written=(ULONG)(((u32*)b.pDmaBuffer)-dma);
  check(status==STATUS_SUCCESS && written==actions*83 && moved==bytes && b.MultipassOffset==3 &&
        b.DmaBufferWriteOffset==128 && b.DmaSize==sizeof(dma)-written*4 &&
        b.DmaBufferPrivateDataSize==sizeof(priv)-(written?PAGING_PRIVATE_HEADER_BYTES+written*4:0),
        "DDI complete permutation advances buffers, sizes and terminal token exactly once");
  if(written)check(PagingPrivateVisit(priv,PAGING_PRIVATE_HEADER_BYTES+written*4,128,written*4,0,NULL,NULL),
                   "DDI cycle private record covers exact published command range");
  if(status!=STATUS_SUCCESS || written!=actions*83)continue;
  for(k=0;k<actions;k++) {
   u32* packet=dma+k*83;
   u64 sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
   u64 dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
   u64 sm=(u64)packet[36]|((u64)packet[37]<<32),dm=(u64)packet[38]|((u64)packet[39]<<32);
   unsigned si=5,di=5,so=0,doff=0,copyBytes=packet[34]+1;
   if(sm==gfx.PagingCopyStaging.mc)si=4;
   else if(sm>=gfx.PagingWindow.mc && sm-gfx.PagingWindow.mc<4096){so=(unsigned)(sm-gfx.PagingWindow.mc);for(i=0;i<4;i++)if(sp==physical[i])si=i;}
   if(dm==gfx.PagingCopyStaging.mc)di=4;
   else if(dm>=gfx.PagingWindow.mc+4096 && dm-gfx.PagingWindow.mc-4096<4096){doff=(unsigned)(dm-gfx.PagingWindow.mc-4096);for(i=0;i<4;i++)if(dp==physical[i])di=i;}
   check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && copyBytes<=4096-so && copyBytes<=4096-doff && si<5 && di<5 && si!=di,
         "decode actual SAVE/COPY/RESTORE PTEs and direct scratch address");
   if(si>=5 || di>=5 || si==di || copyBytes>4096-so || copyBytes>4096-doff)break;
   memcpy(memory[di]+doff,memory[si]+so,copyBytes);
  }
  check(k==actions && !memcmp(memory,expected,sizeof(expected)),"emitted cycle preserves initial bytes across MDL/aperture aliases");
  check(all_pattern(dma+written,1024-written,0xCCCCCCCCu),"permutation leaves unused packet storage intact");
  {PVOID afterDma=b.pDmaBuffer,afterPrivate=b.pDmaBufferPrivateData;ULONG left=b.DmaSize,leftPrivate=b.DmaBufferPrivateDataSize;
   check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && !moved &&
         b.pDmaBuffer==afterDma && b.pDmaBufferPrivateData==afterPrivate && b.DmaSize==left &&
         b.DmaBufferPrivateDataSize==leftPrivate && b.MultipassOffset==3,
         "completed alias token emits no duplicate cycle and reports no new bytes");}
  if(partial) {
   u32 complete[1024];unsigned copied=0,passes=0;u64 total=0;
   memcpy(complete,dma,sizeof(complete));b.MultipassOffset=0;
   do {
    unsigned words,balance=0;
    b.pDmaBuffer=dma;b.DmaSize=384*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
    status=WddmBuildPhysicalTransfer(&d,&b,&moved);
    words=(unsigned)(((u32*)b.pDmaBuffer)-dma);
    check((status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) &&
          (status==STATUS_SUCCESS || words) && words%83==0 && copied+words<=actions*83 &&
          b.DmaSize==384*4-words*4 && b.DmaBufferWriteOffset==128,
          "partial bands make productive bounded progress across buffers");
    if(words%83 || copied+words>actions*83 || (!words && status!=STATUS_SUCCESS))break;
    for(k=0;k<words;k+=83) {
     u64 from=(u64)dma[k+36]|((u64)dma[k+37]<<32),to=(u64)dma[k+38]|((u64)dma[k+39]<<32);
     check(!memcmp(dma+k+4,complete+copied+k+4,4*sizeof(u32)) &&
           !memcmp(dma+k+33,complete+copied+k+33,7*sizeof(u32)),
           "resumed packet PTEs and byte copies match independently replayed complete plan");
     if(to==gfx.PagingCopyStaging.mc)balance++;
     if(from==gfx.PagingCopyStaging.mc){check(balance==1,"partial-band restore has save in same buffer");balance--;}
    }
    check(balance==0,"partial-band submission retains no scratch value for the next buffer");
    if(words)check(PagingPrivateVisit(priv,PAGING_PRIVATE_HEADER_BYTES+words*4,128,words*4,0,NULL,NULL),
                   "partial-band private record covers exact published prefix");
    copied+=words;total+=moved;passes++;
   }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && passes<16);
   check(status==STATUS_SUCCESS && copied==actions*83 && total==bytes && b.MultipassOffset==3,
         "resumed bands reach terminal token with all bytes accounted once");
  }

 }
 check(!flush_lock_depth && !flush_region_depth,"permutation releases engine lifetime ownership");
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

// Three disjoint cycles with noncontiguous members and skipped identity pages.
// DMA, private-record and hardware-ring limits independently force resumes.
static void case_permutation_multipass(unsigned longCycle)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 fence[16],logical[65536];
 static unsigned char memory[9][4096],expected[8][4096];
 const unsigned permutations[2][8]={{0,7,2,5,6,3,4,1},{1,2,3,4,5,6,7,0}};
 const unsigned* permutation=permutations[longCycle];
 const unsigned passes=longCycle?7:3;
 u64 physical[8],destinationPages[8];
 struct {MDL mdl;PFN_NUMBER pages[8];} source={{32768,0},{0}},destination={{32768,0},{0}};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned mode,limit,i,j,k,pass;u32 dma[2048],priv[2070];
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"cycle multipass window");
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"cycle multipass aperture");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,65536),"cycle multipass logical state");
 g_VidMm.Ready=g_VidMm.Write=TRUE;
 for(i=0;i<8;i++)physical[i]=0x100123000ull+(u64)i*0x100003000ull;
 for(i=0;i<8;i++){source.pages[i]=physical[i]>>12;destinationPages[i]=physical[permutation[i]];destination.pages[i]=destinationPages[i]>>12;}
 check(PagingApertureStateMap(&g_VidMm.Aperture,0,8,physical,~4095ull) &&
       PagingApertureStateMap(&g_VidMm.Aperture,10,8,destinationPages,~4095ull),"cycle multipass physical identities");
 for(mode=0;mode<4;mode++)for(limit=longCycle?2:0;limit<3;limit++) {
  DXGKARG_BUILDPAGINGBUFFER b={0};u64 total=0,moved;NTSTATUS status=STATUS_SUCCESS;
  ring.max_dw=limit==2?512:4096;
  if(mode&1){b.Transfer.Source.SegmentId=2;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;}
  else b.Transfer.Source.pMdl=&source.mdl;
  if(mode&2){b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.WddmAperture.mc+10*4096);}
  else b.Transfer.Destination.pMdl=&destination.mdl;
  b.Transfer.TransferSize=32768;
  for(i=0;i<9;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  for(i=0;i<8;i++)memcpy(expected[permutation[i]],memory[i],4096);
  for(pass=0;pass<passes;pass++) {
   ULONG dmaBytes=limit==0?384*4:sizeof(dma);
   ULONG privateBytes=limit==1?PAGING_PRIVATE_HEADER_BYTES+384*4:sizeof(priv);
   b.pDmaBuffer=dma;b.DmaSize=dmaBytes;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=privateBytes;
   b.DmaBufferWriteOffset=0;memset(dma,0xCC,sizeof(dma));memset(priv,0xCC,sizeof(priv));
   status=WddmBuildPhysicalTransfer(&d,&b,&moved);
   check(status==(pass+1==passes?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) &&
         b.MultipassOffset==(pass+1==passes?8:PAGING_PERMUTATION_RESUME|((pass+1)*3)) && moved==(pass+1==passes?32768ull:0) &&
         b.pDmaBuffer==dma+249 && b.DmaSize==dmaBytes-249*4 &&
         b.DmaBufferPrivateDataSize==privateBytes-PAGING_PRIVATE_HEADER_BYTES-249*4,
         "each DDI resume publishes exactly one complete cycle and next atomic-group token");
   if(b.pDmaBuffer!=dma+249)break;
   check(PagingPrivateVisit(priv,PAGING_PRIVATE_HEADER_BYTES+249*4,0,249*4,0,NULL,NULL),"multipass cycle private record coverage");
   for(k=0;k<3;k++) {
    u32* packet=dma+k*83;
    u64 sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
    u64 dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
    u64 sm=(u64)packet[36]|((u64)packet[37]<<32),dm=(u64)packet[38]|((u64)packet[39]<<32);
    unsigned si=9,di=9;
    if(sm==gfx.PagingCopyStaging.mc)si=8;
    else if(sm==gfx.PagingWindow.mc)for(i=0;i<8;i++)if(sp==physical[i])si=i;
    if(dm==gfx.PagingCopyStaging.mc)di=8;
    else if(dm==gfx.PagingWindow.mc+4096)for(i=0;i<8;i++)if(dp==physical[i])di=i;
    check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && packet[34]==4095 && si<9 && di<9 && si!=di,
          "multipass actual packets decode bounded page copy");
    if(si>=9 || di>=9 || si==di)break;
    memcpy(memory[di],memory[si],4096);
   }
   total+=moved;
   memset(memory[8],0xED,4096); // An unrelated retired paging job reuses scratch.
  }
  check(pass==passes && status==STATUS_SUCCESS && total==32768 && !memcmp(memory,expected,sizeof(expected)),
        "all multipass alias bytes match initial snapshot despite scratch overwrite between jobs");
 }
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}


/* Actual virtual DDI byte oracle, including a cycle split into independently
 * submitted scratch-complete groups and partial ranges with untouched bytes. */
static void case_virtual_alias_ordering(unsigned walkMode)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 fence[16];
 static unsigned char memory[7][4096],expected[6][4096];
 const u64 physical[6]={0x100123000ull,0x300789000ull,0x200456000ull,
                       0x400abc000ull,0x500def000ull,0x600987000ull};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned scenario,limit,i,j,k;u32 dma[1024],priv[1100];
 u64 root=4096,app=0,appLength=0,table=0,tableLength=0;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"virtual graph temporary window");
 virtual_graph_map=walkMode==0;use_retained_walk=walkMode!=0;
 if(walkMode) {
  d.FullWddm=d.VramWriteEnabled=1;cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
  check(WddmMemoryLayout(&d,&app,&appLength,&table,&tableLength),"virtual graph actual table layout");
  check(VidMmStartLayout(&d,app,appLength,1,table,tableLength,3)==STATUS_SUCCESS,"virtual graph actual table lifetime startup");
  root=(u64)d.VramPhysical.QuadPart+table+4096;
 }

 for(scenario=0;scenario<3;scenario++)for(limit=0;limit<2;limit++) {
  DXGKARG_BUILDPAGINGBUFFER b={0};u64 moved=0,total=0;unsigned passes=0;
  unsigned start=scenario==2?17:0,bytes=scenario==2?8192+17:12288;
  NTSTATUS status;
  ring.max_dw=limit?384:1024;
  for(i=0;i<3;i++){virtual_graph_pages[i]=physical[i];virtual_graph_pages[3+i]=physical[scenario?(i+1)%3:3+i];}
  if(walkMode) {
   DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0},leaves[6];
   memset(cpu_storage,0,sizeof(cpu_storage));
   u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
   pte.Flags=BC250_DXGK_PTE_VALID|(3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
   for(i=1;i<4;i++) {
    u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;pte.PageAddress=i+1;
    check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"virtual graph initializes actual hierarchy directories");
   }
   for(i=0;i<6;i++) {
    leaves[i].Flags=BC250_DXGK_PTE_VALID|BC250_DXGK_PTE_CACHECOHERENT;
    leaves[i].PageAddress=(walkMode==2?physical[i]:virtual_graph_pages[i])>>12;
   }
   u.PageTableLevel=0;u.PageTableAddress.CpuVirtual=cpu_storage+4*512;u.StartIndex=16;
   u.NumPageTableEntries=6;u.pPageTableEntries=leaves;
   check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"virtual graph initializes actual system leaf PTEs");
   if(walkMode==2) {
    u32 commands[128],updateDma[128],updatePrivate[150];ULONG written=0,next=0;
    BC250_WDDM_PAGING_UNSUPPORTED unsupported;
    DXGKARG_BUILDPAGINGBUFFER update={0};u64 pa;BOOLEAN system;
    for(i=0;i<6;i++)leaves[i].PageAddress=virtual_graph_pages[i]>>12;
    u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    u.PageTableAddress.GpuPhysical.SegmentId=3;u.PageTableAddress.GpuPhysical.SegmentOffset=4*4096;
    check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),0,&written,&next,&unsupported)==STATUS_SUCCESS && next==6,
          "virtual graph builds queued system PTE remap");
    for(i=0;i<6;i++)check(VidMmTranslatePaging(root,(16ull+i)*4096,&pa,&system) && system && pa==physical[i],
                         "unpublished PTE remap leaves logical graph source unchanged");
    memcpy(updatePrivate+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
    update.UpdatePageTable=u;update.pDmaBuffer=updateDma;update.DmaSize=sizeof(updateDma);
    update.pDmaBufferPrivateData=updatePrivate;update.DmaBufferPrivateDataSize=sizeof(updatePrivate);
    check(WddmPublishPagingRecord(&update,written,TRUE,0,next)==STATUS_SUCCESS,
          "virtual graph observes only accepted PTE remap publication");
    for(i=0;i<6;i++)check(VidMmTranslateRetained(root,(16ull+i)*4096,&pa,&system) && system && pa==physical[i],
                         "live table still names pre-update pages until GPU execution");
   }
  }
  for(i=0;i<7;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  memcpy(expected,memory,sizeof(expected));
  for(i=0;i<bytes;i++) {
   unsigned page=(start+i)/4096,offset=(start+i)%4096;
   expected[scenario?(page+1)%3:3+page][offset]=memory[page][offset];
  }
  for(i=0;i<6;i++) {
   ULONGLONG pa=0;BOOLEAN system=FALSE;
   check(VidMmTranslatePaging(root,(16ull+i)*4096+123,&pa,&system) && system &&
         pa==virtual_graph_pages[i]+123,"virtual graph translation positive control preserves page and offset");
  }
  b.TransferVirtual.SourceVirtualAddress=16*4096+start;b.TransferVirtual.DestinationVirtualAddress=19*4096+start;
  b.TransferVirtual.TransferSizeInBytes=bytes;
  do {
   unsigned words,balance=0,old=b.MultipassOffset;
   b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
   memset(dma,0xCC,sizeof(dma));memset(memory[6],0xCD,sizeof(memory[6]));
   status=WddmBuildVirtualTransfer(&d,root,&b,&moved);words=(unsigned)((u32*)b.pDmaBuffer-dma);
   check((status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && words && words%83==0 &&
         words<=1024 && b.MultipassOffset!=old && b.DmaSize==sizeof(dma)-words*4,
         "virtual graph emits productive bounded prefix with exact DMA accounting");
   if((status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) || !words || words%83 || words>1024)break;
   check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),0,words*4,0,NULL,NULL),
         "virtual graph records cover published packets");
   for(k=0;k<words;k+=83) {
    u32* packet=dma+k;
    u64 sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
    u64 dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
    u64 sm=(u64)packet[36]|((u64)packet[37]<<32),dm=(u64)packet[38]|((u64)packet[39]<<32);
    unsigned si=7,di=7,so=0,off=0,n=packet[34]+1;
    if(sm==gfx.PagingCopyStaging.mc){si=6;check(balance==1,"virtual restore follows save in this buffer");balance--;}
    else if(sm>=gfx.PagingWindow.mc && sm-gfx.PagingWindow.mc<4096){so=(unsigned)(sm-gfx.PagingWindow.mc);for(i=0;i<6;i++)if(sp==physical[i])si=i;}
    if(dm==gfx.PagingCopyStaging.mc){di=6;balance++;}
    else if(dm>=gfx.PagingWindow.mc+4096 && dm-gfx.PagingWindow.mc-4096<4096){off=(unsigned)(dm-gfx.PagingWindow.mc-4096);for(i=0;i<6;i++)if(dp==physical[i])di=i;}
    check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && n<=4096-so && n<=4096-off && si<7 && di<7 && si!=di,
          "virtual graph decodes physical copies and bounded scratch transactions");
    if(si>=7 || di>=7 || si==di || n>4096-so || n>4096-off)break;
    memcpy(memory[di]+off,memory[si]+so,n);
   }
   check(k==words && balance==0,"virtual graph scratch is complete before another job may overwrite it");
   check(all_pattern(dma+words,1024-words,0xCCCCCCCCu),"virtual graph preserves unused DMA storage");
   total+=moved;passes++;
  }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && passes<16);
  check(status==STATUS_SUCCESS && total==bytes && b.MultipassOffset==3 && (!limit || !scenario || passes>1),
        "virtual graph finishes once across bounded submissions");
  check(!memcmp(memory,expected,sizeof(expected)),scenario?
        "virtual cross-page cycle preserves initial source bytes":
        "virtual disjoint positive control preserves initial source bytes");
  {PVOID after=b.pDmaBuffer;ULONG remaining=b.DmaSize;
   check(WddmBuildVirtualTransfer(&d,root,&b,&moved)==STATUS_SUCCESS && !moved && b.pDmaBuffer==after && b.DmaSize==remaining,
         "virtual completed token does not emit duplicate work");}
 }
 if(walkMode){VidMmStop();check(!cpu_map_live && !shadow_pool_live,"virtual graph table lifetime fully released");}
 use_retained_walk=0;virtual_graph_map=0;memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
 check(!flush_lock_depth && !flush_region_depth,"virtual graph releases engine lifetime ownership");
}


static void case_graph_shadow_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update={0};DXGK_PTE entries[512];
 DXGKARG_BUILDPAGINGBUFFER build={0};BC250_PAGING_ENDPOINT src={0},dst={0};
 PAGING_GRAPH_BATCH graph={0};PAGING_PAGE_MOVE moves[9];
 unsigned source[3]={0,1,2},destination[3]={1,2,0},readers[3],writer[3],queue[3],forward[3],n=0;
 unsigned char system[3]={0,0,0};u64 pages[3],base=0x200000000ull;
 static u64 memory[4][512],initial[3][512];
 u32 commands[128],dma[128],priv[160];unsigned i,j,k,used=0;ULONG written;NTSTATUS status;
 d.FullWddm=d.VramEnabled=d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(LONGLONG)base;
 d.VramMcBase=0x100000000ull;d.VramLength=8ull<<30;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"graph publication reserves actual VidMm state");
 for(i=0;i<3;i++) {
  pages[i]=base+(4ull+i)*4096;
  for(j=0;j<512;j++){entries[j].Flags=BC250_DXGK_PTE_VALID|BC250_DXGK_PTE_CACHECOHERENT;entries[j].PageAddress=0x100+i*512+j;}
  update.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;update.PageTableLevel=0;
  update.PageTableAddress.CpuVirtual=cpu_storage+(4+i)*512;update.NumPageTableEntries=512;update.pPageTableEntries=entries;
  check(VidMmUpdatePageTable(&update)==STATUS_SUCCESS,"graph publication initializes source table bytes");
  memcpy(initial[i],cpu_storage+(4+i)*512,4096);memcpy(memory[i],initial[i],4096);
 }
 check(PagingPageGraphPlan(source,destination,3,3,readers,writer,queue,forward,moves,9,9,&n) && n==4,
       "graph publication obtains actual cyclic physical plan");
 src.Length=dst.Length=4096;
 for(i=0;i<n;i++) {
  src.Address=moves[i].source==PAGING_PERMUTATION_SCRATCH?gfx.PagingCopyStaging.mc:pages[moves[i].source]-base+d.VramMcBase;
  dst.Address=moves[i].destination==PAGING_PERMUTATION_SCRATCH?gfx.PagingCopyStaging.mc:pages[moves[i].destination]-base+d.VramMcBase;
  status=GfxPagingBuildCopyPage(&d,&src,&dst,0,4096,commands+used,64+used*4,sizeof(commands)-used*4,&written,
                               &(BC250_PAGING_COPY_SLICE){0});
  check(status==STATUS_SUCCESS && written==7,"graph publication builds actual direct-copy packet per action");
  if(status!=STATUS_SUCCESS || written!=7)break;
  used+=written;
 }
 graph.Pages=pages;graph.SystemPages=system;graph.Identities=3;graph.Moves=moves;graph.Count=n;graph.Bytes=4096;
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,used*4);memset(dma,0xCC,sizeof(dma));
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaBufferWriteOffset=64;
 for(i=0;i<2;i++) {
  build.DmaSize=i?sizeof(dma):used*4-1;
  build.DmaBufferPrivateDataSize=i?PAGING_PRIVATE_HEADER_BYTES+used*4-1:sizeof(priv);
  check(WddmPublishPagingRecordCore(&build,used,FALSE,0,0,0,0,0,0,0,0,NULL,&graph)==STATUS_INVALID_PARAMETER &&
        build.pDmaBuffer==dma && build.pDmaBufferPrivateData==priv && all_pattern(dma,128,0xCCCCCCCCu),
        "graph publication rejects short capacity before publishing commands");
  for(j=0;j<3;j++)for(k=0;k<512;k++){u64 v=0;check(PagingPtShadowRead(&g_VidMm.Shadow,pages[j],k,&v)==PAGING_PT_OK && v==initial[j][k],
        "capacity refusal leaves complete logical table snapshot unchanged");}
 }
 build.DmaSize=sizeof(dma);build.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmPublishPagingRecordCore(&build,used,FALSE,0,0,0,0,0,0,0,0,NULL,&graph)==STATUS_SUCCESS &&
       build.pDmaBuffer==dma+used && build.DmaSize==sizeof(dma)-used*4 && !memcmp(dma,commands,used*4),
       "graph publication commits metadata and publishes identical accepted packet bytes");
 check(PagingPrivateVisit(priv,PAGING_PRIVATE_HEADER_BYTES+used*4,64,used*4,0,NULL,NULL),
       "graph publication private range matches command bytes");
 for(i=0;i<n;i++) {
  u32* cmd=dma+i*7;u64 from=(u64)cmd[3]|((u64)cmd[4]<<32),to=(u64)cmd[5]|((u64)cmd[6]<<32);
  unsigned si=4,di=4;
  if(from==gfx.PagingCopyStaging.mc)si=3;if(to==gfx.PagingCopyStaging.mc)di=3;
  for(j=0;j<3;j++){if(from==pages[j]-base+d.VramMcBase)si=j;if(to==pages[j]-base+d.VramMcBase)di=j;}
  check(cmd[0]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && cmd[1]+1==4096 && si<4 && di<4 && si!=di,
        "graph publication replay decodes actual packet physical endpoints");
  if(si>=4 || di>=4 || si==di)break;
  memcpy(memory[di],memory[si],4096);
 }
 for(i=0;i<3;i++)for(j=0;j<512;j++) {
  u64 v=0;
  check(PagingPtShadowRead(&g_VidMm.Shadow,pages[i],j,&v)==PAGING_PT_OK && v==initial[(i+2)%3][j] && v==memory[i][j],
        "published logical table cycle matches independently replayed GPU bytes");
  check(cpu_storage[(4+i)*512+j]==initial[i][j],"logical publication does not write live table backing");
 }
 VidMmStop();check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth,"graph publication lifetime and lock fully released");
}


static void case_retained_graph_capture(unsigned owned)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 fence[16];
 static unsigned char memory[4][4096],expected[3][4096];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned kind,partial,i,j,k,offset=owned==2?0:64;u64 root=4096,physical[3];u32 commands[1024],privateData[2048];ULONG written;NTSTATUS status;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=384;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"captured graph window");
 if(owned) {
  d.FullWddm=d.VramWriteEnabled=1;cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
  check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"owned graph VidMm startup");
 }
 if(owned==2){root=(u64)d.VramPhysical.QuadPart+4096;ring.max_dw=64;}
 for(kind=owned==2?2:0;kind<3;kind++)for(partial=0;partial<2;partial++) {
  PAGING_CAPTURE_OWNER owner={0};PAGING_GRAPH_CAPTURE* c=NULL;PAGING_GRAPH_BATCH batch;
  DXGKARG_BUILDPAGINGBUFFER build={0};ULONGLONG moved=0;
  unsigned band=0,action=0,nextBand=0,nextAction=0,passes=0,planCalls=0,bandCount=0;
  unsigned start=partial?17:0,bytes=partial?8192+17:12288;int result;
  physical[0]=kind==2?0x200010000ull:0x100123000ull;physical[1]=0x200020000ull;physical[2]=0x200030000ull;
  virtual_graph_map=1;virtual_graph_local_mask=kind==2?63:kind==1?30:0;
  for(i=0;i<3;i++){virtual_graph_pages[i]=physical[i];virtual_graph_pages[3+i]=physical[(i+1)%3];}
  if(owned==2) {
   DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE entries[512];
   virtual_graph_map=0;use_retained_walk=1;
   u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.pPageTableEntries=entries;
   for(i=1;i<4;i++) {
    entries[0].Flags=BC250_DXGK_PTE_VALID|(1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);entries[0].PageAddress=i+1;
    u.NumPageTableEntries=1;u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
    check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"owned self-table hierarchy initialization");
   }
   for(i=0;i<3;i++) {
    physical[i]=(u64)d.VramPhysical.QuadPart+(4ull+i)*4096;
    for(j=0;j<512;j++) {
     entries[j].Flags=BC250_DXGK_PTE_VALID|BC250_DXGK_PTE_CACHECOHERENT;
     entries[j].PageAddress=32+i*512+j;
    }
    if(!i)for(j=0;j<6;j++){entries[16+j].PageAddress=4+(j<3?j:(j-2)%3);entries[16+j].Flags=BC250_DXGK_PTE_VALID|(1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);}
    u.NumPageTableEntries=512;u.PageTableLevel=0;u.StartIndex=0;u.PageTableAddress.CpuVirtual=cpu_storage+(4+i)*512;
    check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"owned self-table leaf initialization");
   }
   for(i=0;i<6;i++) {
    u64 pa=0;BOOLEAN sys=TRUE;
    check(VidMmTranslatePaging(root,(16ull+i)*4096,&pa,&sys) && !sys && pa==physical[i<3?i:(i-2)%3],
          "owned self-table real walk positive control");
   }
  }
  if(owned) {
   build.TransferVirtual.SourceVirtualAddress=16*4096+start;
   build.TransferVirtual.DestinationVirtualAddress=19*4096+start;
   build.TransferVirtual.TransferSizeInBytes=bytes;
   build.TransferVirtual.hAllocation=&d;build.TransferVirtual.AllocationOffsetInBytes=17;
   build.pDmaBuffer=commands;build.pDmaBufferPrivateData=privateData;
   build.DmaBufferPrivateDataSize=sizeof(privateData);build.DmaBufferWriteOffset=offset;
   if(owned==3) {
    check(WddmReserveCaptures(&owner)==STATUS_SUCCESS && owner.StorageBytes==GfxPagingCaptureStorageSize(0,0,1ull<<30),
          "system context reserves full paging window before transfer");
    shadow_pool_fail=1;
   }
   check(WddmBuildCapturedVirtualTransfer(&d,root,&owner,&build,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
         !moved && owner.Head && !owner.Head->Band && !owner.Head->Action,
         "owned initial empty buffer retains capture without advancing");
   c=(PAGING_GRAPH_CAPTURE*)PagingCaptureFind(&owner,build.MultipassOffset);
  } else
  check(GfxPagingCaptureVirtualGraph(&d,root,16*4096+start,19*4096+start,bytes,&c)==STATUS_SUCCESS && c,
        "capture accepts system, mixed or local virtual cycle");
  if(!c)continue;
  if(!owned) {
   PAGING_GRAPH_CAPTURE* again=NULL;int live=shadow_pool_live;
   SIZE_T needed=GfxPagingCaptureStorageSize(16*4096+start,19*4096+start,bytes);
   check(needed==shadow_pool_last_bytes,"in-place size matches allocated capture");
   check(GfxPagingCaptureStorageSize(0,0,0)==0 &&
         GfxPagingCaptureStorageSize(~0ull,0,1)==0,"capture size rejects empty/overflowing ranges");
   check(GfxPagingCaptureStorageSize(0,0,1ull<<30)==sizeof(*c)+(SIZE_T)262144*84,
         "documented system paging VA bound has exact storage budget");
   check(GfxPagingCaptureVirtualGraphInPlace(&d,root,16*4096+start,19*4096+start,bytes,c,needed-1,&again)==STATUS_INVALID_PARAMETER &&
         !again && c->Owner.Root==root,"short reservation preserves existing storage");
   memset(c,0xA5,needed);shadow_pool_fail=1;
   check(GfxPagingCaptureVirtualGraphInPlace(&d,root,16*4096+start,19*4096+start,bytes,c,needed,&again)==STATUS_SUCCESS &&
         again==c && shadow_pool_live==live && !c->Owner.Token && !c->Owner.Next,
         "poisoned reservation rebuilds without allocating or freeing");
   shadow_pool_fail=0;c->Owner.PoolTag=BC250_GFX_TAG;
  }
  if(owned==3)check((void*)c==owner.Storage && !PagingCaptureIdleStorage(&owner),
                    "actual DDI retains reservation while allocation is unavailable");
  else check(shadow_pool_last_bytes==sizeof(*c)+(SIZE_T)c->PageCount*84,"capture allocation reuses dead normalization workspace");
  check(c->PageCount==3 && c->Identities==3 && c->Owner.Root==root && c->Owner.Bytes==bytes,
        "capture retains complete operation identity");
  for(i=0;i<3;i++)check(c->Pages[c->SourceIndex[i]]==physical[i] && c->Pages[c->DestinationIndex[i]]==physical[(i+1)%3] &&
       c->SystemPages[c->SourceIndex[i]]==(kind==0 || (kind==1 && i==0)),"capture preserves physical addresses and access domain");
  check((owned || PagingCaptureAttach(&owner,&c->Owner)) && PagingCaptureFind(&owner,c->Owner.Token)==&c->Owner,"context owns captured immutable arrays");
  if(owned) {
   build.DmaSize=sizeof(commands);g_VidMm.Write=0;
   check(WddmBuildCapturedVirtualTransfer(&d,root,&owner,&build,&moved)!=STATUS_SUCCESS &&
         !moved && !c->Owner.Band && !c->Owner.Action && build.pDmaBuffer==commands &&
         build.DmaSize==sizeof(commands) && build.pDmaBufferPrivateData==privateData && build.DmaBufferWriteOffset==offset,
         "refused logical publication preserves owner cursor and DMA progress");
   g_VidMm.Write=1;
  }
  // Every attempted VA lookup now fails. Batches must use captured identities.
  virtual_graph_map=0;translationOk=0;
  for(i=0;i<6;i++)virtual_graph_pages[i]=0xABC000+(u64)i*4096;
  for(i=0;i<4;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  if(owned==2)for(i=0;i<3;i++)memcpy(memory[i],cpu_storage+(4+i)*512,4096);
  memcpy(expected,memory,sizeof(expected));
  for(i=0;i<bytes;i++)expected[((start+i)/4096+1)%3][(start+i)%4096]=memory[(start+i)/4096][(start+i)%4096];
  check(GfxPagingPlanCapturedGraph(c,0,0,0,&batch,&nextBand,&nextAction)==PagingPermutationNeedCycle &&
        !batch.Count && !nextBand && !nextAction && !c->Owner.Band && !c->Owner.Action,
        "capacity refusal leaves captured progress unchanged");
  memset(commands,0xCC,sizeof(commands));
  check(GfxPagingEmitCapturedGraph(&d,c,0,0,commands,64,0,&written,&batch,&nextBand,&nextAction)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
        !written && !batch.Count && !nextBand && !nextAction && all_pattern(commands,1024,0xCCCCCCCCu),
        "captured emitter refuses empty capacity without bytes or progress");
  planCalls=captured_graph_plan_calls;bandCount=c->BandCount;
  do {
   unsigned balance=0,word=0;
   memset(memory[3],0xCD,sizeof(memory[3]));memset(commands,0xCC,sizeof(commands));
   if(owned) {
    build.pDmaBuffer=commands;build.DmaSize=owned==2?48*4:sizeof(commands);
    build.pDmaBufferPrivateData=privateData;build.DmaBufferPrivateDataSize=sizeof(privateData);
    status=WddmBuildCapturedVirtualTransfer(&d,root,&owner,&build,&moved);
    written=((owned==2?48*4:sizeof(commands))-build.DmaSize)/4;
    result=status==STATUS_SUCCESS?PagingPermutationDone:PagingPermutationMore;
    check(PagingPrivateVisit(privateData,(unsigned)(sizeof(privateData)-build.DmaBufferPrivateDataSize),offset,written*4,0,NULL,NULL),
          "owned multi-record publication covers exact DMA bytes");
    check((status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && written &&
          build.DmaBufferWriteOffset==offset && moved==(status==STATUS_SUCCESS?bytes:0),
          "owned callback publishes bytes with exact completion accounting");
    check((status==STATUS_SUCCESS && !owner.Head && build.MultipassOffset==PAGING_CAPTURE_COMPLETE) ||
          (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && PagingCaptureFind(&owner,build.MultipassOffset)),
          "owned callback keeps opaque token until final release");
    if(!written || written>1024)break;
   } else {
   status=GfxPagingEmitCapturedGraph(&d,c,band,action,commands,64,sizeof(commands),&written,&batch,&nextBand,&nextAction);
   result=nextBand==c->BandCount?PagingPermutationDone:PagingPermutationMore;
   check(status==STATUS_SUCCESS && batch.Count && written && written<=1024 &&
         (nextBand!=band || nextAction!=action) && !c->Owner.Band && !c->Owner.Action,
         "captured packets use changed-VA-independent addresses and propose progress without committing it");
   if(status!=STATUS_SUCCESS || !batch.Count || !written || written>1024)break;
   }
   for(k=0;owned?word<written:k<batch.Count;k++) {
    u32* packet=commands+word;unsigned mapped,words,n,from=4,to=4,fromOffset=0,toOffset=0;
    u64 sm,dm,sp=0,dp=0;unsigned sourceSystem,destinationSystem;
    if(word>=written)break;
    mapped=packet[0]!=SDMA_PKT_HEADER_OP(SDMA_OP_COPY);words=mapped?83:7;
    check(words<=written-word,"captured packet fits emitted range");if(words>written-word)break;
    if(mapped) {
     sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
     dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
     sm=(u64)packet[36]|((u64)packet[37]<<32);dm=(u64)packet[38]|((u64)packet[39]<<32);n=packet[34]+1;
     check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY),"captured mapped packet includes actual copy");
    } else {sm=(u64)packet[3]|((u64)packet[4]<<32);dm=(u64)packet[5]|((u64)packet[6]<<32);n=packet[1]+1;}
    sourceSystem=sm>=gfx.PagingWindow.mc && sm-gfx.PagingWindow.mc<4096;
    destinationSystem=dm>=gfx.PagingWindow.mc+4096 && dm-gfx.PagingWindow.mc-4096<4096;
    if(sm==gfx.PagingCopyStaging.mc){from=3;check(balance==1,"captured restore has current-batch save");balance--;}
    else {
     u64 pa=sourceSystem?sp+sm-gfx.PagingWindow.mc:sm-d.VramMcBase+(u64)d.VramPhysical.QuadPart;
     fromOffset=(unsigned)(pa&4095);for(i=0;i<3;i++)if((pa&~4095ull)==physical[i])from=i;
    }
    if(dm==gfx.PagingCopyStaging.mc){to=3;balance++;}
    else {
     u64 pa=destinationSystem?dp+dm-gfx.PagingWindow.mc-4096:dm-d.VramMcBase+(u64)d.VramPhysical.QuadPart;
     toOffset=(unsigned)(pa&4095);for(i=0;i<3;i++)if((pa&~4095ull)==physical[i])to=i;
    }
    check(from<4 && to<4 && from!=to && (owned || n==batch.Bytes) && n<=4096-fromOffset && n<=4096-toOffset,
          "captured actual COPY addresses identify bounded original physical bytes");
    if(from>=4 || to>=4 || from==to || n>4096-fromOffset || n>4096-toOffset)break;
    if(!owned)check((batch.Moves[k].source==PAGING_PERMUTATION_SCRATCH || sourceSystem==batch.SystemPages[batch.Moves[k].source]) &&
          (batch.Moves[k].destination==PAGING_PERMUTATION_SCRATCH || destinationSystem==batch.SystemPages[batch.Moves[k].destination]),
          "captured packet access domains match associated metadata descriptor");
    memcpy(memory[to]+toOffset,memory[from]+fromOffset,n);word+=words;
   }
   check((owned || k==batch.Count) && word==written && balance==0,"captured batch accounts for all packet bytes and completes scratch ownership");
   check(all_pattern(commands+written,1024-written,0xCCCCCCCCu),"captured emitter preserves unused DMA storage");
   if(owned==2) {
    for(i=0;i<3;i++)for(j=0;j<512;j++) {
     u64 value=0,expectedValue;memcpy(&expectedValue,memory[i]+j*8,8);
     check(PagingPtShadowRead(&g_VidMm.Shadow,physical[i],j,&value)==PAGING_PT_OK && value==expectedValue,
           "owned self-table logical entries match independent packet replay per callback");
    }
    if(!partial && !passes) {
     u64 pa=0;BOOLEAN sys=TRUE;
     check(!VidMmTranslatePaging(root,16*4096,&pa,&sys) || pa!=physical[0] || sys,
           "owned transfer actually changes its own source translation before resume");
     check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && owner.Head,
           "owned self-table transfer requires resume after translation changed");
    }
   }
   band=nextBand;action=nextAction;passes++;
  }while(result==PagingPermutationMore && passes<16);
  check(captured_graph_plan_calls-planCalls<=bandCount-1,"resumes plan at most once per new byte band, never per DMA buffer");
  check(result==PagingPermutationDone && !memcmp(memory,expected,sizeof(expected)),"captured cycle preserves initial bytes after mapping changes");
  if(owned) {
   void* before=build.pDmaBuffer;
   check(WddmBuildCapturedVirtualTransfer(&d,root,&owner,&build,&moved)==STATUS_SUCCESS &&
         !moved && build.pDmaBuffer==before && !owner.Head,"owned terminal repeat is a no-op");
  } else {
   check(PagingCaptureDetach(&owner,c->Owner.Token)==&c->Owner && !owner.Head,"completed capture relinquishes owner before release");
   ExFreePoolWithTag(c,c->Owner.PoolTag);
  }
  translationOk=1;
  if(owned==3) {
   unsigned token=owner.LastId;int live=shadow_pool_live;
   check(PagingCaptureIdleStorage(&owner)==owner.Storage,"completion returns reservation to context");
   virtual_graph_map=1;
   for(i=0;i<3;i++){virtual_graph_pages[i]=physical[i];virtual_graph_pages[3+i]=physical[(i+1)%3];}
   build.MultipassOffset=0;build.DmaSize=0;build.pDmaBuffer=commands;
   build.pDmaBufferPrivateData=privateData;build.DmaBufferPrivateDataSize=sizeof(privateData);
   check(WddmBuildCapturedVirtualTransfer(&d,root,&owner,&build,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
         owner.Head==(PAGING_CAPTURE*)owner.Storage && owner.LastId==token+1 && shadow_pool_live==live,
         "next operation reuses reservation without allocation and with fresh token");
   shadow_pool_fail=0;token=build.MultipassOffset;build.MultipassOffset=0;
   check(WddmBuildCapturedVirtualTransfer(&d,root,&owner,&build,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
         (void*)owner.Head!=owner.Storage && PagingCaptureFind(&owner,token)==owner.Storage &&
         shadow_pool_live==live+1 && !PagingCaptureIdleStorage(&owner),
         "interleaved operation retains independent heap capture without overwriting reservation");
   WddmReleaseCaptureOwner(&owner);
   check(!owner.Head && !owner.Storage && !owner.StorageBytes && shadow_pool_live==live-1,
         "context teardown releases active reserved capture exactly once");
   WddmReleaseCaptureOwner(&owner);
   check(shadow_pool_live==live-1,"second drain is harmless");
   shadow_pool_fail=0;
  }
 }
 use_retained_walk=0;virtual_graph_map=0;virtual_graph_local_mask=0;memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
 if(owned)VidMmStop();
 check(!shadow_pool_live && !flush_lock_depth && !flush_region_depth,"captured graph storage and engine readers released");
}

static void case_unequal_virtual_alias_mode(unsigned overlap,unsigned captured,unsigned kind,unsigned reverse)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 fence[16];
 static unsigned char memory[7][4096],expected[6][4096],backing[6][4096];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned i,j,k,word=0,balance=0,soStart=reverse?1:0,doStart=reverse?0:1;
 unsigned dmaBytes=kind>=2?(overlap==1?48:16)*4:4096;
 unsigned tableCount=kind==4?3:6,tableFirst=kind==4?4:1;
 unsigned destinationIds[3],sourceIds[3]={0,1,2};u64 root=4096,physical[6];u32 commands[1024],privateData[2048];ULONG written;NTSTATUS status;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=captured==2?256:1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"captured graph window");

 DXGKARG_BUILDPAGINGBUFFER b={0};PAGING_CAPTURE_OWNER owner={0};u64 moved=0,total=0;unsigned pass=0;
 physical[0]=0x100123000ull;physical[1]=0x200020000ull;physical[2]=0x200030000ull;physical[3]=0x400000000ull;physical[4]=0x400001000ull;physical[5]=0x400002000ull;
 for(i=0;i<6;i++)if(kind)physical[i]=(u64)d.VramPhysical.QuadPart+(16ull+i*3)*4096;
 if(kind>=3) {
  u64 app=0,appLength=0,table=0,tableLength=0;
  DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update={0};DXGK_PTE entries[512];
  d.FullWddm=d.VramWriteEnabled=1;cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
  check(WddmMemoryLayout(&d,&app,&appLength,&table,&tableLength) &&
        VidMmStartLayout(&d,app,appLength,1,table,tableLength,3)==STATUS_SUCCESS,"linear table fixture uses actual production segment layout");
  for(i=0;i<tableCount;i++) {
   physical[i]=(u64)d.VramPhysical.QuadPart+table+((u64)tableFirst+i)*4096;
   for(j=0;j<512;j++){entries[j].Flags=BC250_DXGK_PTE_VALID|BC250_DXGK_PTE_CACHECOHERENT;entries[j].PageAddress=0x100+i*512+j;}
   update.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;update.PageTableLevel=0;
   update.PageTableAddress.CpuVirtual=cpu_storage+(tableFirst+i)*512;update.NumPageTableEntries=512;update.pPageTableEntries=entries;
   check(VidMmUpdatePageTable(&update)==STATUS_SUCCESS,"linear table fixture initializes actual known PTE bytes");
  }
  if(kind==4) {
   root=(u64)d.VramPhysical.QuadPart+table+4096;
   update.NumPageTableEntries=1;
   entries[0].Flags=BC250_DXGK_PTE_VALID|(3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
   for(i=1;i<4;i++) {
    update.PageTableAddress.CpuVirtual=cpu_storage+i*512;update.PageTableLevel=4-i;entries[0].PageAddress=i+1;
    check(VidMmUpdatePageTable(&update)==STATUS_SUCCESS,"linear self-table initializes actual hierarchy");
   }
   for(i=0;i<6;i++){entries[i].Flags=BC250_DXGK_PTE_VALID|(3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);entries[i].PageAddress=4+i%3;}
   update.PageTableAddress.CpuVirtual=cpu_storage+4*512;update.PageTableLevel=0;update.StartIndex=16;
   update.NumPageTableEntries=6;
   check(VidMmUpdatePageTable(&update)==STATUS_SUCCESS,"linear self-table initializes shared leaf mappings");
  }
 }
 virtual_graph_map=1;virtual_graph_local_mask=0;
 for(i=0;i<3;i++)destinationIds[i]=overlap==0?i+3:overlap==1?i:overlap==2?i+1:i?i-1:3;
 if(overlap==4){sourceIds[0]=0;sourceIds[1]=0;sourceIds[2]=1;destinationIds[0]=2;destinationIds[1]=3;destinationIds[2]=0;}
 for(i=0;i<6;i++){unsigned id=i<3?sourceIds[i]:destinationIds[i-3];virtual_graph_pages[i]=physical[id];if(kind>=2 || (kind==1 && (id&1)))virtual_graph_local_mask|=1u<<i;}
 if(kind==4) {
  virtual_graph_map=0;use_retained_walk=1;
  for(i=0;i<6;i++){u64 pa=0;BOOLEAN system=TRUE;
   check(VidMmTranslatePaging(root,(16ull+i)*4096,&pa,&system) && !system && pa==physical[i%3],
         "linear self-table actual walk positive control");}
 }
 for(i=0;i<7;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
 if(kind>=3)for(i=0;i<tableCount;i++)memcpy(memory[i],cpu_storage+(tableFirst+i)*512,4096);
 memcpy(backing,memory,sizeof(backing));
 memcpy(expected,memory,sizeof(expected));
 for(i=0;i<8192;i++)expected[destinationIds[(i+doStart)/4096]][(i+doStart)%4096]=memory[sourceIds[(i+soStart)/4096]][(i+soStart)%4096];
 b.TransferVirtual.SourceVirtualAddress=16*4096+soStart;b.TransferVirtual.DestinationVirtualAddress=19*4096+doStart;
 b.TransferVirtual.TransferSizeInBytes=8192;
 if(captured==1) {
  PAGING_GRAPH_CAPTURE* c=NULL;u64 progress=0,next=0;BC250_PAGING_COPY_SLICE slice;
  check(GfxPagingCaptureVirtualGraph(&d,root,b.TransferVirtual.SourceVirtualAddress,
        b.TransferVirtual.DestinationVirtualAddress,8192,&c)==STATUS_SUCCESS && c && c->Linear && c->Backward,
        "unequal capture proves backward traversal using retained physical identities");
  if(!c)goto Cleanup;
  check(c->SourcePageCount==2 && c->DestinationPageCount==3,"unequal capture preserves distinct endpoint page counts");
  virtual_graph_map=0;translationOk=0;
  while(progress<8192) {
   unsigned from=7,to=7,so,doff;
   check(GfxPagingCapturedLinearSlice(c,progress,&slice,&next) && next>progress && !c->Progress,
         "retained reverse slice proposes bounded progress without modifying owner");
   if(next<=progress)break;
   for(i=0;i<6;i++) {
    if((slice.SourcePhysical&~4095ull)==physical[i])from=i;
    if((slice.DestinationPhysical&~4095ull)==physical[i])to=i;
   }
   so=(unsigned)(slice.SourcePhysical&4095);doff=(unsigned)(slice.DestinationPhysical&4095);
   check(from<6 && to<6 && slice.SourceSystem && slice.DestinationSystem &&
         slice.Bytes<=4096-so && slice.Bytes<=4096-doff,"retained unequal slice selects original physical endpoints");
   if(from>=6 || to>=6)break;
   memcpy(memory[6],memory[from]+so,slice.Bytes);memcpy(memory[to]+doff,memory[6],slice.Bytes);
   progress=next;
  }
  check(progress==8192 && !memcmp(memory,expected,sizeof(expected)),"retained unequal copy preserves initial bytes with translation unavailable");
  ExFreePoolWithTag(c,c->Owner.PoolTag);translationOk=1;goto Cleanup;
 }

 if(kind>=3) {
  b.pDmaBuffer=commands;b.DmaSize=dmaBytes;b.pDmaBufferPrivateData=privateData;b.DmaBufferPrivateDataSize=sizeof(privateData);
  if(kind==4) {
   b.DmaSize=0;
   check(WddmBuildCapturedVirtualTransfer(&d,root,&owner,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && owner.Head,
         "linear self-table captures valid actual mappings before publication gate test");
   b.DmaSize=dmaBytes;
  }
  g_VidMm.Write=0;
  check(WddmBuildCapturedVirtualTransfer(&d,root,&owner,&b,&moved)==STATUS_DEVICE_NOT_READY &&
        owner.Head && !((PAGING_GRAPH_CAPTURE*)owner.Head)->Progress && !moved &&
        b.pDmaBuffer==commands && b.pDmaBufferPrivateData==privateData && b.DmaSize==dmaBytes,
        "linear table publication refusal leaves capture and DMA progress unchanged");
  g_VidMm.Write=1;
 }
 do {
  b.pDmaBuffer=commands;b.DmaSize=dmaBytes;b.pDmaBufferPrivateData=privateData;b.DmaBufferPrivateDataSize=sizeof(privateData);
  status=captured==2?WddmBuildCapturedVirtualTransfer(&d,root,&owner,&b,&moved):WddmBuildVirtualTransfer(&d,root,&b,&moved);total+=moved;
  if(captured==2){virtual_graph_map=0;translationOk=0;}
  written=(dmaBytes-b.DmaSize)/4;word=0;balance=0;
  check((status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && written,"unequal alias emits progress");
  for(k=0;word<written;k++) {
    u32* packet=commands+word;unsigned mapped,words,n,from=7,to=7,fromOffset=0,toOffset=0;
    u64 sm,dm,sp=0,dp=0;unsigned sourceSystem,destinationSystem,staged=0;
    if(word>=written)break;
    mapped=packet[0]!=SDMA_PKT_HEADER_OP(SDMA_OP_COPY);words=mapped?83:7;
    check(words<=written-word,"captured packet fits emitted range");if(words>written-word)break;
    if(mapped) {
     sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
     dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
     sm=(u64)packet[36]|((u64)packet[37]<<32);dm=(u64)packet[38]|((u64)packet[39]<<32);n=packet[34]+1;
     check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY),"captured mapped packet includes actual copy");
     if(dm==gfx.PagingCopyStaging.mc) {
      staged=1;words=100;
      check(words<=written-word && packet[50]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && packet[51]+1==n &&
            ((u64)packet[53]|((u64)packet[54]<<32))==gfx.PagingCopyStaging.mc,
            "unequal alias staged transaction has matching save and restore");
      if(words>written-word)break;
      dm=(u64)packet[55]|((u64)packet[56]<<32);
     }
    } else {sm=(u64)packet[3]|((u64)packet[4]<<32);dm=(u64)packet[5]|((u64)packet[6]<<32);n=packet[1]+1;}
    if(!mapped && dm==gfx.PagingCopyStaging.mc) {
     staged=1;words=34;
     check(words<=written-word && packet[17]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && packet[18]+1==n &&
           ((u64)packet[20]|((u64)packet[21]<<32))==gfx.PagingCopyStaging.mc,
           "local staged unequal copy pairs save and restore within one buffer");
     if(words>written-word)break;
     dm=(u64)packet[22]|((u64)packet[23]<<32);
    }
    sourceSystem=sm>=gfx.PagingWindow.mc && sm-gfx.PagingWindow.mc<4096;
    destinationSystem=dm>=gfx.PagingWindow.mc+4096 && dm-gfx.PagingWindow.mc-4096<4096;
    if(sm==gfx.PagingCopyStaging.mc){from=6;check(balance==1,"captured restore has current-batch save");balance--;}
    else {
     u64 pa=sourceSystem?sp+sm-gfx.PagingWindow.mc:sm-d.VramMcBase+(u64)d.VramPhysical.QuadPart;
     fromOffset=(unsigned)(pa&4095);for(i=0;i<6;i++)if((pa&~4095ull)==physical[i])from=i;
    }
    if(dm==gfx.PagingCopyStaging.mc){to=6;balance++;}
    else {
     u64 pa=destinationSystem?dp+dm-gfx.PagingWindow.mc-4096:dm-d.VramMcBase+(u64)d.VramPhysical.QuadPart;
     toOffset=(unsigned)(pa&4095);for(i=0;i<6;i++)if((pa&~4095ull)==physical[i])to=i;
    }
    check(from<7 && to<7 && (staged || from!=to || fromOffset+n<=toOffset || toOffset+n<=fromOffset) && n<=4096-fromOffset && n<=4096-toOffset,
          "captured actual COPY addresses identify bounded original physical bytes");
    if(from>=7 || to>=7 || (!staged && from==to && fromOffset+n>toOffset && toOffset+n>fromOffset) || n>4096-fromOffset || n>4096-toOffset)break;
    check(sourceSystem==(unsigned)(kind==0 || (kind==1 && !(from&1))) &&
          destinationSystem==(unsigned)(kind==0 || (kind==1 && !(to&1))),"unequal packets preserve captured local/system access domains");
    if(staged){memcpy(memory[6],memory[from]+fromOffset,n);memcpy(memory[to]+toOffset,memory[6],n);}
    else memcpy(memory[to]+toOffset,memory[from]+fromOffset,n);word+=words;

  }
  check(word==written && !balance,"unequal alias consumes complete scratch groups");
  if(captured==2)check(PagingPrivateVisit(privateData,(unsigned)(sizeof(privateData)-b.DmaBufferPrivateDataSize),0,written*4,0,NULL,NULL),
                         "owned unequal transfer publishes exact private DMA coverage");
  if(kind>=3)for(i=0;i<tableCount;i++)for(j=0;j<512;j++) {
   u64 value=0,wanted;memcpy(&wanted,memory[i]+j*8,8);
   check(PagingPtShadowRead(&g_VidMm.Shadow,physical[i],j,&value)==PAGING_PT_OK && value==wanted,
         "linear table logical state matches independent DMA replay after each callback");
   check(!memcmp(cpu_storage+(tableFirst+i)*512+j,backing[i]+j*8,8),"linear table command construction preserves live backing bytes");
  }
  if(kind==4 && reverse && !pass) {
   u64 pa=0;BOOLEAN system=TRUE;
   check(!VidMmTranslatePaging(root,16*4096,&pa,&system) || system || pa!=physical[0],
         "linear first accepted buffer changes its own source mapping");
   check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && owner.Head,
         "linear self-table capture must resume after its address map changes");
  }
  pass++;
 }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<16);
 check(status==STATUS_SUCCESS && total==8192,"unequal alias accounts all requested bytes");
 check(!memcmp(memory,expected,sizeof(expected)),"unequal alias matches original byte snapshot across physical page boundary");
 if(captured==2)check(pass>1 && !owner.Head && b.MultipassOffset==PAGING_CAPTURE_COMPLETE,
                     "owned unequal transfer resumes then releases captured metadata");
Cleanup:
 if(kind>=3){VidMmStop();check(!shadow_pool_live && !cpu_map_live,"linear table fixture releases capture and table resources");}
 translationOk=1;use_retained_walk=0;virtual_graph_map=0;virtual_graph_local_mask=0;memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_unequal_virtual_alias(unsigned overlap,unsigned captured)
{
 unsigned kind,reverse;
 if(captured!=2){case_unequal_virtual_alias_mode(overlap,captured,0,0);return;}
 for(kind=0;kind<4;kind++)for(reverse=0;reverse<2;reverse++)
  case_unequal_virtual_alias_mode(overlap,captured,kind,reverse);
 if(overlap==1)for(reverse=0;reverse<2;reverse++)case_unequal_virtual_alias_mode(overlap,captured,4,reverse);
}

static void case_monotone_alias_direction(void)
{
 static unsigned char memory[6][4096],initial[6][4096],expected[6][4096],scratch[4096];
 const unsigned offsets[4]={0,1,17,4095},sources[5][3]={{4,0,2},{4,4,2},{4,0,4},{4,4,4},{0,2,2}};
 const unsigned* src=sources[0];
 unsigned dst[3],work[12],a,b,disjoint,i,j,variant,accepted=0;
 for(variant=0;variant<5;variant++)for(disjoint=0;disjoint<218;disjoint++)for(a=0;a<4;a++)for(b=0;b<4;b++) {
  unsigned so=offsets[a],dof=offsets[b],bytes=8192,done=0;
  unsigned sn=(so+bytes+4095)/4096,dn=(dof+bytes+4095)/4096;int backward=0;
  src=sources[variant];
  if(disjoint<2)for(i=0;i<3;i++)dst[i]=disjoint?i*2+1:src[i];
  else {unsigned code=disjoint-2;for(i=0;i<3;i++){dst[i]=code%6;code/=6;}}
  if(!PagingPageAliasDirection(src,sn,dst,dn,6,work,so,dof,&backward)) {
   check(variant || disjoint>=2,"original monotone controls remain accepted");continue;
  }
  accepted++;
  for(i=0;i<6;i++)for(j=0;j<4096;j++)initial[i][j]=(unsigned char)(i*43+j*13+j/127);
  memcpy(memory,initial,sizeof(memory));memcpy(expected,initial,sizeof(expected));
  for(i=0;i<bytes;i++)expected[dst[(dof+i)/4096]][(dof+i)%4096]=initial[src[(so+i)/4096]][(so+i)%4096];
  check(PagingPageAliasDirection(src,sn,dst,dn,6,work,so,dof,&backward),"monotone classifier accepts fragmented physical identities");
  while(done<bytes) {
   unsigned pos=backward?bytes-done:done,n,sp,dp;
   if(backward) {
    unsigned sr=(so+pos-1)%4096+1,dr=(dof+pos-1)%4096+1;
    n=sr<dr?sr:dr;if(n>bytes-done)n=bytes-done;pos-=n;
   } else {
    unsigned sr=4096-(so+pos)%4096,dr=4096-(dof+pos)%4096;
    n=sr<dr?sr:dr;if(n>bytes-done)n=bytes-done;
   }
   sp=so+pos;dp=dof+pos;
   memcpy(scratch,memory[src[sp/4096]]+sp%4096,n);
   memcpy(memory[dst[dp/4096]]+dp%4096,scratch,n);done+=n;
  }
  check(!memcmp(memory,expected,sizeof(memory)),"proven monotone order preserves original bytes for both shift directions");
 }
 check(accepted>32,"classifier exercises additional physical page-position dependencies");
 printf("monotone interval direction: %u accepted full-memory snapshot fixtures\n",accepted);
 src=sources[0];
 {int backward;unsigned shifted[3]={0,2,5},opposite[3]={5,4,0};
  check(PagingPageAliasDirection(src,3,shifted,3,6,work,0,1,&backward) && backward,
        "later source page overwritten by earlier destination requires backward order");
  check(PagingPageAliasDirection(src,3,opposite,3,6,work,0,1,&backward) && !backward,
        "page-position dependency overrides positive byte-offset shift");
 }
 {int backward;unsigned cycle[3]={0,2,4};
  check(!PagingPageAliasDirection(src,3,cycle,3,6,work,0,1,&backward),"cyclic physical mapping remains for general dependency planner");}
}

static void case_context_capture_admission(void)
{
 BC250_DEVICE d={0};BC250_WDDM w={0};BC250_WDDM_OBJECT* object;unsigned mode;
 d.Wddm=&w;
 for(mode=0;mode<5;mode++) {
  int live=shadow_pool_live;
  w.Objects.next=w.Objects.prev=&w.Objects;w.ObjectCount=0;w.Stopping=mode==4;
  context_publications=context_reserved_publications=0;shadow_pool_calls=0;
  shadow_pool_fail_at=mode==2?1:mode==3?2:0;
  object=WddmNewContext(&d,mode!=0);
  if(mode<2) {
   check(object && w.ObjectCount==1 && context_publications==1 && !context_lock_depth,
         "context admission publishes exactly one prepared object");
   if(object) {
    check(context_reserved_publications==(mode==1) &&
          !!object->Captures.Storage==(mode==1) && shadow_pool_live==live+1+(mode==1),
          "only system context publishes with an already-owned reservation");
    WddmReleaseCaptureOwner(&object->Captures);ExFreePoolWithTag(object,BC250_WDDM_TAG);
   }
  } else {
   check(!object && !w.ObjectCount && !context_publications && !context_lock_depth,
         "reservation/object/admission refusal publishes no context");
  }
  check(shadow_pool_live==live,"context admission transfers or releases every allocation");
 }
 shadow_pool_fail_at=0;
}
