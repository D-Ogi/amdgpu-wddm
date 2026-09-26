#include "paging_capture.h"
int PagingCaptureAttach(PAGING_CAPTURE_OWNER* Owner,PAGING_CAPTURE* Capture)
{
    PAGING_CAPTURE* p;
    if(!Owner || !Capture || Owner->LastId>=0x7fffffffu)return 0;
    for(p=Owner->Head;p;p=p->Next)if(p==Capture)return 0;
    Capture->Token=PAGING_CAPTURE_TAG|++Owner->LastId;
    Capture->Next=Owner->Head;Owner->Head=Capture;
    return 1;
}
PAGING_CAPTURE* PagingCaptureFind(const PAGING_CAPTURE_OWNER* Owner,unsigned Token)
{
    PAGING_CAPTURE* p;
    if(!Owner || !(Token&PAGING_CAPTURE_TAG))return 0;
    for(p=Owner->Head;p;p=p->Next)if(p->Token==Token)return p;
    return 0;
}
PAGING_CAPTURE* PagingCaptureDetach(PAGING_CAPTURE_OWNER* Owner,unsigned Token)
{
    PAGING_CAPTURE** link;
    if(!Owner || !(Token&PAGING_CAPTURE_TAG))return 0;
    for(link=&Owner->Head;*link;link=&(*link)->Next)if((*link)->Token==Token) {
        PAGING_CAPTURE* result=*link;*link=result->Next;result->Next=0;return result;
    }
    return 0;
}
PAGING_CAPTURE* PagingCaptureTakeAll(PAGING_CAPTURE_OWNER* Owner)
{
    PAGING_CAPTURE* result;
    if(!Owner)return 0;
    result=Owner->Head;Owner->Head=0;
    // Keep LastId: cancel/drain must not make a stale resume token valid again.
    return result;
}
