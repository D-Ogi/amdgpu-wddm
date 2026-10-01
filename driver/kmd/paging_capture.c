#include "paging_capture.h"
int PagingCaptureAttach(PAGING_CAPTURE_OWNER* Owner,PAGING_CAPTURE* Capture)
{
    PAGING_CAPTURE* p;
    if(!Owner || !Capture || Owner->LastId>=0x7fffffffu)return 0;
    for(p=Owner->Head;p;p=p->Next)if(p==Capture)return 0;
    Capture->Token=PAGING_CAPTURE_TAG|++Owner->LastId;
    Capture->Next=Owner->Head;Owner->Head=Capture;
    Owner->ActivePlans++;
    Owner->ReservedBytes+=Capture->ReservationBytes;
    if(Owner->ActivePlans>Owner->PeakPlans)Owner->PeakPlans=Owner->ActivePlans;
    if(Owner->ReservedBytes>Owner->PeakReservedBytes)Owner->PeakReservedBytes=Owner->ReservedBytes;
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
        PAGING_CAPTURE* result=*link;*link=result->Next;result->Next=0;
        Owner->ActivePlans--;Owner->ReservedBytes-=result->ReservationBytes;
        return result;
    }
    return 0;
}
PAGING_CAPTURE* PagingCaptureTakeAll(PAGING_CAPTURE_OWNER* Owner)
{
    PAGING_CAPTURE* result;
    if(!Owner)return 0;
    result=Owner->Head;Owner->Head=0;
    Owner->ActivePlans=0;Owner->ReservedBytes=0;
    // Keep LastId: cancel/drain must not make a stale resume token valid again.
    return result;
}

void* PagingCaptureStorage(const PAGING_CAPTURE_OWNER* Owner,unsigned long long Bytes)
{
    unsigned long long at=0,span;
    PAGING_CAPTURE* p;
    if(!Owner || !Owner->Storage || !Bytes || Bytes>~0ull-7)return 0;
    span=(Bytes+7)&~7ull;
    while(at<=Owner->StorageBytes && span<=Owner->StorageBytes-at) {
        for(p=Owner->Head;p;p=p->Next)if(p->ReservationBytes) {
            // Reserved headers are inside this arena; heap captures have span0.
            unsigned long long begin=(unsigned char*)p-(unsigned char*)Owner->Storage;
            if(at<begin+p->ReservationBytes && begin<at+span) {
                at=begin+p->ReservationBytes;
                break;
            }
        }
        if(!p)return (unsigned char*)Owner->Storage+at;
    }
    return 0;
}

void* PagingCaptureIdleStorage(const PAGING_CAPTURE_OWNER* Owner)
{
    return Owner?PagingCaptureStorage(Owner,Owner->StorageBytes):0;
}
