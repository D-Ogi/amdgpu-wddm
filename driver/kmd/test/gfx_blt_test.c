/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx_blt.h"
#include "../gfx_copy.h"
#include "../gfx_copy_fields.h"
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#c);exit(1);}}while(0)
int main(void)
{
    BC250_BLIT_SURFACE s={8,7,40,280,Bc250BltBgra8},d={11,9,48,432,Bc250BltBgra8};
    BC250_BLIT_RECT sr={2,1,7,6},dr={4,3,9,8};
    BC250_BLIT_PLAN plan,wide,bad;
    BC250_BLIT_CURSOR cursor,next;
    unsigned char source[280],actual[432],expected[432];
    unsigned int buffer[64],saved[64],capacity,written,i,j,x,y,attempts,maximum;
    unsigned long long sourceBase=0x100000000ull,destinationBase=0x200000000ull;
    BC250_GFX_BLIT_RESULT result;
    for(i=0;i<sizeof(source);i++)source[i]=(unsigned char)(37u*i+13u);
    memset(expected,0xcc,sizeof(expected));
    for(y=3;y<8;y++)for(x=4;x<9;x++)
        memcpy(expected+y*48u+x*4u,source+(y-3u+1u)*40u+(x-4u+2u)*4u,4);
    CHECK(Bc250PlanBlt(&s,&d,&sr,&dr,NULL,&plan)==Bc250BltCopy);
    for(capacity=7;capacity<=36;capacity++){
        memset(actual,0xcc,sizeof(actual));cursor.Row=cursor.ByteInRow=0;attempts=0;
        do{
            memset(buffer,0xcc,sizeof(buffer));memcpy(saved,buffer,sizeof(buffer));
            result=Bc250EmitGfxBlt(&plan,sourceBase,destinationBase,&cursor,&cursor,buffer,capacity,&written);
            CHECK(result==Bc250GfxBltMore || result==Bc250GfxBltDone);
            CHECK(written>0 && written<=capacity && written%7u==0);
            CHECK(!memcmp(buffer+written,saved+written,sizeof(buffer)-written*sizeof(buffer[0])));
            for(j=0;j<written;j+=7){
                unsigned long long from=((unsigned long long)buffer[j+3]<<32)|buffer[j+2];
                unsigned long long to=((unsigned long long)buffer[j+5]<<32)|buffer[j+4];
                unsigned int bytes=buffer[j+6]&S_506_BYTE_COUNT(~0u);
                CHECK(from>=sourceBase && to>=destinationBase);
                from-=sourceBase;to-=destinationBase;
                CHECK(from+bytes<=sizeof(source) && to+bytes<=sizeof(actual));
                CHECK(!!(buffer[j+1]&S_501_CP_SYNC(1))==(j+7==written));
                CHECK(!!(buffer[j+6]&S_506_RAW_WAIT(1))==(j==0));
                memcpy(actual+to,source+from,bytes);
            }
            CHECK(++attempts<=plan.Rows);
        }while(result==Bc250GfxBltMore);
        CHECK(cursor.Row==plan.Rows && cursor.ByteInRow==0);
        CHECK(!memcmp(actual,expected,sizeof(actual)));
    }
    maximum=Bc250GfxCopyMaxBytes();wide=plan;
    wide.SourceOffset=wide.DestinationOffset=0;
    wide.RowBytes=maximum+64;wide.SourcePitch=wide.DestinationPitch=wide.RowBytes+16;wide.Rows=2;
    cursor.Row=cursor.ByteInRow=0;
    for(i=0;i<4;i++){
        result=Bc250EmitGfxBlt(&wide,sourceBase,destinationBase,&cursor,&next,buffer,7,&written);
        CHECK(written==7 && result==(i==3?Bc250GfxBltDone:Bc250GfxBltMore));
        CHECK(next.Row==(i+1)/2 && next.ByteInRow==((i&1u)?0:maximum));
        CHECK((buffer[6]&S_506_BYTE_COUNT(~0u))==((i&1u)?64:maximum));cursor=next;
    }
    cursor.Row=cursor.ByteInRow=0;
    memset(buffer,0xcc,sizeof(buffer));memcpy(saved,buffer,sizeof(buffer));
    CHECK(Bc250EmitGfxBlt(&plan,sourceBase,destinationBase,&cursor,&next,buffer,6,&written)==Bc250GfxBltNoSpace);
    CHECK(written==0 && next.Row==0 && next.ByteInRow==0);
    CHECK(Bc250EmitGfxBlt(&plan,sourceBase,sourceBase,&cursor,&next,buffer,64,&written)==Bc250GfxBltInvalid);
    bad=plan;bad.DestinationOffset=~0ull-3;
    CHECK(Bc250EmitGfxBlt(&bad,sourceBase,0,&cursor,&next,buffer,64,&written)==Bc250GfxBltInvalid);
    bad=plan;bad.SourcePitch=4;
    CHECK(Bc250EmitGfxBlt(&bad,sourceBase,destinationBase,&cursor,&next,buffer,64,&written)==Bc250GfxBltInvalid);
    cursor.ByteInRow=plan.RowBytes;
    CHECK(Bc250EmitGfxBlt(&plan,sourceBase,destinationBase,&cursor,&next,buffer,64,&written)==Bc250GfxBltInvalid);
    CHECK(!memcmp(buffer,saved,sizeof(buffer)));
    memset(&bad,0,sizeof(bad));cursor.Row=cursor.ByteInRow=0;
    CHECK(Bc250EmitGfxBlt(&bad,sourceBase,destinationBase,&cursor,&next,NULL,0,&written)==Bc250GfxBltDone && !written);
    /* Dirty-list multipass: pixel oracle is independent of the packet cursor. */
    {
        BC250_BLIT_RECT rects[4]={{4,3,7,5},{8,5,9,8},{0,0,1,1},{6,4,8,7}};
        unsigned int offset,totalWrites,k;
        memset(expected,0xcc,sizeof(expected));
        for(y=3;y<8;y++)for(x=4;x<9;x++) {
            int hit=0;
            for(k=0;k<4;k++)if((int)x>=rects[k].Left && (int)x<rects[k].Right &&
                (int)y>=rects[k].Top && (int)y<rects[k].Bottom)hit=1;
            if(hit)memcpy(expected+y*48+x*4,source+(y-2)*40+(x-2)*4,4);
        }
        for(capacity=7;capacity<=36;capacity++) {
            offset=totalWrites=attempts=0;memset(actual,0xcc,sizeof(actual));
            do {
                memset(buffer,0xcc,sizeof(buffer));memcpy(saved,buffer,sizeof(buffer));
                result=Bc250EmitGfxBltList(&s,&d,&sr,&dr,rects,4,sourceBase,destinationBase,
                    offset,&offset,buffer,capacity,&written);
                CHECK(result==Bc250GfxBltMore || result==Bc250GfxBltDone);
                CHECK(written && written%7==0 && written<=capacity);
                CHECK(!memcmp(buffer+written,saved+written,sizeof(buffer)-written*sizeof(buffer[0])));
                for(j=0;j<written;j+=7) {
                    unsigned long long from=(((unsigned long long)buffer[j+3]<<32)|buffer[j+2])-sourceBase;
                    unsigned long long to=(((unsigned long long)buffer[j+5]<<32)|buffer[j+4])-destinationBase;
                    unsigned int bytes=buffer[j+6]&S_506_BYTE_COUNT(~0u);
                    CHECK(from+bytes<=sizeof(source) && to+bytes<=sizeof(actual));
                    memcpy(actual+to,source+from,bytes);
                }
                CHECK(buffer[6]&S_506_RAW_WAIT(1));
                CHECK(buffer[written-6]&S_501_CP_SYNC(1));
                totalWrites+=written/7;CHECK(offset==totalWrites);CHECK(++attempts<=8);
            } while(result==Bc250GfxBltMore);
            CHECK(offset==8 && !memcmp(actual,expected,sizeof(actual)));
        }
        /* A bad late rectangle must not leave a valid prefix in the DMA buffer,
         * including when a previous pass already consumed that prefix. */
        rects[3].Right=12;
        for(offset=0;offset<3;offset++) {
            unsigned int nextOffset;
            memset(buffer,0xcc,sizeof(buffer));memcpy(saved,buffer,sizeof(buffer));
            CHECK(Bc250EmitGfxBltList(&s,&d,&sr,&dr,rects,4,sourceBase,destinationBase,
                offset,&nextOffset,buffer,64,&written)==Bc250GfxBltInvalid);
            CHECK(!written && nextOffset==offset && !memcmp(saved,buffer,sizeof(buffer)));
        }
        CHECK(Bc250EmitGfxBltList(&s,&d,&sr,&dr,0,0,sourceBase,destinationBase,
            0,&offset,buffer,6,&written)==Bc250GfxBltNoSpace && !written && !offset);
        CHECK(Bc250EmitGfxBltList(&s,&d,&sr,&dr,0,0,sourceBase,destinationBase,
            6,&offset,buffer,64,&written)==Bc250GfxBltInvalid && !written && offset==6);
        CHECK(Bc250EmitGfxBltList(&s,&d,&sr,&dr,0,0,sourceBase,destinationBase,
            5,&offset,0,0,&written)==Bc250GfxBltDone && !written && offset==5);
    }
    {
        BC250_BLIT_SURFACE huge={0,2,0,0,Bc250BltBgra8};
        BC250_BLIT_RECT full;
        unsigned int offset=0;
        huge.Width=(maximum+64u)/4u;huge.Pitch=maximum+80u;
        huge.Bytes=(unsigned long long)huge.Pitch*huge.Height;
        full.Left=full.Top=0;full.Right=(int)huge.Width;full.Bottom=2;
        for(i=0;i<4;i++) {
            result=Bc250EmitGfxBltList(&huge,&huge,&full,&full,0,0,sourceBase,destinationBase,
                offset,&offset,buffer,7,&written);
            CHECK(result==(i==3?Bc250GfxBltDone:Bc250GfxBltMore) && offset==i+1 && written==7);
            CHECK((buffer[6]&S_506_BYTE_COUNT(~0u))==((i&1)?64:maximum));
            CHECK((((unsigned long long)buffer[3]<<32)|buffer[2])==
                sourceBase+(unsigned long long)(i/2)*huge.Pitch+(i&1)*maximum);
        }
        /* More than UINT packets cannot be represented by MultipassOffset. */
        huge.Height=0x7fffffffu;huge.Width=maximum/4u+1u;huge.Pitch=huge.Width*4;
        huge.Bytes=(unsigned long long)huge.Pitch*huge.Height;
        full.Right=(int)huge.Width;full.Bottom=(int)huge.Height;
        /* Two packets/row still fit UINT; a third packet makes the total overflow. */
        huge.Width=maximum/2u+1u;huge.Pitch=huge.Width*4;
        huge.Bytes=(unsigned long long)huge.Pitch*huge.Height;full.Right=(int)huge.Width;
        memset(buffer,0xcc,sizeof(buffer));memcpy(saved,buffer,sizeof(buffer));
        CHECK(Bc250EmitGfxBltList(&huge,&huge,&full,&full,0,0,0,1ull<<62,
            0,&offset,buffer,64,&written)==Bc250GfxBltInvalid);
        CHECK(!offset && !written && !memcmp(buffer,saved,sizeof(buffer)));
    }
    {
        unsigned char record[32],original[32];
        memset(record,0xcc,sizeof(record));memcpy(original,record,sizeof(record));
        CHECK(!Bc250GfxPresentRecord(record,23,sourceBase,32));
        CHECK(!Bc250GfxPresentRecord(record,32,sourceBase+1,32));
        CHECK(!Bc250GfxPresentRecord(record,32,sourceBase,28));
        CHECK(!Bc250GfxPresentRecord(record,32,~0ull-3,32));
        CHECK(!memcmp(record,original,sizeof(record)));
        CHECK(Bc250GfxPresentRecord(record,32,sourceBase,64));
        CHECK(!memcmp(record+24,original+24,8));
        CHECK(Bc250GfxPresentMatches(record,24,sourceBase,64));
        CHECK(!Bc250GfxPresentMatches(record,23,sourceBase,64));
        CHECK(!Bc250GfxPresentMatches(record,24,sourceBase+4,64));
        CHECK(!Bc250GfxPresentMatches(record,24,sourceBase,32));
        memcpy(original,record,sizeof(record));
        for(i=0;i<24;i++){
            record[i]^=1;CHECK(!Bc250GfxPresentMatches(record,24,sourceBase,64));
            memcpy(record,original,sizeof(record));
        }
    }
    puts("PASS Present private-record binding, truncation, mutation and range checks");
    puts("PASS full dirty-list prevalidation and packet-ordinal multipass at30 capacities");
    puts("PASS decoded row copies at30 capacities, padding, intra-row resume, batch-only sync, whole-footprint rejection");
    return 0;
}
