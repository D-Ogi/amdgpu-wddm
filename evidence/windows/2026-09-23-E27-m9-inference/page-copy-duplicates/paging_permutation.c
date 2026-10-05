#include "paging_permutation.h"
int PagingPermutationPlan(const unsigned* Destination, unsigned Count,
    unsigned* Inverse, unsigned char* Visited, PAGING_PAGE_MOVE* Moves,
    unsigned Capacity, unsigned* Written)
{
    unsigned i, used=0;
    if (!Written) return 0;
    *Written=0;
    if (!Destination || !Inverse || !Visited || !Moves || !Count ||
        Count==PAGING_PERMUTATION_SCRATCH || Count>0xffffffffu-Count/2 ||
        Capacity<Count+Count/2) return 0;
    for (i=0;i<Count;i++) Visited[i]=0;
    for (i=0;i<Count;i++) {
        unsigned dst=Destination[i];
        if (dst>=Count || Visited[dst]) return 0;
        Visited[dst]=1; Inverse[dst]=i;
    }
    for (i=0;i<Count;i++) Visited[i]=0;
    for (i=0;i<Count;i++) {
        unsigned target=i, source;
        if (Visited[i]) continue;
        if (Destination[i]==i) {Visited[i]=1;continue;}
        Moves[used].source=i; Moves[used++].destination=PAGING_PERMUTATION_SCRATCH;
        /* Walk inverse edges so every overwrite follows the last source read.
         * Only the first source must survive outside the cycle's own pages. */
        for (;;) {
            Visited[target]=1;
            source=Inverse[target];
            if (source==i) break;
            Moves[used].source=source; Moves[used++].destination=target;
            target=source;
        }
        Moves[used].source=PAGING_PERMUTATION_SCRATCH; Moves[used++].destination=target;
    }
    *Written=used;
    return 1;
}

int PagingPermutationBatch(const PAGING_PAGE_MOVE* Moves,unsigned Count,
    unsigned Start,unsigned MaxMoves,unsigned* Next,unsigned* Required)
{
    unsigned cursor=Start;
    if(!Next || !Required)return PagingPermutationInvalid;
    *Next=Start;*Required=0;
    if(!Moves || Start>Count)return PagingPermutationInvalid;
    while(cursor<Count) {
        unsigned end=cursor+1,cycle;
        if(Moves[cursor].source!=PAGING_PERMUTATION_SCRATCH &&
           Moves[cursor].destination!=PAGING_PERMUTATION_SCRATCH) {
            if(cursor-Start==MaxMoves) {
                *Next=cursor;*Required=1;
                return cursor==Start?PagingPermutationNeedCycle:PagingPermutationMore;
            }
            cursor++;continue;
        }
        if(Moves[cursor].destination!=PAGING_PERMUTATION_SCRATCH ||
           Moves[cursor].source==PAGING_PERMUTATION_SCRATCH)return PagingPermutationInvalid;
        while(end<Count && Moves[end].source!=PAGING_PERMUTATION_SCRATCH) {
            if(Moves[end].destination==PAGING_PERMUTATION_SCRATCH)return PagingPermutationInvalid;
            end++;
        }
        if(end==Count || Moves[end].destination==PAGING_PERMUTATION_SCRATCH || end==cursor+1)
            return PagingPermutationInvalid;
        cycle=end-cursor+1;
        if(cycle>MaxMoves-(cursor-Start)) {
            *Next=cursor;*Required=cycle;
            return cursor==Start?PagingPermutationNeedCycle:PagingPermutationMore;
        }
        cursor=end+1;
    }
    *Next=cursor;return PagingPermutationDone;
}

