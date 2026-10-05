#include "paging_intervals.h"
unsigned PagingIntervalCapacity(PAGING_U64 address,PAGING_U64 bytes)
{
    PAGING_U64 count;
    if(!bytes || address>~0ull-bytes)return 0;
    count=bytes/4096+((bytes%4096+(address&4095)+4095)/4096);
    return count<=0xffffffffull?(unsigned)count:0;
}
static void Swap(PAGING_INTERVAL* a,PAGING_INTERVAL* b){PAGING_INTERVAL t=*a;*a=*b;*b=t;}
static void Sift(PAGING_INTERVAL* a,unsigned root,unsigned count)
{
    while(count>1 && root<=(count-2)/2) {
        unsigned child=root*2+1;
        if(child+1<count && a[child].begin<a[child+1].begin)child++;
        if(a[root].begin>=a[child].begin)break;
        Swap(a+root,a+child);root=child;
    }
}
int PagingIntervalsDisjoint(void* context,PAGING_RESOLVE source,PAGING_RESOLVE destination,
    PAGING_U64 src,PAGING_U64 dst,PAGING_U64 bytes,PAGING_INTERVAL* work,unsigned capacity,int* disjoint)
{
    PAGING_U64 progress=0,physical;unsigned n=0,i,merged=0,needed=PagingIntervalCapacity(src,bytes);
    if(!disjoint)return 0;
    *disjoint=0;
    if(!source || !destination || !work || !needed || capacity<needed || !PagingIntervalCapacity(dst,bytes))return 0;
    while(progress<bytes) {
        unsigned count=4096-(unsigned)((src+progress)&4095);
        if(count>bytes-progress)count=(unsigned)(bytes-progress);
        if(!source(context,src+progress,count,&physical) || physical>~0ull-count)return 0;
        work[n].begin=physical;work[n++].end=physical+count;progress+=count;
    }
    for(i=n/2;i;i--)Sift(work,i-1,n);
    for(i=n;i>1;i--){Swap(work,work+i-1);Sift(work,0,i-1);}
    for(i=0;i<n;i++) {
        if(merged && work[i].begin<=work[merged-1].end) {
            if(work[i].end>work[merged-1].end)work[merged-1].end=work[i].end;
        } else work[merged++]=work[i];
    }
    progress=0;
    while(progress<bytes) {
        unsigned lo=0,hi=merged,count=4096-(unsigned)((dst+progress)&4095);
        if(count>bytes-progress)count=(unsigned)(bytes-progress);
        if(!destination(context,dst+progress,count,&physical) || physical>~0ull-count)return 0;
        while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(work[mid].end<=physical)lo=mid+1;else hi=mid;}
        if(lo<merged && work[lo].begin<physical+count)return 1;
        progress+=count;
    }
    *disjoint=1;return 1;
}
