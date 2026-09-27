/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../blit_plan.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)

int main(void)
{
    BC250_BLIT_SURFACE src={8,7,40,280,Bc250BltBgra8};
    BC250_BLIT_SURFACE dst={11,9,48,432,Bc250BltBgra8};
    BC250_BLIT_RECT sr={2,1,7,6},dr={4,3,9,8},dirty;
    BC250_BLIT_PLAN p;
    unsigned char source[280], actual[432], expected[432];
    unsigned int i,x,y,row,cases=0;
    int l,t,r,b;
    for(i=0;i<sizeof(source);i++) source[i]=(unsigned char)(i*37u+13u);
    /* Independent pixel oracle uses absolute coordinates. Padding and unaffected
     * pixels must stay sentinel-filled. Includes empty/disjoint/overlapping clips. */
    for(l=0;l<=11;l++) for(r=l;r<=11;r++)
    for(t=0;t<=9;t++) for(b=t;b<=9;b++) {
        BC250_BLIT_RESULT result;
        int hasPixels=0;
        dirty.Left=l;dirty.Top=t;dirty.Right=r;dirty.Bottom=b;
        memset(actual,0xcc,sizeof(actual));memset(expected,0xcc,sizeof(expected));
        result=Bc250PlanBlt(&src,&dst,&sr,&dr,&dirty,&p);
        CHECK(result!=Bc250BltInvalid);
        for(y=0;y<dst.Height;y++) for(x=0;x<dst.Width;x++) {
            if((int)x>=l && (int)x<r && (int)y>=t && (int)y<b &&
               (int)x>=dr.Left && (int)x<dr.Right && (int)y>=dr.Top && (int)y<dr.Bottom) {
                unsigned int sx=x-4u+2u,sy=y-3u+1u;
                memcpy(expected+y*dst.Pitch+x*4u,source+sy*src.Pitch+sx*4u,4);
                hasPixels=1;
            }
        }
        CHECK(result==(hasPixels ? Bc250BltCopy : Bc250BltEmpty));
        if(result==Bc250BltCopy) for(row=0;row<p.Rows;row++)
            memcpy(actual+p.DestinationOffset+(unsigned long long)row*p.DestinationPitch,
                   source+p.SourceOffset+(unsigned long long)row*p.SourcePitch,p.RowBytes);
        else CHECK(!p.Rows && !p.RowBytes && !p.SourceOffset && !p.DestinationOffset);
        CHECK(memcmp(actual,expected,sizeof(actual))==0);cases++;
    }
    CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,NULL,&p)==Bc250BltCopy && p.Rows==5 && p.RowBytes==20);
    dst.Bytes--;CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,NULL,&p)==Bc250BltInvalid && p.Rows==0);dst.Bytes++;
    dst.Format=Bc250BltRgba8;CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,NULL,&p)==Bc250BltInvalid);dst.Format=Bc250BltBgra8;
    dr.Right++;CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,NULL,&p)==Bc250BltInvalid);dr.Right--;
    dirty=dr;dirty.Left=-1;CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,&dirty,&p)==Bc250BltInvalid);
    dirty=dr;dirty.Right=12;CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,&dirty,&p)==Bc250BltInvalid);
    sr.Left=INT_MIN;CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,NULL,&p)==Bc250BltInvalid);sr.Left=2;
    src.Width=UINT_MAX;CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,NULL,&p)==Bc250BltInvalid);
    CHECK(Bc250PlanBlt(NULL,&dst,&sr,&dr,NULL,&p)==Bc250BltInvalid);
    CHECK(Bc250PlanBlt(&dst,&dst,&dr,&dr,NULL,NULL)==Bc250BltInvalid);
    /* Large byte offsets must not truncate at32 bits. */
    src.Width=dst.Width=1;src.Height=dst.Height=INT_MAX;
    src.Pitch=dst.Pitch=0xfffffffcu;
    src.Bytes=dst.Bytes=(unsigned long long)src.Pitch*src.Height;
    sr.Left=dr.Left=0;sr.Right=dr.Right=1;
    sr.Top=dr.Top=INT_MAX-1;sr.Bottom=dr.Bottom=INT_MAX;
    CHECK(Bc250PlanBlt(&src,&dst,&sr,&dr,NULL,&p)==Bc250BltCopy);
    CHECK(p.SourceOffset==(unsigned long long)(INT_MAX-1)*0xfffffffcu && p.Rows==1);
    printf("PASS %u independent pixel-oracle cases, padding, invalid geometry and64-bit offsets\n",cases);
    return 0;
}