static void IdentitySwap(PAGING_PAGE_IDENTITY* A,PAGING_PAGE_IDENTITY* B)
{
    PAGING_PAGE_IDENTITY t=*A;*A=*B;*B=t;
}
static void IdentitySift(PAGING_PAGE_IDENTITY* Work,unsigned Root,unsigned Count)
{
    while(Count>1 && Root<=(Count-2)/2) {
        unsigned child=Root*2+1;
        if(child+1<Count && Work[child].address<Work[child+1].address)child++;
        if(Work[Root].address>=Work[child].address)break;
        IdentitySwap(Work+Root,Work+child);Root=child;
    }
}
int PagingPermutationNormalize(const unsigned long long* SourcePages,
    const unsigned long long* DestinationPages,unsigned Count,
    PAGING_PAGE_IDENTITY* Work,unsigned char* Seen,unsigned* Permutation)
{
    unsigned i;
    if(!SourcePages || !DestinationPages || !Count || Count==PAGING_PERMUTATION_SCRATCH ||
       !Work || !Seen || !Permutation)return 0;
    for(i=0;i<Count;i++) {
        if((SourcePages[i]|DestinationPages[i])&4095ull)return 0;
        Work[i].address=SourcePages[i];Work[i].index=i;Seen[i]=0;
    }
    for(i=Count/2;i;i--)IdentitySift(Work,i-1,Count);
    for(i=Count;i>1;i--){IdentitySwap(Work,Work+i-1);IdentitySift(Work,0,i-1);}
    for(i=1;i<Count;i++)if(Work[i-1].address==Work[i].address)return 0;
    for(i=0;i<Count;i++) {
        unsigned lo=0,hi=Count,index;
        while(lo<hi) {
            unsigned mid=lo+(hi-lo)/2;
            if(Work[mid].address<DestinationPages[i])lo=mid+1;else hi=mid;
        }
        if(lo==Count || Work[lo].address!=DestinationPages[i])return 0;
        index=Work[lo].index;
        if(Seen[index])return 0;
        Seen[index]=1;Permutation[i]=index;
    }
    return 1;
}

int PagingPermutationPlanBounded(const unsigned* Destination,unsigned Count,
    unsigned* Inverse,unsigned char* Visited,PAGING_PAGE_MOVE* Moves,
    unsigned Capacity,unsigned MaxAtomicMoves,unsigned* Written)
{
    unsigned i,used=0;
    if(!Written)return 0;
    *Written=0;
    if(!Destination || !Inverse || !Visited || !Moves || !Count ||
       Count>0xffffffffu/3 || Capacity<Count*3 || MaxAtomicMoves<3)return 0;
    for(i=0;i<Count;i++)Visited[i]=0;
    for(i=0;i<Count;i++) {
        unsigned dst=Destination[i];
        if(dst>=Count || Visited[dst])return 0;
        Visited[dst]=1;Inverse[dst]=i;
    }
    for(i=0;i<Count;i++)Visited[i]=0;
    for(i=0;i<Count;i++) {
        unsigned cursor=i,length=0;
        if(Visited[i])continue;
        do {Visited[cursor]=1;length++;cursor=Destination[cursor];}while(cursor!=i);
        if(length==1)continue;
        if(length+1<=MaxAtomicMoves) {
            unsigned target=i,source;
            Moves[used].source=i;Moves[used++].destination=PAGING_PERMUTATION_SCRATCH;
            for(;;) {
                source=Inverse[target];
                if(source==i)break;
                Moves[used].source=source;Moves[used++].destination=target;
                target=source;
            }
            Moves[used].source=PAGING_PERMUTATION_SCRATCH;Moves[used++].destination=target;
        } else {
            // Swap the fixed pivot with forward successors. Each transposition
            // restores scratch before the next one, including across DMA jobs.
            for(cursor=Destination[i];cursor!=i;cursor=Destination[cursor]) {
                Moves[used].source=i;Moves[used++].destination=PAGING_PERMUTATION_SCRATCH;
                Moves[used].source=cursor;Moves[used++].destination=i;
                Moves[used].source=PAGING_PERMUTATION_SCRATCH;Moves[used++].destination=cursor;
            }
        }
    }
    *Written=used;return 1;
}

