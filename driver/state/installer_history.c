#include <string.h>
#include "installer_history.h"
#include "catalog_history.generated.h"
typedef char HistoryHeaderSize[(sizeof(AMDGPU_TRANSFER_HEADER)==360)?1:-1];
// The same checksum as transfer.c (FNV-1a over the record, the checksum DWORD read as zero). transfer.c is also
// built into the KMD, so this user-mode reader keeps its own copy.
static unsigned int Sum(const unsigned char *p,unsigned int bytes)
{
    unsigned int i,sum=2166136261u;
    for(i=0;i<bytes;i++){sum^=(i>=28 && i<32)?0:p[i];sum*=16777619u;}
    return sum;
}
static unsigned int Fold(unsigned int c){return c>='A' && c<='Z'?c+('a'-'A'):c;}
// The comparison of AmdgpuStateFind: ASCII letters without case.
static int Same(const char *a,const char *b)
{
    while(*a && *b){if(Fold((unsigned char)*a++)!=Fold((unsigned char)*b++))return 0;}
    return *a==*b;
}
unsigned int AmdgpuHistoryFrame(const void *buffer,unsigned int bytes)
{
    AMDGPU_TRANSFER_HEADER h;
    if(!buffer || bytes<sizeof(h) || bytes>AMDGPU_TRANSFER_MAX_BYTES)return AMDGPU_STATE_SCHEMA;
    memcpy(&h,buffer,sizeof(h));
    return h.magic==AMDGPU_TRANSFER_MAGIC && h.version==AMDGPU_TRANSFER_VERSION && h.bytes==bytes &&
        h.checksum==Sum((const unsigned char*)buffer,bytes)?AMDGPU_STATE_OK:AMDGPU_STATE_SCHEMA;
}
unsigned int AmdgpuHistoryCheck(const void *buffer,unsigned int bytes,unsigned int *catalog)
{
    const unsigned char *p=(const unsigned char*)buffer;AMDGPU_TRANSFER_HEADER h;
    unsigned int c,i,used=sizeof(h),count=0;
    *catalog=AMDGPU_HISTORY_NONE;
    if(AmdgpuHistoryFrame(buffer,bytes))return AMDGPU_STATE_SCHEMA;
    memcpy(&h,p,sizeof(h));
    for(c=0;c<AMDGPU_STATE_HISTORY_COUNT;c++)if(!memcmp(h.catalog,StateHistory[c].catalog,64))break;
    if(c==AMDGPU_STATE_HISTORY_COUNT)return AMDGPU_STATE_UNKNOWN;
    // The rules of AmdgpuTransferCheck, against the earlier catalog: one entry for each persistent value, in order.
    for(i=0;i<StateHistory[c].count;i++) {
        const AMDGPU_STATE_HISTORY_NAME *n=&StateHistory[c].names[i];AMDGPU_TRANSFER_ENTRY e;
        if(n->kind==AMDGPU_STATE_VOLATILE)continue;
        if(bytes-used<sizeof(e))return AMDGPU_STATE_SCHEMA;
        memcpy(&e,p+used,sizeof(e));used+=sizeof(e);
        if(e.index!=i || e.present>1 || e.bytes>bytes-used || e.bytes>n->max_bytes ||
           (!e.present && (e.type || e.bytes)) || (e.present && !e.bytes))return AMDGPU_STATE_SCHEMA;
        used+=e.bytes;count++;
    }
    if(used!=bytes || count!=h.count)return AMDGPU_STATE_SCHEMA;
    *catalog=c;return AMDGPU_STATE_OK;
}
unsigned int AmdgpuHistoryFind(unsigned int catalog,const void *buffer,unsigned int bytes,
    const AMDGPU_STATE_SPEC *spec,AMDGPU_STATE_VALUE *value,const void **data)
{
    const unsigned char *p=(const unsigned char*)buffer;unsigned int used=sizeof(AMDGPU_TRANSFER_HEADER);
    if(!value || !data || !spec || catalog>=AMDGPU_STATE_HISTORY_COUNT)return AMDGPU_STATE_INVALID_ARGUMENT;
    memset(value,0,sizeof(*value));*data=0;
    while(used+sizeof(AMDGPU_TRANSFER_ENTRY)<=bytes) {
        AMDGPU_TRANSFER_ENTRY e;const AMDGPU_STATE_HISTORY_NAME *n;
        memcpy(&e,p+used,sizeof(e));used+=sizeof(e);
        if(e.index>=StateHistory[catalog].count || e.bytes>bytes-used)return AMDGPU_STATE_SCHEMA;
        n=&StateHistory[catalog].names[e.index];
        if(Same(n->path,spec->path) && Same(n->name,spec->name)) {
            value->status=e.present?AMDGPU_STATE_OK:AMDGPU_STATE_ABSENT;value->type=e.type;value->bytes=e.bytes;
            if(e.present){*data=p+used;value->source=AMDGPU_STATE_OVERRIDE;}
            return value->status;
        }
        used+=e.bytes;
    }
    value->status=AMDGPU_STATE_ABSENT;return AMDGPU_STATE_ABSENT;
}
