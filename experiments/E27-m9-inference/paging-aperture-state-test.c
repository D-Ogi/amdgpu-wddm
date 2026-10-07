#include <stdio.h>
#include <stdlib.h>
#include "../../driver/kmd/paging_aperture_state.h"
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);return 1;}} while(0)
int main(void)
{
    unsigned i;unsigned long long physical,alias;
    unsigned long long pages[256],*storage=(unsigned long long*)calloc(PAGING_APERTURE_MIN_BYTES/4096,8);
    PAGING_APERTURE aperture;PAGING_APERTURE_STATE state={0};
    CHECK(storage!=NULL);
    CHECK(PagingApertureInit(0x100000000ull,1ull<<30,0x200000000ull,2ull<<20,PAGING_APERTURE_MIN_BYTES,&aperture));
    CHECK(PagingApertureStateInit(&state,&aperture,storage,(unsigned)(PAGING_APERTURE_MIN_BYTES/4096)));
    CHECK(!PagingApertureStateResolve(&state,aperture.mc,4,&physical));
    // Fragmented pages and page0 survive input storage reuse; nothing borrows PFNs.
    pages[0]=0x91000;pages[1]=0;pages[2]=0x37000;
    CHECK(PagingApertureStateMap(&state,2,3,pages,~4095ull));
    pages[0]=0xDE000;
    CHECK(PagingApertureStateResolve(&state,aperture.mc+2*4096+12,4,&physical)&&physical==0x9100c);
    CHECK(PagingApertureStateResolve(&state,aperture.mc+3*4096,4096,&physical)&&physical==0);
    CHECK(PagingApertureStateResolve(&state,aperture.mc+4*4096,4096,&physical)&&physical==0x37000);
    // Remap accepted before any hardware execution resolves to planned new pages.
    pages[0]=0x57000;pages[1]=0x91000;
    CHECK(PagingApertureStateMap(&state,2,2,pages,~4095ull));
    CHECK(PagingApertureStateResolve(&state,aperture.mc+2*4096,4096,&physical)&&physical==0x57000);
    CHECK(PagingApertureStateMap(&state,20,1,pages,~4095ull));
    CHECK(PagingApertureStateResolve(&state,aperture.mc+20*4096,4096,&alias)&&alias==physical);
    // Multipass: complete256-page batches at both ends of the advertised aperture.
    for(i=0;i<256;i++)pages[i]=(unsigned long long)(i*7+1)*4096;
    CHECK(PagingApertureStateMap(&state,1000,256,pages,~4095ull));
    CHECK(PagingApertureStateMap(&state,state.count-256,256,pages,~4095ull));
    for(i=0;i<256;i++) {
        CHECK(PagingApertureStateResolve(&state,aperture.mc+(1000ull+i)*4096+4092,4,&physical)&&physical==pages[i]+4092);
        CHECK(PagingApertureStateResolve(&state,aperture.mc+(state.count-256ull+i)*4096,4096,&physical)&&physical==pages[i]);
    }
    // Refused input cannot change a previously published prefix.
    pages[0]=0x77000;pages[1]=0x88123;
    CHECK(!PagingApertureStateMap(&state,2,2,pages,~4095ull));
    CHECK(PagingApertureStateResolve(&state,aperture.mc+2*4096,4,&physical)&&physical==0x57000);
    CHECK(!PagingApertureStateResolve(&state,aperture.mc+4095,2,&physical));
    CHECK(!PagingApertureStateResolve(&state,aperture.mc+aperture.bytes,4,&physical));
    CHECK(PagingApertureStateUnmap(&state,2,3));
    CHECK(!PagingApertureStateResolve(&state,aperture.mc+2*4096,4,&physical));
    CHECK(PagingApertureStateResolve(&state,aperture.mc+20*4096,4,&physical)&&physical==0x57000);
    free(storage);printf("PASS %u aperture logical-state checks\n",checks);return 0;
}
