#include "paging_aperture_state.h"

static int StateRange(const PAGING_APERTURE_STATE* s,unsigned first,unsigned count)
{
    return s && s->entries && s->count==PAGING_APERTURE_BYTES/4096 && count &&
           first<s->count && count<=s->count-first;
}

int PagingApertureStateInit(PAGING_APERTURE_STATE* s,const PAGING_APERTURE* aperture,
                           unsigned long long* storage,unsigned storagePages)
{
    unsigned i,pages=(unsigned)(PAGING_APERTURE_BYTES/4096);
    unsigned long long mc,table;
    if(!s || !storage || storagePages<pages ||
       !PagingApertureRange(aperture,0,pages,&mc,&table))return 0;
    // The caller supplies separate storage; initialization never allocates or maps.
    for(i=0;i<pages;i++)storage[i]=0;
    s->aperture=*aperture;s->entries=storage;s->count=pages;
    return 1;
}

int PagingApertureStateMap(PAGING_APERTURE_STATE* s,unsigned first,unsigned count,
                          const unsigned long long* pages,unsigned long long addressMask)
{
    unsigned i;
    if(!StateRange(s,first,count) || count>PAGING_APERTURE_BATCH_PAGES || !pages)return 0;
    // Validate the complete accepted batch before changing any logical mapping.
    for(i=0;i<count;i++)if((pages[i]&4095) || (pages[i]&~addressMask))return 0;
    for(i=0;i<count;i++)s->entries[first+i]=pages[i]|1ull;
    return 1;
}

int PagingApertureStateUnmap(PAGING_APERTURE_STATE* s,unsigned first,unsigned count)
{
    unsigned i;
    if(!StateRange(s,first,count))return 0;
    for(i=0;i<count;i++)s->entries[first+i]=0;
    return 1;
}

int PagingApertureStateResolve(const PAGING_APERTURE_STATE* s,unsigned long long mc,
                              unsigned bytes,unsigned long long* physical)
{
    unsigned long long offset,entry;
    unsigned page;
    if(!physical)return 0;
    *physical=0;
    if(!s || !StateRange(s,0,1) || mc<s->aperture.mc)return 0;
    offset=mc-s->aperture.mc;
    if(offset>=s->aperture.bytes || !bytes || bytes>4096-(offset&4095))return 0;
    page=(unsigned)(offset/4096);entry=s->entries[page];
    if(!(entry&1ull))return 0;
    *physical=(entry&~4095ull)+(offset&4095);
    return 1;
}
