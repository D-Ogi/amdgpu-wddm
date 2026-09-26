#include "paging_capture.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line%d: %s\n",__LINE__,#x);}}while(0)
#include <stdlib.h>
#define BC250_WDDM_TAG 0
static unsigned released;
typedef struct {PAGING_CAPTURE_OWNER Captures;} BC250_WDDM_OBJECT;
static void ExFreePoolWithTag(void* p,unsigned tag){(void)tag;released++;free(p);}
static void WddmReleaseCaptures(BC250_WDDM_OBJECT* Object)
{
    PAGING_CAPTURE* capture=PagingCaptureTakeAll(&Object->Captures);
    while(capture) {
        PAGING_CAPTURE* next=capture->Next;
        ExFreePoolWithTag(capture,BC250_WDDM_TAG);
        capture=next;
    }
}

int main(void)
{
    PAGING_CAPTURE_OWNER owner={0},other={0};PAGING_CAPTURE captures[8];
    unsigned tokens[8],i;PAGING_CAPTURE *p,*list;
    memset(captures,0,sizeof(captures));
    for(i=0;i<8;i++) {
        captures[i].Root=0x123000;captures[i].Source=0x10000;captures[i].Destination=0x20000;
        captures[i].Bytes=12288;captures[i].Band=i%3;captures[i].Action=i*7;
        CHECK(PagingCaptureAttach(&owner,captures+i));tokens[i]=captures[i].Token;
        CHECK(tokens[i]==(PAGING_CAPTURE_TAG|i+1));
    }
    for(i=0;i<8;i++) {
        p=PagingCaptureFind(&owner,tokens[i]);
        CHECK(p==captures+i && p->Band==i%3 && p->Action==i*7 && p->Bytes==12288);
        CHECK(!PagingCaptureFind(&other,tokens[i]));
    }
    // Interleaved completions remove head, middle and tail independently.
    CHECK(PagingCaptureDetach(&owner,tokens[7])==captures+7 && !captures[7].Next);
    CHECK(PagingCaptureDetach(&owner,tokens[3])==captures+3 && !captures[3].Next);
    CHECK(PagingCaptureDetach(&owner,tokens[0])==captures && !captures[0].Next);
    CHECK(!PagingCaptureDetach(&owner,tokens[3]) && !PagingCaptureFind(&owner,tokens[3]));
    CHECK(PagingCaptureAttach(&owner,captures+3) && captures[3].Token!=tokens[3]);
    CHECK(!PagingCaptureFind(&owner,tokens[3]));
    CHECK(!PagingCaptureAttach(&owner,captures+3));
    // Context/adapter cleanup takes exactly the remaining allocations once.
    list=PagingCaptureTakeAll(&owner);i=0;
    for(p=list;p;p=p->Next)i++;
    CHECK(i==6 && !owner.Head && !PagingCaptureTakeAll(&owner));
    CHECK(PagingCaptureAttach(&owner,captures) && captures[0].Token==(PAGING_CAPTURE_TAG|10u));
    CHECK(!PagingCaptureFind(&owner,tokens[0]));
    CHECK(PagingCaptureDetach(&owner,captures[0].Token)==captures);
    // Token exhaustion cannot alias an earlier transfer or take ownership.
    owner.LastId=0x7ffffffeu;
    CHECK(PagingCaptureAttach(&owner,captures) && captures[0].Token==0xffffffffu);
    CHECK(!PagingCaptureAttach(&owner,captures+1) && owner.Head==captures && !captures[0].Next);
    CHECK(!PagingCaptureFind(&owner,0) && !PagingCaptureFind(&owner,1));
    {BC250_WDDM_OBJECT object={0};
     for(i=0;i<8;i++) {PAGING_CAPTURE* capture=(PAGING_CAPTURE*)calloc(1,sizeof(*capture));CHECK(capture!=NULL);CHECK(PagingCaptureAttach(&object.Captures,capture));}
     WddmReleaseCaptures(&object);CHECK(released==8 && !object.Captures.Head);
     WddmReleaseCaptures(&object);CHECK(released==8 && !object.Captures.Head);
    }
    printf("%u checks, %u failures: %s\n",checks,failures,failures?"FAIL":"PASS");
    return failures?1:0;
}
