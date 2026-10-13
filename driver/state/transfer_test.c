#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "transfer.h"
static unsigned int checks,failures,fail_call,read_call;
static int empty;
static unsigned int final_record,final_cpu;
static unsigned int write_call,fail_write,releases,phase,put_count,cpu_present,cpu_value;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static unsigned int Fetch(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,void *data,unsigned int capacity)
{
    unsigned int off=final_cpu;
    (void)context;(void)source;
    if(++read_call==fail_call) return AMDGPU_STATE_ACCESS;
    if(spec->kind==AMDGPU_STATE_VOLATILE) { CHECK(0);return AMDGPU_STATE_IO; }
    if(final_record && !strcmp(spec->path,"StopRecord") && !strcmp(spec->name,"BootId")) {
        if(capacity<4)return AMDGPU_STATE_BUFFER;
        value->type=4;value->bytes=4;memcpy(data,&final_record,4);return 0;
    }
    if(empty || strcmp(spec->name,"CpuTune")) return AMDGPU_STATE_ABSENT;
    if(capacity<4) return AMDGPU_STATE_BUFFER;
    value->type=4;value->bytes=4;memcpy(data,&off,4);return AMDGPU_STATE_OK;
}
static unsigned int Store(void *context,unsigned int operation,const AMDGPU_TRANSFER_HEADER *header,
    const AMDGPU_STATE_SPEC *spec,const AMDGPU_STATE_VALUE *value,const void *data)
{
    (void)context;(void)header;
    if(operation==AMDGPU_TRANSFER_RELEASE) { releases++;return 0; }
    if(++write_call==fail_write) return AMDGPU_STATE_IO;
    if(operation==AMDGPU_TRANSFER_BEGIN) { CHECK(phase==0);phase=operation; }
    else if(operation==AMDGPU_TRANSFER_PUT) {
        CHECK(phase==AMDGPU_TRANSFER_BEGIN);put_count++;
        CHECK(spec && spec->kind!=AMDGPU_STATE_VOLATILE);
        if(!strcmp(spec->name,"CpuTune")) {
            cpu_present=value->status==0;
            if(cpu_present) memcpy(&cpu_value,data,4);
        }
    } else {
        CHECK(operation==phase+1 || (phase==AMDGPU_TRANSFER_BEGIN && operation==AMDGPU_TRANSFER_FLUSH_DATA));
        phase=operation;
    }
    return 0;
}
static void ResetStore(void)
{ write_call=releases=phase=put_count=cpu_present=cpu_value=0; }
int main(void)
{
    AMDGPU_TRANSFER_HEADER h={0},read;
    AMDGPU_STATE_VALUE value;const void *data;
    unsigned char *p=malloc(AMDGPU_TRANSFER_MAX_BYTES),*copy=malloc(AMDGPU_TRANSFER_MAX_BYTES);
    unsigned char boot[16]={2};unsigned int bytes=0,n=0,i,cpu=0,root_reads,groups,failed,write_count;
    CHECK(p && copy);if(!p || !copy) return 1;
    h.origin=AMDGPU_TRANSFER_LEGACY;h.source_schema=1;
    h.transaction[0]=1;h.device_hash[0]=2;h.source_boot[0]=3;
    memcpy(h.catalog,AmdgpuStateCatalogId(),64);
    strcpy_s(h.source_version,32,"0.7.216.101");strcpy_s(h.destination_version,32,"0.7.216.102");
    strcpy_s(h.destination_version,32,"65536.7.216.102");
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&bytes)==AMDGPU_STATE_SCHEMA);
    memset(h.destination_version,0,32);
    strcpy_s(h.destination_version,32,"0.7.216.102");
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&bytes)==0);
    root_reads=read_call;
    CHECK(AmdgpuTransferCheck(p,bytes,&read)==0 && read.count==root_reads);
    CHECK(AmdgpuTransferBind(&read,h.device_hash,h.transaction,boot,h.destination_version)==0);
    CHECK(AmdgpuTransferBind(&read,h.device_hash,h.transaction,h.source_boot,h.destination_version)==AMDGPU_STATE_INCOMPLETE);
    CHECK(AmdgpuTransferBind(&read,h.device_hash,boot,boot,h.destination_version)==AMDGPU_STATE_SCHEMA);
    CHECK(AmdgpuTransferBind(&read,h.device_hash,h.transaction,boot,"0.7.216.103")==AMDGPU_STATE_SCHEMA);
    for(i=0;i<AmdgpuStateCount();i++) if(!strcmp(AmdgpuStateAt(i)->name,"CpuTune")) cpu=i;
    CHECK(AmdgpuTransferFind(p,bytes,cpu,&value,&data)==0 && value.bytes==4 && !memcmp(data,"\0\0\0\0",4));
    final_record=73;
    CHECK(AmdgpuTransferMergeFinal(p,bytes,Fetch,NULL,copy,AMDGPU_TRANSFER_MAX_BYTES,&n,&failed)==0);
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(i);
        if(!strcmp(spec->path,"StopRecord") && !strcmp(spec->name,"BootId")) {
            CHECK(AmdgpuTransferFind(copy,n,i,&value,&data)==0 && value.bytes==4 && !memcmp(data,&final_record,4));
        }
    }
    final_cpu=1;
    CHECK(AmdgpuTransferMergeFinal(p,bytes,Fetch,NULL,copy,AMDGPU_TRANSFER_MAX_BYTES,&n,&failed)==AMDGPU_STATE_SCHEMA && !n && failed==cpu);
    final_cpu=final_record=0;
    // Every persistent field is represented; volatile records are never copied.
    for(i=0;i<AmdgpuStateCount();i++) {
        unsigned int want=AmdgpuStateAt(i)->kind==AMDGPU_STATE_VOLATILE?AMDGPU_STATE_UNKNOWN:
                          i==cpu?AMDGPU_STATE_OK:AMDGPU_STATE_ABSENT;
        CHECK(AmdgpuTransferFind(p,bytes,i,&value,&data)==want);
    }
    for(i=0;i<bytes;i++) {
        p[i]^=1;CHECK(AmdgpuTransferCheck(p,bytes,NULL)!=0);p[i]^=1;
        CHECK(AmdgpuTransferCheck(p,i,NULL)!=0);
    }
    CHECK(AmdgpuTransferCheck(p,bytes+1,NULL)!=0);
    memcpy(copy,p,bytes);
    for(i=1;i<=root_reads;i++) {
        read_call=0;fail_call=i;n=1;
        CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,copy,AMDGPU_TRANSFER_MAX_BYTES,&n)==AMDGPU_STATE_ACCESS && n==0);
        CHECK(AmdgpuTransferCheck(copy,bytes,NULL)!=0);
    }
    fail_call=0;
    for(i=sizeof(h);i<bytes;i++) {
        n=1;CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,copy,i,&n)==AMDGPU_STATE_BUFFER && n==0);
    }
    // Legacy absent CpuTune means off; only fresh install receives factory on.
    empty=1;
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&bytes)==0);
    ResetStore();
    CHECK(AmdgpuTransferApply(p,bytes,Store,NULL,&groups,&failed)==0 && releases==1 &&
          phase==AMDGPU_TRANSFER_FLUSH_ACTIVE && cpu_present && cpu_value==0 && put_count==root_reads);
    write_count=write_call;
    for(i=1;i<=write_count;i++) {
        ResetStore();fail_write=i;
        CHECK(AmdgpuTransferApply(p,bytes,Store,NULL,&groups,&failed)==AMDGPU_STATE_IO && releases==1);
        CHECK(write_call==i && phase<AMDGPU_TRANSFER_FLUSH_ACTIVE);
    }
    fail_write=0;empty=0;
    h.origin=AMDGPU_TRANSFER_FRESH;h.source_schema=0;memset(h.source_version,0,32);
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==AMDGPU_STATE_SCHEMA);
    empty=1;CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==0);
    CHECK(AmdgpuTransferCheck(p,n,NULL)==0);
    ResetStore();CHECK(AmdgpuTransferApply(p,n,Store,NULL,&groups,&failed)==0 && !cpu_present);
    h.board_record[0]=1;
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==AMDGPU_STATE_SCHEMA);
    h.board_record[0]=0;h.origin=AMDGPU_TRANSFER_CURRENT;h.source_schema=2;
    strcpy_s(h.source_version,32,"0.7.216.101");
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==AMDGPU_STATE_SCHEMA);
    h.source_generation[0]=1;
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==0);
    h.origin=AMDGPU_TRANSFER_ARCHIVE;h.board_kind=AMDGPU_BOARD_UNPROVEN;
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==0);
    CHECK(AmdgpuTransferCheck(p,n,NULL)==0);
    h.board_kind=AMDGPU_BOARD_FRESH;
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==AMDGPU_STATE_SCHEMA);
    h.board_kind=AMDGPU_BOARD_UNPROVEN;h.source_generation[0]=0;
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==AMDGPU_STATE_SCHEMA);
    h.source_generation[0]=1;
    h.reserved[7]=1;
    CHECK(AmdgpuTransferCapture(&h,Fetch,NULL,p,AMDGPU_TRANSFER_MAX_BYTES,&n)==AMDGPU_STATE_SCHEMA);
    free(copy);free(p);
    printf("%u transfer checks, %u failures\n",checks,failures);return failures?1:0;
}
