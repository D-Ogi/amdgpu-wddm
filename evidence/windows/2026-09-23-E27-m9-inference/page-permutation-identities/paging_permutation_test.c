#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "../paging_permutation.h"
static unsigned cases, bad;
static void Check(unsigned* permutation,unsigned n) {
 unsigned inverse[8], written=0, memory[9][17], original[8][17], expected[8][17], i,j,k;
 unsigned char visited[8];PAGING_PAGE_MOVE moves[12];
 unsigned normalized[8];unsigned long long srcPages[8],dstPages[8];
 PAGING_PAGE_IDENTITY identities[8];
 for(i=0;i<n;i++)srcPages[i]=i?0x100000000ull*(i+1)+(i*5%3)*4096ull:0;
 for(i=0;i<n;i++)dstPages[i]=srcPages[permutation[i]];
 if(!PagingPermutationNormalize(srcPages,dstPages,n,identities,visited,normalized)){bad++;return;}
 for(i=0;i<n;i++)if(normalized[i]!=permutation[i]){bad++;return;}
 memset(memory,0,sizeof(memory));
 for(i=0;i<n;i++)for(j=0;j<17;j++)original[i][j]=memory[i][j]=i*1009+j*17+3;
 for(i=0;i<n;i++)memcpy(expected[permutation[i]],original[i],sizeof(original[i]));
 if(!PagingPermutationPlan(normalized,n,inverse,visited,moves,12,&written)){bad++;return;}
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
static void Classification(void) {
 unsigned long long src[]={0,0x100000000ull},dst[]={0x100000000ull,0};
 PAGING_PAGE_IDENTITY work[2];unsigned char seen[2];unsigned p[2];
 if(!PagingPermutationNormalize(src,dst,2,work,seen,p)||p[0]!=1||p[1]!=0)bad++;
 dst[1]=dst[0];if(PagingPermutationNormalize(src,dst,2,work,seen,p))bad++;
 dst[1]=0x200000000ull;if(PagingPermutationNormalize(src,dst,2,work,seen,p))bad++;
 dst[1]=1;if(PagingPermutationNormalize(src,dst,2,work,seen,p))bad++;
 dst[1]=0;src[1]=0;if(PagingPermutationNormalize(src,dst,2,work,seen,p))bad++;
}
static void LargeNormalize(void) {
 const unsigned n=262144;unsigned i;clock_t begin;
 unsigned long long* src=malloc((size_t)n*sizeof(*src));
 unsigned long long* dst=malloc((size_t)n*sizeof(*dst));
 PAGING_PAGE_IDENTITY* work=malloc((size_t)n*sizeof(*work));
 unsigned char* seen=malloc(n);unsigned* p=malloc((size_t)n*sizeof(*p));
 if(!src||!dst||!work||!seen||!p){bad++;goto cleanup;}
 for(i=0;i<n;i++)src[i]=0x100000000ull+(n-i)*8192ull;
 for(i=0;i<n;i++)dst[i]=src[(i+1)%n];
 begin=clock();
 if(!PagingPermutationNormalize(src,dst,n,work,seen,p))bad++;
 else for(i=0;i<n;i++)if(p[i]!=(i+1)%n){bad++;break;}
 printf("1GiB page identities: %u, normalization CPU seconds %.6f\n",n,
        (double)(clock()-begin)/CLOCKS_PER_SEC);
cleanup:
 free(src);free(dst);free(work);free(seen);free(p);
}
int main(void) {
 unsigned p[8],n,i;
 Classification();
 LargeNormalize();
 for(n=1;n<=8;n++){for(i=0;i<n;i++)p[i]=i;Enumerate(p,n,0);}
 printf("page permutations: %u cases, %u snapshot-oracle failures\n",cases,bad);
 return bad?1:0;
}
