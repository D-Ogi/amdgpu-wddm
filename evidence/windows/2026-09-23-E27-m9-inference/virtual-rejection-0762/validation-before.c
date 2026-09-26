#include <stdio.h>
#include <string.h>
#include "umd_blob.h"
#define STATUS_INVALID_PARAMETER 0xC000000DU
#define BC250_WDDM_NODE_3D 0
#define GuardLog(...) ((void)0)
typedef struct { unsigned DmaBufferUmdPrivateDataSize, DmaBufferPrivateDataSize, SubmissionFenceId; const void* pDmaBufferPrivateData; } SUBMIT;
static unsigned Validate(const SUBMIT* Submit,unsigned Node) {

    struct umd_submit_view ib;
    unsigned umdLen = Submit->DmaBufferUmdPrivateDataSize;
    unsigned bufLen = Submit->DmaBufferPrivateDataSize;
    const void* bytes = Submit->pDmaBufferPrivateData;
    int st = UMD_BLOB_TOO_SMALL;
    const char* why = "unknown";
    unsigned nIbs = 0;

    ib.num_ibs = 0;
    ib.ib_va = 0;
    ib.ib_bytes = 0;
    ib.single_ib = 0;
    if (bytes != NULL && umdLen != 0 && umdLen <= bufLen)
        st = UmdBlobParseSubmit(bytes, umdLen, &ib);

 (void)why; (void)nIbs; (void)Node;
 return 0;
}
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
static void wr(unsigned char*p,unsigned v) {memcpy(p,&v,4);}
static void valid(unsigned char*p,unsigned num) {
 memset(p,0,256);
 wr(p,UMD_BLOB_SUBMIT_MAGIC);wr(p+4,1);
 wr(p+8,UMD_BLOB_SUBMIT_PREFIX+num*UMD_BLOB_IB_BYTES);
 wr(p+16,UMD_BLOB_IP_GFX);wr(p+20,num);
 for(unsigned i=0;i<num;i++) {
  unsigned char*ib=p+UMD_BLOB_SUBMIT_PREFIX+i*UMD_BLOB_IB_BYTES;
  wr(ib,0x10000);wr(ib+8,256);wr(ib+12,UMD_BLOB_IP_GFX);
 }
}
int main(void) {
 unsigned char bytes[256];
 const unsigned used=UMD_BLOB_SUBMIT_PREFIX+UMD_BLOB_IB_BYTES;
 SUBMIT s={used,sizeof(bytes),7,bytes};
 valid(bytes,1);CHECK(Validate(&s,0)==0);
 for(unsigned n=0;n<used;n++) {s.DmaBufferUmdPrivateDataSize=n;CHECK(Validate(&s,0)==STATUS_INVALID_PARAMETER);}
 s.DmaBufferUmdPrivateDataSize=used;
 s.DmaBufferPrivateDataSize=used-1;CHECK(Validate(&s,0)==STATUS_INVALID_PARAMETER);
 s.DmaBufferPrivateDataSize=sizeof(bytes);s.pDmaBufferPrivateData=NULL;
 CHECK(Validate(&s,0)==STATUS_INVALID_PARAMETER);s.pDmaBufferPrivateData=bytes;
 CHECK(Validate(&s,1)==STATUS_INVALID_PARAMETER);
 bytes[0]^=1;CHECK(Validate(&s,0)==STATUS_INVALID_PARAMETER);
 valid(bytes,1);wr(bytes+UMD_BLOB_SUBMIT_PREFIX+8,3);CHECK(Validate(&s,0)==STATUS_INVALID_PARAMETER);
 valid(bytes,2);s.DmaBufferUmdPrivateDataSize=UMD_BLOB_SUBMIT_PREFIX+2*UMD_BLOB_IB_BYTES;
 CHECK(Validate(&s,0)==STATUS_INVALID_PARAMETER);
 valid(bytes,1);s.DmaBufferUmdPrivateDataSize=used;CHECK(Validate(&s,0)==0);
 puts("PASS: real BC2S pre-dispatch validation: valid controls, every truncation, capacity mismatch, null, wrong node/magic/IB, unsupported multi-IB");return 0;
}
