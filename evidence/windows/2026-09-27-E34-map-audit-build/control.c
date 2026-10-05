#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "P:/BC-250/scratch/g0-umd-audit-source/src/gallium/drivers/zink/zink_bc250_map_audit.h"
static unsigned growths;
static int fail_growth;
static void *grow(void *p, size_t bytes) { growths++; return fail_growth ? NULL : realloc(p,bytes); }
static struct zink_bc250_map_bucket key(unsigned width) {
 struct zink_bc250_map_bucket k={0}; k.target=2;k.width=width;k.height=479;k.depth=1;k.format=105;k.usage=2;k.box_width=width;k.box_height=479;k.box_depth=1;return k;
}
int main(void) {
 struct zink_bc250_map_bucket *b=NULL;unsigned count=0,capacity=0;
 struct zink_bc250_map_bucket k=key(1);
 fail_growth=1;
 assert(!zink_bc250_map_record(&b,&count,&capacity,&k,4,grow));
 assert(!b && !count && !capacity);
 fail_growth=0;
 for(unsigned i=1;i<=128;i++){k=key(i);assert(zink_bc250_map_record(&b,&count,&capacity,&k,(uint64_t)i*4,grow));}
 assert(count==128 && capacity==128);
 struct zink_bc250_map_bucket *before=b;
 fail_growth=1;k=key(129);
 assert(!zink_bc250_map_record(&b,&count,&capacity,&k,516,grow));
 assert(b==before && count==128 && capacity==128 && b[127].calls==1 && b[127].bytes==512);
 k=key(1);unsigned prior_growths=growths;
 assert(zink_bc250_map_record(&b,&count,&capacity,&k,4,grow));
 assert(growths==prior_growths && b[0].calls==2 && b[0].bytes==8);
 fail_growth=0;
 for(unsigned i=129;i<=4097;i++){k=key(i);assert(zink_bc250_map_record(&b,&count,&capacity,&k,(uint64_t)i*4,grow));}
 uint64_t calls=0,bytes=0;
 for(unsigned i=0;i<count;i++){assert(b[i].width==i+1);calls+=b[i].calls;bytes+=b[i].bytes;}
 assert(count==4097 && capacity==8192 && calls==4098 && bytes==((uint64_t)4097*4098/2)*4+4);
 /* Force relocation explicitly; records are values, never internal pointers. */
 struct zink_bc250_map_bucket *copy=malloc((size_t)capacity*sizeof(*b));assert(copy);memcpy(copy,b,(size_t)count*sizeof(*b));free(b);b=copy;
 k=key(4097);assert(zink_bc250_map_record(&b,&count,&capacity,&k,16388,grow));
 assert(b[4096].calls==2 && b[4096].bytes==32776);
 printf("PASS keys=%u capacity=%u requests=%llu bytes=%llu growth_calls=%u initial_OOM=preserved growth_OOM=preserved duplicate_under_OOM=pass relocation=pass\n",count,capacity,(unsigned long long)calls,(unsigned long long)bytes,growths);
 free(b);return 0;
}
