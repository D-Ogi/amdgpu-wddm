/* CPU-only comparison of per-band and shared identity normalization.
 * Both paths plan validation + emission bands; compare semantic physical moves.
 * This measures planner CPU work, not KMD callback or GPU throughput. */
#include "../paging_permutation.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static unsigned long long mix(unsigned long long h,unsigned long long x)
{return (h^x)*1099511628211ull;}
static int run(unsigned n,int shared,double* seconds,unsigned long long* digest)
{
    unsigned char* mem=(unsigned char*)malloc((size_t)n*128);
    unsigned long long *src,*dst,*physical,h=1469598103934665603ull;
    PAGING_PAGE_IDENTITY* ids;
    PAGING_PAGE_MOVE* moves;
    unsigned *si,*di,*readers,*writer,*queue,*forward;
    PAGING_PAGE_BAND bands[3];
    unsigned i,b,pass,k,count=0,identities=0,written=0;
    clock_t begin;
    if(!mem)return 0;
    src=(unsigned long long*)mem;dst=src+n;ids=(PAGING_PAGE_IDENTITY*)(dst+n);
    physical=(unsigned long long*)(ids+2*n);si=(unsigned*)(physical+2*n);di=si+n;
    readers=di+n;writer=readers+2*n;queue=writer+2*n;forward=queue+2*n;
    moves=(PAGING_PAGE_MOVE*)(forward+2*n);
    for(i=0;i<n;i++)src[i]=0x100000000ull+((unsigned long long)((i*17u)%n)<<12);
    for(i=0;i<n;i++)dst[i]=src[(i+1)%n];
    if(!PagingPageBands(17,(unsigned long long)(n-1)*4096+17,bands,&count) || count!=3){free(mem);return 0;}
    begin=clock();
    if(shared && !PagingPageGraphNormalize(src,dst,n,ids,physical,si,di,&identities)){free(mem);return 0;}
    for(pass=0;pass<2;pass++)for(b=0;b<count;b++) {
        unsigned first=(unsigned)bands[b].FirstPage,len=(unsigned)bands[b].PageCount;
        if(!shared && !PagingPageGraphNormalize(src+first,dst+first,len,ids,physical,si,di,&identities)){free(mem);return 0;}
        if(!PagingPageGraphPlan(si+(shared?first:0),di+(shared?first:0),len,identities,
            readers,writer,queue,forward,moves,3*n,8,&written)){free(mem);return 0;}
        h=mix(h,written);h=mix(h,bands[b].Offset);h=mix(h,bands[b].Bytes);
        for(k=0;k<written;k++) {
            h=mix(h,moves[k].source==PAGING_PERMUTATION_SCRATCH?~0ull:physical[moves[k].source]);
            h=mix(h,moves[k].destination==PAGING_PERMUTATION_SCRATCH?~0ull:physical[moves[k].destination]);
        }
    }
    *seconds=(double)(clock()-begin)/CLOCKS_PER_SEC;*digest=h;
    free(mem);return 1;
}
int main(void)
{
    unsigned sizes[]={32,4096,262144},s,trial;
    for(s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++)for(trial=0;trial<3;trial++) {
        double oldTime=0,newTime=0;unsigned long long oldHash=0,newHash=0;
        /* Alternate order to reduce consistent first-run cache bias. */
        if(trial&1) {
            if(!run(sizes[s],1,&newTime,&newHash)||!run(sizes[s],0,&oldTime,&oldHash))return 2;
        } else if(!run(sizes[s],0,&oldTime,&oldHash)||!run(sizes[s],1,&newTime,&newHash))return 2;
        printf("pages=%u trial=%u per_band_s=%.6f shared_s=%.6f semantic_match=%s hash=%llx\n",
            sizes[s],trial,oldTime,newTime,oldHash==newHash?"yes":"NO",newHash);
        if(oldHash!=newHash)return 1;
    }
    return 0;
}
