/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../gfx_copy.h"
#include "../gfx_copy_fields.h"
#include "../../amdgpu-import/nvd.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#c);exit(1); } } while(0)
static void check(unsigned int bytes)
{
    unsigned int buffer[512],saved[512],count,i;
    unsigned long long src=0x100000000ull,dst=0x900000000ull,total=0;
    memset(buffer,0xcc,sizeof(buffer));memcpy(saved,buffer,sizeof(buffer));
    count=Bc250GfxCopyDwords(bytes);CHECK(count>0 && count<512);
    CHECK(!Bc250EmitGfxCopy(buffer,count-1,src,dst,bytes));CHECK(!memcmp(buffer,saved,sizeof(buffer)));
    CHECK(Bc250EmitGfxCopy(buffer,count,src,dst,bytes)==count);
    for(i=0;i<count;i+=7){
        unsigned int amount=buffer[i+6]&S_506_BYTE_COUNT(~0u);
        unsigned int control=S_501_SRC_SEL(V_501_SRC_ADDR_USING_L2)|S_501_DST_SEL(V_501_DST_ADDR_USING_L2);
        if(i+7==count) control|=S_501_CP_SYNC(1);
        CHECK(buffer[i]==(unsigned int)PACKET3(PACKET3_DMA_DATA,5));
        CHECK(buffer[i+1]==control);
        CHECK(((unsigned long long)buffer[i+3]<<32 | buffer[i+2])==src+total);
        CHECK(((unsigned long long)buffer[i+5]<<32 | buffer[i+4])==dst+total);
        CHECK((buffer[i+6]&~S_506_BYTE_COUNT(~0u))==(i==0?S_506_RAW_WAIT(1):0));
        CHECK(amount>0 && amount<=bytes-total);
        total+=amount;
    }
    CHECK(total==bytes);CHECK(!memcmp(buffer+count,saved+count,sizeof(buffer)-count*sizeof(buffer[0])));
}
int main(void)
{
    unsigned int buffer[32],saved[32],sizes[]={1,3,4,31,32,33,4096,9216000,
        S_506_BYTE_COUNT(~0u)&~31u,(S_506_BYTE_COUNT(~0u)&~31u)+1,UINT_MAX};
    unsigned int i;
    for(i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++)check(sizes[i]);
    memset(buffer,0xcc,sizeof(buffer));memcpy(saved,buffer,sizeof(buffer));
    CHECK(!Bc250EmitGfxCopy(buffer,32,0,8,16));
    CHECK(!Bc250EmitGfxCopy(buffer,32,8,0,16));
    CHECK(!Bc250EmitGfxCopy(buffer,32,0,0,16));
    CHECK(!Bc250EmitGfxCopy(buffer,32,~0ull,0,2));
    CHECK(!Bc250EmitGfxCopy(buffer,32,0,~0ull,2));
    CHECK(!Bc250EmitGfxCopy(buffer,32,0,4096,0));
    CHECK(!Bc250EmitGfxCopy(NULL,32,0,4096,4));
    CHECK(!memcmp(buffer,saved,sizeof(buffer)));
    CHECK(Bc250EmitGfxCopy(buffer,7,0,16,16)==7); /* adjacent is not overlapping */
    memset(buffer,0xcc,sizeof(buffer));memcpy(saved,buffer,sizeof(buffer));
    CHECK(!Bc250EmitGfxAcquire(NULL,8));
    for(i=0;i<8;i++) {
        CHECK(!Bc250EmitGfxAcquire(buffer,i));
        CHECK(!memcmp(buffer,saved,sizeof(buffer)));
    }
    CHECK(Bc250EmitGfxAcquire(buffer,8)==8);
    /* Independently decoded GFX10 type3 header, global range and cache bits. */
    CHECK((buffer[0]>>30)==3 && ((buffer[0]>>16)&0x3fff)==6 && ((buffer[0]>>8)&255)==0x58);
    CHECK(buffer[1]==0 && buffer[2]==0xffffffffu && buffer[3]==0xffffffu);
    CHECK(buffer[4]==0 && buffer[5]==0 && buffer[6]==10);
    CHECK(buffer[7]==((1u<<0)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<14)|(1u<<15)));
    CHECK(!memcmp(buffer+8,saved+8,sizeof(buffer)-8*sizeof(buffer[0])));
    puts("PASS GFX10 acquire packet, global range, cache operations, short-buffer atomicity");
    puts("PASS GFX copy chunks,64-bit addresses,final sync,first RAW wait,capacity/overlap/overflow rejection");
    return 0;
}
