
int main(void) {
    static unsigned char dma[65536];
    __declspec(align(8)) static unsigned privateData[32768];
    DXGK_BUILDPAGINGBUFFER_COPY_RANGE ranges[3]={{512,0x200000000ull,0x300000000ull,0,0},
        {128,0x200010000ull,0x300010000ull,64,128},{1,0x200020000ull,0x300020000ull,511,511}};
    struct amdgpu_ring ring={0};
    BC250_GFX gfx={1,&ring,&g_adev};
    BC250_WDDM wddm={0};
    BC250_DEVICE device={&wddm,&gfx,0};
    DXGKARG_BUILDPAGINGBUFFER build={0};
    unsigned mode;
    ring.adev=&g_adev;ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=16384;
    for(mode=0;mode<2;mode++) {
        unsigned consumed,firstBytes;u32* ib;
        memset(dma,0xCC,sizeof(dma));memset(privateData,0xCC,sizeof(privateData));
        memset(&build,0,sizeof(build));modelTracked=(int)mode;translations=commits=0;
        build.pDmaBuffer=dma;build.DmaSize=16372;
        build.pDmaBufferPrivateData=privateData;build.DmaBufferPrivateDataSize=sizeof(privateData);
        build.DmaBufferGpuVirtualAddress=0x100000000ull;build.DmaBufferWriteOffset=12;
        build.CopyPageTableEntries.NumRanges=3;build.CopyPageTableEntries.pRanges=ranges;
        check(WddmBuildNativePagingCopies(&device,0x100000,&build)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,
            "two whole ranges fit before multipass continuation");
        check(build.MultipassOffset==2 && build.DmaBufferWriteOffset==12 && build.DmaSize==0,
            "accepted range progress and caller input offset are distinct");
        consumed=(unsigned)((unsigned char*)build.pDmaBuffer-dma);
        check(consumed==16372 && build.pDmaBufferPrivateData==privateData+64,"exact DMA and private ownership advanced");
        firstBytes=privateData[1];ib=(u32*)(dma+privateData[8]);
        check(address(ib+3)==0x200000000ull && address(ib+22)==0x300000000ull,
            "first copy source and destination remain original virtual operands");
        ib=(u32*)(dma+firstBytes+privateData[32+8]);
        check(address(ib+3)==ranges[1].SrcPageTableAddress+512 &&
              address(ib+22)==ranges[1].DstPageTableAddress+1024,"second range retains independent indexes");
        check(commits==(modelTracked?2:0) && translations==(modelTracked?6:0),
            "only capacity-accepted ranges commit metadata; untracked root never walks");
        check(PagingPrivateBindRoot(privateData,256,0x10000000Cull,consumed,0x98765000),
            "actual DDI records rebind at virtual submission");
        check(privateData[6]==0x98765000 && privateData[38]==0x98765000,"both accepted IB roots rebind");
        // Resume the third range in a fresh buffer after the first really retires.
        build.pDmaBuffer=dma;build.DmaSize=sizeof(dma);build.DmaBufferWriteOffset=0;
        build.pDmaBufferPrivateData=privateData;build.DmaBufferPrivateDataSize=sizeof(privateData);
        check(WddmBuildNativePagingCopies(&device,0x200000,&build)==STATUS_SUCCESS &&
              build.MultipassOffset==3 && build.DmaBufferWriteOffset==0,"whole-range multipass completes without replaying prefix");
        check(commits==(modelTracked?3:0),"logical publication counts each accepted range once");
        ib=(u32*)(dma+privateData[8]);
        check(address(ib+3)==ranges[2].SrcPageTableAddress+4088 &&
              address(ib+22)==ranges[2].DstPageTableAddress+4088,"last PTE retains byte-exact table offsets");
    }
    printf("Native PTE DDI route: %u checks, %u failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
