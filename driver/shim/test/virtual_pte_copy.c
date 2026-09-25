/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Actual emitters, independent byte interpreter and memmove oracle.
 * The VA map changes AFTER construction. This is a host control, not proof
 * of GPU TLB behavior, OS root ownership or DMA residency on the lab. */
#define main paging_regression_main
#include "paging_packets.c"
#undef main
#include "bc250_sdma_virtual_ptes.h"

struct mapping { u64 va; unsigned bytes; unsigned char *data; };
static struct mapping maps[3];
static unsigned char *resolve(u64 va, unsigned bytes)
{
    unsigned i;
    for (i=0;i<3;i++)
        if (va>=maps[i].va && va-maps[i].va<=maps[i].bytes &&
            bytes<=maps[i].bytes-(unsigned)(va-maps[i].va))
            return maps[i].data+(size_t)(va-maps[i].va);
    fprintf(stderr,"Unmapped model address\n");exit(2);
}
static u64 address(const u32 *p) { return (u64)p[0] | ((u64)p[1]<<32); }
static int execute(const u32 *ib, unsigned words)
{
    unsigned at=0;
    while(at<words) {
        unsigned op=ib[at]&255u;
        if(op==SDMA_OP_NOP) { ++at;continue; }
        if(op==SDMA_OP_COPY && words-at>=7) {
            unsigned bytes=ib[at+1]+1;
            memcpy(resolve(address(ib+at+5),bytes),resolve(address(ib+at+3),bytes),bytes);
            at+=7;
        } else if(op==SDMA_OP_FENCE && words-at>=4) {
            memcpy(resolve(address(ib+at+1),4),ib+at+3,4);at+=4;
        } else if(op==SDMA_OP_POLL_REGMEM && words-at>=6) {
            u32 value;
            memcpy(&value,resolve(address(ib+at+1),4),4);
            if((value&ib[at+4])!=(ib[at+3]&ib[at+4]))return 0;
            at+=6;
        } else return 0;
    }
    return 1;
}
static void late_binding(void)
{
    static unsigned char buffer[16384],old[8192],live[8192],expected[8192];
    const u64 dma=0x100000000ull,src=0x200000000ull,dst=0x300000000ull;
    const unsigned offsets[]={0,1,63,255,511},counts[]={1,2,64,256,512};
    unsigned a,b,c,alias,alignment,i,cases=0;
    struct bc250_sdma_virtual_ptes built;
    for(alignment=0;alignment<4096;alignment+=4) {
        memset(buffer,0xCC,sizeof(buffer));
        check(bc250_sdma_build_virtual_ptes(&g_adev,buffer,sizeof(buffer),dma+alignment,
            src,dst,512,&built)==0,"every DWORD-aligned DMA start admits full table copy");
        check(!((dma+alignment+built.csa_offset)&63u) &&
              !((dma+alignment+built.ib_offset)&31u) &&
              !((dma+alignment+built.staging_offset)&4095u) &&
              !(built.ib_dwords&7u),"CSA IB staging and length alignment");
        check(built.marker_offset>=built.csa_offset+64 &&
              built.ib_offset>=built.marker_offset+4 &&
              built.staging_offset>=built.ib_offset+built.ib_dwords*4 &&
              built.bytes==built.staging_offset+4096,"owned writable regions stay disjoint");
        check(all_pattern((u32*)(buffer+built.bytes),(sizeof(buffer)-built.bytes)/4,0xCCCCCCCCu),
              "DMA tail stays untouched");
    }
    for(alias=0;alias<2;alias++)for(a=0;a<5;a++)for(b=0;b<5;b++)for(c=0;c<5;c++) {
        unsigned count=counts[c],from=offsets[a]*8,to=offsets[b]*8;
        unsigned required;
        if(count>512-offsets[a] || count>512-offsets[b])continue;
        for(i=0;i<sizeof(old);i++){old[i]=(unsigned char)(i*7+11);live[i]=(unsigned char)(i*19+i/17+3);}
        memcpy(expected,live,sizeof(live));
        memmove(expected+(alias?0:4096)+to,expected+from,count*8);
        /* Build against the old mapping, then relocate BOTH endpoints.
         * The emitter cannot inspect either map: only addresses enter it. */
        maps[0].va=dma+12;maps[0].bytes=sizeof(buffer);maps[0].data=buffer;
        maps[1].va=src;maps[1].bytes=4096;maps[1].data=old;
        maps[2].va=dst;maps[2].bytes=4096;maps[2].data=old+(alias?0:4096);
        check(bc250_sdma_build_virtual_ptes(&g_adev,buffer,sizeof(buffer),dma+12,
            src+from,dst+to,count,&built)==0,"build with unresolved table VAs");
        required=built.bytes;
        maps[1].data=live;maps[2].data=live+(alias?0:4096);
        check(execute((u32*)(buffer+built.ib_offset),built.ib_dwords),"ordered two-copy interpreter completes");
        check(memcmp(live,expected,sizeof(live))==0,"relocated and aliasing tables match memmove oracle");
        for(i=0;i<sizeof(old);i++)if(old[i]!=(unsigned char)(i*7+11))break;
        check(i==sizeof(old),"old backing remains untouched");
        /* Positive sensitivity: a construction-time physical source would
         * still supply the old bytes, which differ from the expected result. */
        check(memcmp(old+from,expected+(alias?0:4096)+to,count*8)!=0,
              "stale physical capture would fail this oracle");
        memset(buffer,0xCC,sizeof(buffer));
        check(bc250_sdma_build_virtual_ptes(&g_adev,buffer,required-1,dma+12,
            src+from,dst+to,count,&built)==BC250_SDMA_PAGING_INSUFFICIENT &&
            built.bytes==required && all_pattern((u32*)buffer,sizeof(buffer)/4,0xCCCCCCCCu),
            "multipass capacity refusal writes no partial operation");
        ++cases;
    }
    printf("Virtual PTE copy: %u relocated/alias cases; 1024 DMA start alignments\n",cases);
}
int main(void)
{
    if(paging_regression_main(0,NULL))return 1;
    late_binding();
    printf("Combined: %u checks, %u failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
