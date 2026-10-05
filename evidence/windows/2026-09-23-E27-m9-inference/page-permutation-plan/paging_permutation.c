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
