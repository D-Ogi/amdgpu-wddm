#include <stdio.h>
#include <string.h>
#include "../paging_permutation.h"
static unsigned cases, bad;
static void Check(unsigned* permutation,unsigned n) {
 unsigned inverse[8], written=0, memory[9][17], original[8][17], expected[8][17], i,j,k;
 unsigned char visited[8];PAGING_PAGE_MOVE moves[12];
 memset(memory,0,sizeof(memory));
 for(i=0;i<n;i++)for(j=0;j<17;j++)original[i][j]=memory[i][j]=i*1009+j*17+3;
 for(i=0;i<n;i++)memcpy(expected[permutation[i]],original[i],sizeof(original[i]));
 if(!PagingPermutationPlan(permutation,n,inverse,visited,moves,12,&written)){bad++;return;}
 /* One action per simulated DMA buffer. Scratch and pages survive every resume.
  * Oracle uses a full initial snapshot, independent of plan traversal. */
 for(k=0;k<written;k++) {
  unsigned src=moves[k].source,dst=moves[k].destination;
  if(src==PAGING_PERMUTATION_SCRATCH)src=n;
  if(dst==PAGING_PERMUTATION_SCRATCH)dst=n;
  if(src>n||dst>n){bad++;return;}
  memcpy(memory[dst],memory[src],sizeof(memory[src]));
 }
 for(i=0;i<n;i++)if(memcmp(memory[i],expected[i],sizeof(memory[i])))bad++;
 /* Exercise every action budget. Between batches another queued job destroys
  * the shared scratch page; correct complete-cycle boundaries tolerate this. */
 for(j=1;j<=12;j++) {
  unsigned cursor=0,steps=0;
  for(i=0;i<n;i++)memcpy(memory[i],original[i],sizeof(original[i]));
  while(cursor<written) {
   unsigned next=cursor,required=0,cap=j;
   int rc=PagingPermutationBatch(moves,written,cursor,cap,&next,&required);
   if(rc==PagingPermutationNeedCycle) {
    if(next!=cursor || required<=cap){bad++;break;}
    cap=required; /* simulate a correctly sized complete-cycle buffer */
    rc=PagingPermutationBatch(moves,written,cursor,cap,&next,&required);
   }
   if((rc!=PagingPermutationDone && rc!=PagingPermutationMore) || next<=cursor ||
      next>written || next-cursor>cap || ++steps>n){bad++;break;}
   memset(memory[n],0xa5,sizeof(memory[n]));
   for(k=cursor;k<next;k++) {
    unsigned src=moves[k].source,dst=moves[k].destination;
    if(src==PAGING_PERMUTATION_SCRATCH)src=n;
    if(dst==PAGING_PERMUTATION_SCRATCH)dst=n;
    memcpy(memory[dst],memory[src],sizeof(memory[src]));
   }
   cursor=next;
  }
  for(i=0;i<n;i++)if(memcmp(memory[i],expected[i],sizeof(memory[i])))bad++;
 }
 cases++;
}
static void Enumerate(unsigned* p,unsigned n,unsigned at) {
 unsigned i;
 if(at==n){Check(p,n);return;}
 for(i=at;i<n;i++) {
  unsigned t=p[at];p[at]=p[i];p[i]=t;
  Enumerate(p,n,at+1);
  t=p[at];p[at]=p[i];p[i]=t;
 }
}
int main(void) {
 unsigned p[8],n,i;
 for(n=1;n<=8;n++){for(i=0;i<n;i++)p[i]=i;Enumerate(p,n,0);}
 printf("page permutations: %u cases, %u snapshot-oracle failures\n",cases,bad);
 return bad?1:0;
}
