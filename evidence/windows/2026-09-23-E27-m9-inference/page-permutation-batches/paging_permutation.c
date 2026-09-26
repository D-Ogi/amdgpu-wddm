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
