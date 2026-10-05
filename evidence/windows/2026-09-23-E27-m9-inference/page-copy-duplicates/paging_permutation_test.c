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
 /* Oversized cycles must make progress with the original fixed budget.
  * No simulated budget growth: each accepted atomic group retires scratch. */
 for(j=3;j<=9;j++) {
  PAGING_PAGE_MOVE bounded[24];unsigned count=0,cursor=0,steps=0;
  if(!PagingPermutationPlanBounded(normalized,n,inverse,visited,bounded,24,j,&count)){bad++;continue;}
  for(i=0;i<n;i++)memcpy(memory[i],original[i],sizeof(original[i]));
  while(cursor<count) {
   unsigned next=cursor,required=0;
   int rc=PagingPermutationBatch(bounded,count,cursor,j,&next,&required);
   if((rc!=PagingPermutationDone && rc!=PagingPermutationMore) || next<=cursor ||
      next>count || next-cursor>j || ++steps>n){bad++;break;}
   memset(memory[n],0xa5,sizeof(memory[n]));
   for(k=cursor;k<next;k++) {
    unsigned src=bounded[k].source,dst=bounded[k].destination;
    if(src==PAGING_PERMUTATION_SCRATCH)src=n;
    if(dst==PAGING_PERMUTATION_SCRATCH)dst=n;
    if(src>n || dst>n){bad++;break;}
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
static void Graphs(void) {
 unsigned n,graphCases=0;
 for(n=1;n<=6;n++) {
  unsigned src[6]={0},dst[6],readers[8],writer[8],queue[8],forward[8];
  unsigned original[8][17],expected[8][17],memory[9][17],i,j,budget;
  int more=1;
  for(i=0;i<n;i++)dst[i]=i;
  for(i=0;i<n+2;i++)for(j=0;j<17;j++)original[i][j]=i*1009+j*17+3;
  while(more) {
   for(i=0;i<n+2;i++)memcpy(expected[i],original[i],sizeof(original[i]));
   for(i=0;i<n;i++)memcpy(expected[dst[i]],original[src[i]],sizeof(original[i]));
   for(budget=3;budget<=8;budget++) {
    PAGING_PAGE_MOVE moves[18];unsigned count=0,cursor=0;
    if(!PagingPageGraphPlan(src,dst,n,n+2,readers,writer,queue,forward,moves,18,budget,&count)){bad++;continue;}
    for(i=0;i<n+2;i++)memcpy(memory[i],original[i],sizeof(original[i]));
    while(cursor<count) {
     unsigned next=cursor,required=0,k;
     int rc=PagingPermutationBatch(moves,count,cursor,budget,&next,&required);
     if((rc!=PagingPermutationDone && rc!=PagingPermutationMore) || next<=cursor || next-cursor>budget){bad++;break;}
     memset(memory[n+2],0xa5,sizeof(memory[n+2]));
     for(k=cursor;k<next;k++) {
      unsigned from=moves[k].source,to=moves[k].destination;
      if(from==PAGING_PERMUTATION_SCRATCH)from=n+2;
      if(to==PAGING_PERMUTATION_SCRATCH)to=n+2;
      if(from>n+2 || to>n+2){bad++;break;}
      memcpy(memory[to],memory[from],sizeof(memory[from]));
     }
     cursor=next;
    }
    for(i=0;i<n+2;i++)if(memcmp(memory[i],expected[i],sizeof(memory[i])))bad++;
   }
   graphCases++;
   for(i=0;i<n;i++){if(++src[i]<n+2)break;src[i]=0;}
   if(i==n)more=0;
  }
 }
 printf("page copy graphs: %u, budgets 3..8, cumulative failures %u\n",graphCases,bad);
}
static void DuplicateGraphs(void) {
 unsigned n,graphCases=0,accepted=0;
 for(n=1;n<=4;n++) {
  unsigned combinations=1,code;
  for(code=0;code<n;code++)combinations*=16;
  for(code=0;code<combinations;code++) {
   unsigned value=code,src[4],dst[4],first[4],readers[4],writer[4],queue[4],forward[4];
   unsigned memory[5][7],original[4][7],expected[4][7],i,j,written=0,cursor=0;
   PAGING_PAGE_MOVE moves[12];int consistent=1,ok;
   for(i=0;i<4;i++)first[i]=4;
   for(i=0;i<n;i++) {
    src[i]=value%4;value/=4;dst[i]=value%4;value/=4;
    if(first[dst[i]]<4 && first[dst[i]]!=src[i])consistent=0;
    first[dst[i]]=src[i];
   }
   ok=PagingPageGraphPlan(src,dst,n,4,readers,writer,queue,forward,moves,12,3,&written);
   if(ok!=consistent){bad++;continue;}
   graphCases++;
   if(!ok){if(written)bad++;continue;}
   accepted++;
   for(i=0;i<4;i++)for(j=0;j<7;j++)memory[i][j]=original[i][j]=i*1009+j*17+3;
   memcpy(expected,original,sizeof(expected));
   for(i=0;i<n;i++)memcpy(expected[dst[i]],original[src[i]],sizeof(original[i]));
   while(cursor<written) {
    unsigned next=cursor,required=0,k;
    int rc=PagingPermutationBatch(moves,written,cursor,3,&next,&required);
    if((rc!=PagingPermutationDone && rc!=PagingPermutationMore)||next<=cursor||next-cursor>3){bad++;break;}
    memset(memory[4],0xa5,sizeof(memory[4]));
    for(k=cursor;k<next;k++) {
     unsigned from=moves[k].source,to=moves[k].destination;
     if(from==PAGING_PERMUTATION_SCRATCH)from=4;
     if(to==PAGING_PERMUTATION_SCRATCH)to=4;
     if(from>4||to>4){bad++;break;}
     memcpy(memory[to],memory[from],sizeof(memory[from]));
    }
    cursor=next;
   }
   if(memcmp(memory,expected,sizeof(expected)))bad++;
  }
 }
 printf("duplicate-destination graphs: %u classified, %u accepted, cumulative failures %u\n",graphCases,accepted,bad);
}
int main(void) {
 unsigned p[8],n,i;
 DuplicateGraphs();
 Graphs();
 Classification();
 LargeNormalize();
 for(n=1;n<=8;n++){for(i=0;i<n;i++)p[i]=i;Enumerate(p,n,0);}
 printf("page permutations: %u cases, %u snapshot-oracle failures\n",cases,bad);
 return bad?1:0;
}