int PagingPageGraphPlan(const unsigned* Source,const unsigned* Destination,
    unsigned Count,unsigned Identities,unsigned* Readers,unsigned* Writer,
    unsigned* Queue,unsigned* Forward,PAGING_PAGE_MOVE* Moves,unsigned Capacity,
    unsigned MaxAtomicMoves,unsigned* Written)
{
    unsigned i,head=0,tail=0,used=0;
    if(!Written)return 0;
    *Written=0;
    if(!Source || !Destination || !Readers || !Writer || !Queue || !Forward ||
       !Moves || !Count || !Identities || Count>0xffffffffu/3 ||
       Identities==PAGING_PERMUTATION_SCRATCH || Capacity<3*Count || MaxAtomicMoves<3)return 0;
    for(i=0;i<Identities;i++){Readers[i]=0;Writer[i]=PAGING_PERMUTATION_SCRATCH;}
    for(i=0;i<Count;i++) {
        unsigned src=Source[i],dst=Destination[i];
        if(src>=Identities || dst>=Identities)return 0;
        if(Writer[dst]!=PAGING_PERMUTATION_SCRATCH) {
            // Repeated identical assignments have the same initial-value result.
            // Conflicting sources for one destination need a separate contract.
            if(Source[Writer[dst]]!=src)return 0;
            continue;
        }
        Writer[dst]=i;
    }
    for(i=0;i<Count;i++) {
        if(Writer[Destination[i]]!=i)continue; // Count each canonical reader once.
        if(Source[i]==Destination[i])Writer[Destination[i]]=PAGING_PERMUTATION_SCRATCH;
        else Readers[Source[i]]++;
    }
    for(i=0;i<Identities;i++)
        if(Writer[i]!=PAGING_PERMUTATION_SCRATCH && !Readers[i])Queue[tail++]=i;
    while(head<tail) {
        unsigned dst=Queue[head++],src=Source[Writer[dst]];
        Moves[used].source=src;Moves[used++].destination=dst;
        Writer[dst]=PAGING_PERMUTATION_SCRATCH;
        if(--Readers[src]==0 && Writer[src]!=PAGING_PERMUTATION_SCRATCH)Queue[tail++]=src;
    }
    // With distinct destinations, pruning leaves disjoint simple cycles. Any
    // fan-out has already been copied before its source participates in a cycle.
    for(i=0;i<Identities;i++)if(Writer[i]!=PAGING_PERMUTATION_SCRATCH)
        Forward[Source[Writer[i]]]=i;
    for(i=0;i<Identities;i++) {
        unsigned cursor=i,length=0;
        if(Writer[i]==PAGING_PERMUTATION_SCRATCH)continue;
        do {length++;cursor=Forward[cursor];}while(cursor!=i);
        if(length+1<=MaxAtomicMoves) {
            unsigned target=i,src;
            Moves[used].source=i;Moves[used++].destination=PAGING_PERMUTATION_SCRATCH;
            for(;;) {
                src=Source[Writer[target]];
                if(src==i)break;
                Moves[used].source=src;Moves[used++].destination=target;
                target=src;
            }
            Moves[used].source=PAGING_PERMUTATION_SCRATCH;Moves[used++].destination=target;
        } else {
            for(cursor=Forward[i];cursor!=i;cursor=Forward[cursor]) {
                Moves[used].source=i;Moves[used++].destination=PAGING_PERMUTATION_SCRATCH;
                Moves[used].source=cursor;Moves[used++].destination=i;
                Moves[used].source=PAGING_PERMUTATION_SCRATCH;Moves[used++].destination=cursor;
            }
        }
        cursor=i;
        do {Writer[cursor]=PAGING_PERMUTATION_SCRATCH;cursor=Forward[cursor];}while(cursor!=i);
    }
    *Written=used;return 1;
}

int PagingPageGraphNormalize(const unsigned long long* SourcePages,
    const unsigned long long* DestinationPages,unsigned Count,
    PAGING_PAGE_IDENTITY* Work,unsigned long long* Pages,unsigned* SourceIndex,
    unsigned* DestinationIndex,unsigned* IdentityCount)
{
    unsigned i,total,unique=0;
    if(!IdentityCount)return 0;
    *IdentityCount=0;
    if(!SourcePages || !DestinationPages || !Work || !Pages || !SourceIndex ||
       !DestinationIndex || !Count || Count>0x7fffffffu)return 0;
    total=Count*2;
    for(i=0;i<Count;i++) {
        if((SourcePages[i]|DestinationPages[i])&4095ull)return 0;
        Work[i].address=SourcePages[i];Work[i].index=i;
        Work[Count+i].address=DestinationPages[i];Work[Count+i].index=Count+i;
    }
    for(i=total/2;i;i--)IdentitySift(Work,i-1,total);
    for(i=total;i>1;i--){IdentitySwap(Work,Work+i-1);IdentitySift(Work,0,i-1);}
    for(i=0;i<total;i++) {
        if(!i || Work[i].address!=Work[i-1].address)Pages[unique++]=Work[i].address;
        if(Work[i].index<Count)SourceIndex[Work[i].index]=unique-1;
        else DestinationIndex[Work[i].index-Count]=unique-1;
    }
    *IdentityCount=unique;return 1;
}
