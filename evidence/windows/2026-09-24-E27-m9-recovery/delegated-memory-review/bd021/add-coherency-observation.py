from pathlib import Path
r=Path('bc250-win')
p=r/'driver/kmd/vidmm.c'
s=p.read_text()
s=s.replace('    volatile LONG64 EncodedCoherentSystem, EncodedUncachedSystem, EncodedCoherencyMismatch;', '''    // Encoding attempts, not unique pages or GPU retirement. Domains: system,
    // application local, table local. Aggregate once per callback/slice below.
    volatile LONG64 EncodedCoherent[BC250_VIDMM_LEVELS][3];
    volatile LONG64 EncodedNoncoherent[BC250_VIDMM_LEVELS][3];
    volatile LONG64 EncodedSnoopMismatch[BC250_VIDMM_LEVELS][3];''')
marker='// PASSIVE_LEVEL. Immediate CPU_VIRTUAL initialization, or the serialized'
helper='''// Count only completely validated encodings. Repeat expands to the number of
// output entries; GPU slices count only their emitted subset. Re-encoding at
// logical publication can count again, so these are observations, not residency.
// Keep interlocked work per nonempty bucket, not per PTE. No cache policy changes.
static void VidMmCountEncoding(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                               ULONG Start, ULONG Count, const ULONGLONG* Entries)
{
    BC250_VIDMM* vm=&g_VidMm;
    LONG64 coherent[3]={0},noncoherent[3]={0},mismatch[3]={0};
    ULONG i,kind,level=Update->PageTableLevel;
    for(i=0;i<Count;i++) {
        const DXGK_PTE* pte=Update->pPageTableEntries+(Update->Flags.Repeat?0:Start+i);
        ULONG segment=(ULONG)((pte->Flags&BC250_DXGK_PTE_SEGMENT_MASK)>>BC250_DXGK_PTE_SEGMENT_SHIFT);
        BOOLEAN requested;
        if(!(pte->Flags&BC250_DXGK_PTE_VALID)) continue;
        kind=segment==vm->Pte.system_segment?0u:
             (vm->Pte.table_size && segment==vm->Pte.table_segment?2u:1u);
        requested=(BOOLEAN)((pte->Flags&BC250_DXGK_PTE_CACHECOHERENT)!=0);
        if(requested)coherent[kind]++;else noncoherent[kind]++;
        if(requested!=((Entries[i]&AMDGPU_PTE_SNOOPED)!=0))mismatch[kind]++;
    }
    for(kind=0;kind<3;kind++) {
        if(coherent[kind])InterlockedAdd64(&vm->EncodedCoherent[level][kind],coherent[kind]);
        if(noncoherent[kind])InterlockedAdd64(&vm->EncodedNoncoherent[level][kind],noncoherent[kind]);
        if(mismatch[kind])InterlockedAdd64(&vm->EncodedSnoopMismatch[level][kind],mismatch[kind]);
    }
}

'''
assert s.count(marker)==1
s=s.replace(marker,helper+marker)
old='''        if (NT_SUCCESS(status)) {
            if (cpu) table = (volatile ULONGLONG*)Update->PageTableAddress.CpuVirtual;'''
new='''        if (NT_SUCCESS(status)) {
            VidMmCountEncoding(Update,0,Update->NumPageTableEntries,entries);
            if (cpu) table = (volatile ULONGLONG*)Update->PageTableAddress.CpuVirtual;'''
assert s.count(old)==1;s=s.replace(old,new)
a=s.index('    // Diagnostics count successful encoding attempts, including re-encoding at')
b=s.index('    *Physical = table',a)
s=s[:a]+'    VidMmCountEncoding(Update,Start,Count,Entries);\n'+s[b:]
old='''    GuardLog("vidmm summary: system leaf encoding coherent %lld noncoherent %lld snoop mismatches %lld",
             vm->EncodedCoherentSystem,vm->EncodedUncachedSystem,vm->EncodedCoherencyMismatch);'''
new='''    // Sampled totals may grow during this summary; they are not a synchronized
    // snapshot. Segment IDs preserve the OS input identity (including local tables).
    for(level=0;level<BC250_VIDMM_LEVELS;level++) {
        UINT kind;
        for(kind=0;kind<3;kind++) {
            LONG64 coherent=InterlockedCompareExchange64(&vm->EncodedCoherent[level][kind],0,0);
            LONG64 noncoherent=InterlockedCompareExchange64(&vm->EncodedNoncoherent[level][kind],0,0);
            LONG64 mismatch=InterlockedCompareExchange64(&vm->EncodedSnoopMismatch[level][kind],0,0);
            if(!coherent && !noncoherent && !mismatch)continue;
            segment=kind==0?vm->Pte.system_segment:(kind==1?vm->Pte.vram_segment:vm->Pte.table_segment);
            GuardLog("vidmm summary: PTE encoding level %u segment %u coherent %lld noncoherent %lld snoop mismatches %lld",
                     level,segment,coherent,noncoherent,mismatch);
        }
    }'''
assert s.count(old)==1;s=s.replace(old,new);p.write_text(s)
p=r/'experiments/E27-m9-inference/generate-paging-route-test.py';s=p.read_text();s=s.replace("a=vm.index('static NTSTATUS VidMmUpdatePageTableLocked(');", "a=vm.index('static void VidMmCountEncoding(');b=vm.index('// PASSIVE_LEVEL. Immediate CPU_VIRTUAL',a)\nencoder=vm[a:b]+encoder\na=vm.index('static NTSTATUS VidMmUpdatePageTableLocked(');");p.write_text(s)
p=r/'experiments/E27-m9-inference/paging-route-test-prefix.c';s=p.read_text();s=s.replace('Written,EncodedCoherentSystem,EncodedUncachedSystem,EncodedCoherencyMismatch;', 'Written,EncodedCoherent[4][3],EncodedNoncoherent[4][3],EncodedSnoopMismatch[4][3];');p.write_text(s)
p=r/'experiments/E27-m9-inference/paging-route-test-suffix.c';s=p.read_text();s=s.replace('g_VidMm.EncodedCoherentSystem','g_VidMm.EncodedCoherent[0][0]').replace('g_VidMm.EncodedUncachedSystem','g_VidMm.EncodedNoncoherent[0][0]').replace('g_VidMm.EncodedCoherencyMismatch','g_VidMm.EncodedSnoopMismatch[0][0]');p.write_text(s)
