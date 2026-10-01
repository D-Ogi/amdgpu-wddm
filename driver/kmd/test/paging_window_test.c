#include <stdio.h>
#include "../paging_window.h"
#define CHECK(x) do{if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){PAGING_WINDOW w;unsigned long long p=(PAGING_DRIVER_GTT_LIMIT/4096)*8;
 CHECK(PagingWindowInit(0x100000000ull,512ull<<20,0x200000000ull,1ull<<20,&w));
 CHECK(w.mc==0x104000000ull && w.table==0x200000000ull+p);
 CHECK(PagingWindowInit(0,PAGING_DRIVER_GTT_LIMIT+8192,0,p+16,&w));
 CHECK(!PagingWindowInit(0,PAGING_DRIVER_GTT_LIMIT+8191,0,p+16,&w) && !w.mc && !w.table);
 CHECK(!PagingWindowInit(0,PAGING_DRIVER_GTT_LIMIT+8192,0,p+15,&w));
 CHECK(!PagingWindowInit(1,512ull<<20,0,1ull<<20,&w));
 CHECK(!PagingWindowInit(0,512ull<<20,1,1ull<<20,&w));
 CHECK(!PagingWindowInit(0xfffffffffffff000ull,512ull<<20,0,1ull<<20,&w));
 CHECK(!PagingWindowInit(0,512ull<<20,0xfffffffffffffff8ull,1ull<<20,&w));
 puts("PASS: disjoint driver/paging aperture, exact bounds, short aperture/table, alignment and48bit overflow");return 0;}
