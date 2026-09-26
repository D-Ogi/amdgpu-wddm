#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../driver/kmd/paging_intervals.h"
typedef struct {PAGING_U64 src[5],dst[5];unsigned calls;int large;} TEST;
static int Resolve(TEST*t,PAGING_U64 a,unsigned bytes,PAGING_U64*out,int dst)
{
 unsigned p=(unsigned)(a/4096);(void)bytes;t->calls++;
 if(t->large)*out=((PAGING_U64)((p*17u)&262143u)+(dst?1048576ull:0))*4096+(a&4095);
 else {if(p>=5)return 0;*out=(dst?t->dst[p]:t->src[p])*4096+(a&4095);}
 return 1;
}
static int Src(void*c,PAGING_U64 a,unsigned b,PAGING_U64*p){return Resolve(c,a,b,p,0);}
static int Dst(void*c,PAGING_U64 a,unsigned b,PAGING_U64*p){return Resolve(c,a,b,p,1);}
static int Oracle(TEST*t,PAGING_U64 src,PAGING_U64 dst,PAGING_U64 bytes)
{
 PAGING_U64 i=0,j,a,b;
 while(i<bytes){unsigned x=4096-(unsigned)((src+i)&4095);if(x>bytes-i)x=(unsigned)(bytes-i);Src(t,src+i,x,&a);
  j=0;while(j<bytes){unsigned y=4096-(unsigned)((dst+j)&4095);if(y>bytes-j)y=(unsigned)(bytes-j);Dst(t,dst+j,y,&b);
   if(a<b+y && b<a+x)return 0;j+=y;}i+=x;}
 return 1;
}
int main(void)
{
 TEST t={0};PAGING_INTERVAL small[5],*large;unsigned seed=17,n,k;int got,want;clock_t begin;
 for(n=0;n<5000;n++) {
  PAGING_U64 src,dst,bytes;
  for(k=0;k<5;k++){seed=seed*1664525u+1013904223u;t.src[k]=(seed>>12)%13;seed=seed*1664525u+1013904223u;t.dst[k]=(seed>>12)%13;}
  seed=seed*1664525u+1013904223u;src=(seed>>12)&4095;dst=(seed>>24)*16;
  bytes=1+(seed%12288);want=Oracle(&t,src,dst,bytes);
  if(!PagingIntervalsDisjoint(&t,Src,Dst,src,dst,bytes,small,5,&got)||got!=want){printf("FAIL case%u\n",n);return 1;}
 }
 large=(PAGING_INTERVAL*)malloc(262144*sizeof(*large));if(!large)return 2;
 t.large=1;t.calls=0;begin=clock();
 if(!PagingIntervalsDisjoint(&t,Src,Dst,0,0,1ull<<30,large,262144,&got)||!got||t.calls!=524288)return 1;
 printf("PASS 5000 independent overlap-oracle cases;1GiB spans,524288resolver calls,%.3fs\n",(double)(clock()-begin)/CLOCKS_PER_SEC);
 free(large);return 0;
}
