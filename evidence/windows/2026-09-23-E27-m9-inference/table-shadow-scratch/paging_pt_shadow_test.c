#include <stdio.h>
#include <string.h>
#include "paging_pt_shadow.h"
static PAGING_PT_SHADOW_SLOT slots[4], before[4];
static unsigned checks,failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);}} while(0)
static void test_copy(void)
{
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 values[512],v;
    PAGING_PT_U64 model[16],snapshot[16];unsigned char known[16],saved[16];
    unsigned i,src,dst,count;
    for(i=0;i<512;i++)values[i]=0x123400000ull+i;
    CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
    CHECK(PagingPtShadowApply(&s,0,0,512,values,1)==PAGING_PT_OK);
    v=0;CHECK(PagingPtShadowApply(&s,0,123,1,&v,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowApply(&s,16384,0,1,values,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowCopy(&s,0,0,16384,0,512)==PAGING_PT_OK);
    for(i=0;i<512;i++)CHECK(PagingPtShadowRead(&s,16384,i,&v)==PAGING_PT_OK && v==(i==123?0:values[i]));
    // Missing source is unknown, never the stale old destination or a known zero.
    CHECK(PagingPtShadowCopy(&s,32768,0,16384,63,3)==PAGING_PT_OK);
    for(i=63;i<66;i++)CHECK(PagingPtShadowRead(&s,16384,i,&v)==PAGING_PT_MISSING && v==0);
    CHECK(PagingPtShadowRead(&s,16384,62,&v)==PAGING_PT_OK && v==values[62]);
    CHECK(PagingPtShadowRead(&s,16384,66,&v)==PAGING_PT_OK && v==values[66]);
    memcpy(before,slots,sizeof(slots));
    CHECK(PagingPtShadowCopy(&s,0,0,32768,0,1)==PAGING_PT_MISSING && s.Used==2);
    CHECK(PagingPtShadowCopy(&s,0,511,16384,0,2)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopy(&s,0,0,16384,511,2)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopy(&s,1,0,16384,0,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopy(&s,0,0,1ull<<48,0,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopy(&s,0,0,16384,0,0)==PAGING_PT_INVALID);
    CHECK(memcmp(before,slots,sizeof(slots))==0);
    // Independent snapshot oracle, both overlap directions, self-copy and
    // disjoint subranges. Base60 crosses the64-bit Known bitmap boundary.
    for(src=0;src<8;src++)for(dst=0;dst<8;dst++)for(count=1;count<=8;count++) {
        CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
        for(i=0;i<16;i++) {
            model[i]=snapshot[i]=(i==5?0:100+i);known[i]=saved[i]=(unsigned char)(i%3!=1);
            if(known[i])CHECK(PagingPtShadowApply(&s,0,60+i,1,model+i,1)==PAGING_PT_OK);
        }
        for(i=0;i<count;i++){model[dst+i]=snapshot[src+i];known[dst+i]=saved[src+i];}
        CHECK(PagingPtShadowCopy(&s,0,60+src,0,60+dst,count)==PAGING_PT_OK);
        for(i=0;i<16;i++) {
            int status=PagingPtShadowRead(&s,0,60+i,&v);
            CHECK(known[i]?(status==PAGING_PT_OK && v==model[i]):(status==PAGING_PT_MISSING && v==0));
        }
    }
}
static void test_fill(void)
{
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 values[512],v;
    unsigned first,count,i;unsigned model[1024];
    for(i=0;i<512;i++)values[i]=0x123456789abcdef0ull+i;
    CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
    for(first=0;first<1024;first++)for(count=1;count<=9 && count<=1024-first;count++) {
        CHECK(PagingPtShadowApply(&s,0x1000,0,512,values,1)==PAGING_PT_OK);
        memcpy(model,values,sizeof(model));
        for(i=first;i<first+count;i++)model[i]=0xA5C31234;
        CHECK(PagingPtShadowFill(&s,0x1000+(PAGING_PT_U64)first*4,count*4,0xA5C31234)==PAGING_PT_OK);
        for(i=first/2;i<=(first+count-1)/2;i++) {
            PAGING_PT_U64 expected=((PAGING_PT_U64)model[i*2+1]<<32)|model[i*2];
            CHECK(PagingPtShadowRead(&s,0x1000,i,&v)==PAGING_PT_OK && v==expected);
        }
        CHECK(memcmp(slots[1].Entries,model,sizeof(model))==0);
    }
    CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
    CHECK(PagingPtShadowApply(&s,0x1000,511,1,values,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowApply(&s,0x2000,0,1,values,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowFill(&s,0x1ffc,8,0xAABBCCDD)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0x1000,511,&v)==PAGING_PT_OK && v==0xAABBCCDD9abcdef0ull);
    CHECK(PagingPtShadowRead(&s,0x2000,0,&v)==PAGING_PT_OK && v==0x12345678AABBCCDDull);
    CHECK(PagingPtShadowFill(&s,0x1000,4,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0x1000,0,&v)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowFill(&s,0x1000,8,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0x1000,0,&v)==PAGING_PT_OK && !v);
    memcpy(before,slots,sizeof(slots));
    CHECK(PagingPtShadowFill(&s,0x1001,8,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowFill(&s,0x1000,0,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowFill(&s,0xfffffffffffcull,8,0)==PAGING_PT_INVALID);
    CHECK(memcmp(before,slots,sizeof(slots))==0);
    CHECK(PagingPtShadowFill(&s,0x3000,4096,0)==PAGING_PT_OK && s.Used==2);
}

static void test_half_knowledge(void)
{
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 v,zero=0;
    unsigned i,order,src,dst,count,j;
    PAGING_PT_U64 model[16],saved[16];unsigned known[16],savedKnown[16];
    for(order=0;order<2;order++)for(i=0;i<512;i++) {
        CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
        CHECK(PagingPtShadowApply(&s,0,511,1,&zero,1)==PAGING_PT_OK);
        CHECK(PagingPtShadowCopy(&s,4096,0,0,i,1)==PAGING_PT_OK);
        CHECK(PagingPtShadowFill(&s,(PAGING_PT_U64)i*8+order*4,4,order?0x11223344:0xAABBCCDD)==PAGING_PT_OK);
        CHECK(PagingPtShadowRead(&s,0,i,&v)==PAGING_PT_MISSING && !v);
        CHECK(PagingPtShadowFill(&s,(PAGING_PT_U64)i*8+(1-order)*4,4,order?0xAABBCCDD:0x11223344)==PAGING_PT_OK);
        CHECK(PagingPtShadowRead(&s,0,i,&v)==PAGING_PT_OK && v==0x11223344AABBCCDDull);
    }
    // Snapshot oracle covers both overlap directions and bitmap boundaries.
    for(src=0;src<8;src++)for(dst=0;dst<8;dst++)for(count=1;count<=8;count++) {
        CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
        CHECK(PagingPtShadowApply(&s,0,511,1,&zero,1)==PAGING_PT_OK);
        for(i=0;i<16;i++) {
            known[i]=savedKnown[i]=i%4;model[i]=saved[i]=((PAGING_PT_U64)(200+i)<<32)|(100+i);
            if(known[i]&1)CHECK(PagingPtShadowFill(&s,(28+i)*8,4,100+i)==PAGING_PT_OK);
            if(known[i]&2)CHECK(PagingPtShadowFill(&s,(28+i)*8+4,4,200+i)==PAGING_PT_OK);
        }
        for(i=0;i<count;i++){model[dst+i]=saved[src+i];known[dst+i]=savedKnown[src+i];}
        CHECK(PagingPtShadowCopy(&s,0,28+src,0,28+dst,count)==PAGING_PT_OK);
        for(i=0;i<16;i++) {
            int rc=PagingPtShadowRead(&s,0,28+i,&v);
            CHECK(known[i]==3 ? rc==PAGING_PT_OK && v==model[i] : rc==PAGING_PT_MISSING && !v);
            // Complete only missing halves; previously copied known halves must survive.
            for(j=0;j<2;j++)if(!(known[i]&(1u<<j))) {
                CHECK(PagingPtShadowFill(&s,(28+i)*8+j*4,4,0x98765432)==PAGING_PT_OK);
                if(j)model[i]=(model[i]&0xffffffffull)|0x9876543200000000ull;
                else model[i]=(model[i]&0xffffffff00000000ull)|0x98765432;
            }
            CHECK(PagingPtShadowRead(&s,0,28+i,&v)==PAGING_PT_OK && v==model[i]);
        }
    }
}

static void test_byte_copy(void)
{
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 values[512],v,zero=0;
    unsigned char model[128],saved[128],known[128],savedKnown[128];
    unsigned src,dst,count,i,j;
    for(i=0;i<512;i++)values[i]=0x1020304050607080ull+i;
    for(src=0;src<16;src++)for(dst=0;dst<16;dst++)for(count=1;count<=16;count++) {
        CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
        CHECK(PagingPtShadowApply(&s,0x1000,0,512,values,1)==PAGING_PT_OK);
        CHECK(PagingPtShadowApply(&s,0,511,1,&zero,1)==PAGING_PT_OK);
        for(i=0;i<128;i++) {
            model[i]=saved[i]=(unsigned char)(values[i/8]>>((i%8)*8));
            known[i]=savedKnown[i]=(unsigned char)(i%3!=1);
            if(known[i])CHECK(PagingPtShadowCopyBytes(&s,0x1000+i,48+i,1,1)==PAGING_PT_OK);
        }
        for(i=0;i<count;i++){model[dst+i]=saved[src+i];known[dst+i]=savedKnown[src+i];}
        CHECK(PagingPtShadowCopyBytes(&s,48+src,48+dst,count,1)==PAGING_PT_OK);
        for(i=0;i<16;i++) {
            unsigned all=1;PAGING_PT_U64 expected=0;
            for(j=0;j<8;j++){all&=known[i*8+j];expected|=(PAGING_PT_U64)model[i*8+j]<<(j*8);}
            CHECK(all ? PagingPtShadowRead(&s,0,6+i,&v)==PAGING_PT_OK && v==expected :
                        PagingPtShadowRead(&s,0,6+i,&v)==PAGING_PT_MISSING && !v);
            // Complete missing bytes from a known source, exposing retained partial values.
            for(j=0;j<8;j++)if(!known[i*8+j]) {
                CHECK(PagingPtShadowCopyBytes(&s,0x1000,48+i*8+j,1,1)==PAGING_PT_OK);
                model[i*8+j]=0x80;
            }
            expected=0;for(j=0;j<8;j++)expected|=(PAGING_PT_U64)model[i*8+j]<<(j*8);
            CHECK(PagingPtShadowRead(&s,0,6+i,&v)==PAGING_PT_OK && v==expected);
        }
    }
    CHECK(PagingPtShadowCopyBytes(&s,0x2000,128*8,8,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowCopyBytes(&s,0x1003,128*8+3,1,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowCopy(&s,0,128,0,129,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,129,&v)==PAGING_PT_MISSING && !v);
    for(i=0;i<8;i++)if(i!=3)CHECK(PagingPtShadowCopyBytes(&s,0x1000,129*8+i,1,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,129,&v)==PAGING_PT_OK && v==0x8080808050808080ull);
    CHECK(PagingPtShadowCopyBytes(&s,0x1fff,4095,1,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowCopyBytes(&s,0x1000,48,1,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,6,&v)==PAGING_PT_MISSING && !v);
    CHECK(PagingPtShadowCopyBytes(&s,0x1000,48,1,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,6,&v)==PAGING_PT_OK);
    memcpy(before,slots,sizeof(slots));
    CHECK(PagingPtShadowCopyBytes(&s,4095,0,2,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopyBytes(&s,0,4095,2,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopyBytes(&s,0,0,0,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopyBytes(&s,0,0,1,2)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopyBytes(&s,1ull<<48,0,1,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopyBytes(&s,0,1ull<<48,1,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopyBytes(&s,0,0x2000,1,1)==PAGING_PT_MISSING);
    CHECK(memcmp(before,slots,sizeof(slots))==0);
}

static void test_staged_cycle(void)
{
    static PAGING_PT_SHADOW_SLOT scratch;
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 values[512];
    unsigned offset,shortBand,page,i,bytes;
    for(offset=0;offset<4096;offset+=67)for(shortBand=0;shortBand<2;shortBand++) {
        bytes=4096-offset;if(shortBand && bytes>17)bytes=17;
        CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
        for(page=0;page<3;page++) {
            for(i=0;i<512;i++)values[i]=0x1937597B9DBFD1E3ull^((PAGING_PT_U64)page*0x3141592653589793ull)^i;
            CHECK(PagingPtShadowApply(&s,(PAGING_PT_U64)page*4096,0,512,values,1)==PAGING_PT_OK);
            for(i=page;i<4096;i+=13)CHECK(PagingPtShadowCopyBytes(&s,0,(PAGING_PT_U64)page*4096+i,1,0)==PAGING_PT_OK);
        }
        memcpy(before,slots,sizeof(slots));memset(&scratch,0xCD,sizeof(scratch));
        CHECK(PagingPtShadowSaveBytes(&s,offset,bytes,1,&scratch)==PAGING_PT_OK);
        CHECK(PagingPtShadowCopyBytes(&s,8192+offset,offset,bytes,1)==PAGING_PT_OK);
        CHECK(PagingPtShadowCopyBytes(&s,4096+offset,8192+offset,bytes,1)==PAGING_PT_OK);
        CHECK(PagingPtShadowRestoreBytes(&s,&scratch,4096+offset,bytes)==PAGING_PT_OK);
        CHECK(s.Used==3 && !scratch.Occupied);
        for(page=0;page<3;page++)for(i=0;i<4096;i++) {
            unsigned from=(i>=offset && i-offset<bytes)?(page+2)%3:page;
            unsigned a,b;PAGING_PT_SHADOW_SLOT *actual=NULL,*expected=NULL;
            for(a=0;a<4;a++){if(slots[a].Occupied && slots[a].Physical==(PAGING_PT_U64)page*4096)actual=&slots[a];}
            for(b=0;b<4;b++){if(before[b].Occupied && before[b].Physical==(PAGING_PT_U64)from*4096)expected=&before[b];}
            CHECK(actual && expected &&
                  ((actual->Known[i/64]>>(i%64))&1)==((expected->Known[i/64]>>(i%64))&1) &&
                  (!((expected->Known[i/64]>>(i%64))&1) ||
                   ((actual->Entries[i/8]>>((i%8)*8))&255)==((expected->Entries[i/8]>>((i%8)*8))&255)));
        }
        // A later SAVE of unknown data must not reuse the previous cycle's bytes.
        CHECK(PagingPtShadowSaveBytes(&s,0,17,0,&scratch)==PAGING_PT_OK);
        for(i=0;i<64;i++)CHECK(scratch.Known[i]==0);
        CHECK(PagingPtShadowRestoreBytes(&s,&scratch,0,17)==PAGING_PT_OK);
        for(i=0;i<3;i++){PAGING_PT_U64 value=123;CHECK(PagingPtShadowRead(&s,0,i,&value)==PAGING_PT_MISSING && !value);}
    }
}

int main(void)
{
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 values[512],v=123;unsigned i;
    CHECK(PagingPtShadowTableCount(1ull<<30,4)==515);
    CHECK(PagingPtShadowTableCount(1ull<<30,3)==514);
    CHECK(PagingPtShadowTableCount(1ull<<30,2)==513);
    CHECK(PagingPtShadowTableCount((1ull<<30)+1,2)==0);
    CHECK(PagingPtShadowTableCount((1ull<<30)+1,4)==517);
    CHECK(PagingPtShadowTableCount(0,4)==0);
    CHECK(PagingPtShadowTableCount((1ull<<48)+1,4)==0);
    CHECK(PagingPtShadowTableCount(1,0)==0);
    CHECK(PagingPtShadowTableCount(1,5)==0);
    CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
    for(i=0;i<512;i++)values[i]=0x123400000ull+i;
    CHECK(PagingPtShadowRead(&s,0,0,&v)==PAGING_PT_MISSING && v==0);
    CHECK(PagingPtShadowApply(&s,0,0,1,values,0)==PAGING_PT_MISSING && !s.Used);
    CHECK(PagingPtShadowApply(&s,0,63,2,values,1)==PAGING_PT_OK && s.Used==1);
    CHECK(PagingPtShadowRead(&s,0,62,&v)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowRead(&s,0,63,&v)==PAGING_PT_OK && v==values[0]);
    CHECK(PagingPtShadowRead(&s,0,64,&v)==PAGING_PT_OK && v==values[1]);
    // Split a full-table update as the current GPU builder splits512 PTEs.
    CHECK(PagingPtShadowApply(&s,0,0,480,values,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,480,&v)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowApply(&s,0,480,32,values+480,0)==PAGING_PT_OK);
    for(i=0;i<512;i++)CHECK(PagingPtShadowRead(&s,0,i,&v)==PAGING_PT_OK && v==values[i]);
    // Explicit invalidation is a known zero, not an absent table entry.
    v=0;CHECK(PagingPtShadowApply(&s,0,64,1,&v,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,64,&v)==PAGING_PT_OK && v==0);
    // All four physical pages hash to one bucket; collisions must retain identity.
    for(i=1;i<4;i++)CHECK(PagingPtShadowApply(&s,(PAGING_PT_U64)i*16384,0,1,values+i,1)==PAGING_PT_OK);
    for(i=1;i<4;i++)CHECK(PagingPtShadowRead(&s,(PAGING_PT_U64)i*16384,0,&v)==PAGING_PT_OK && v==values[i]);
    memcpy(before,slots,sizeof(slots));
    CHECK(PagingPtShadowCanApply(&s,0,0,1,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowCanApply(&s,65536,0,1,1)==PAGING_PT_FULL);
    CHECK(PagingPtShadowCanApply(&s,65536,0,1,0)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowCanApply(&s,0,512,1,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,65536,0,1,values,1)==PAGING_PT_FULL);
    CHECK(PagingPtShadowApply(&s,65536,0,1,values,0)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowApply(&s,0,511,2,values,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,1,0,1,values,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,1ull<<48,0,1,values,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,0,0,0,values,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,0,0,1,NULL,0)==PAGING_PT_INVALID);
    CHECK(memcmp(before,slots,sizeof(slots))==0 && s.Used==4);
    // Existing tables remain writable even when registration storage is full.
    CHECK(PagingPtShadowApply(&s,0,0,1,values+7,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,0,&v)==PAGING_PT_OK && v==values[7]);
    CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK && !s.Used);
    CHECK(PagingPtShadowRead(&s,0,63,&v)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowApply(&s,0xfffffffff000ull,511,1,values,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0xfffffffff000ull,511,&v)==PAGING_PT_OK && v==values[0]);
    test_copy();
    test_fill();
    test_half_knowledge();
    test_byte_copy();
    test_staged_cycle();
    printf("%u checks, %u failures: %s\n",checks,failures,failures?"FAIL":"PASS");
    return failures?1:0;
}
