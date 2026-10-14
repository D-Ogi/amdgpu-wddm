#include "transfer.h"
// Structures have no pointers or compiler-dependent long fields. Untrusted input
// is copied before inspection, so its address need not be aligned.
typedef char HeaderSize[(sizeof(AMDGPU_TRANSFER_HEADER)==360)?1:-1];
typedef char EntrySize[(sizeof(AMDGPU_TRANSFER_ENTRY)==16)?1:-1];
static void Copy(void *out,const void *in,unsigned int bytes)
{ unsigned int i;for(i=0;i<bytes;i++) ((unsigned char*)out)[i]=((const unsigned char*)in)[i]; }
static void Zero(void *out,unsigned int bytes)
{ unsigned int i;for(i=0;i<bytes;i++) ((unsigned char*)out)[i]=0; }
static int Same(const void *a,const void *b,unsigned int bytes)
{ unsigned int i;for(i=0;i<bytes;i++) if(((const unsigned char*)a)[i]!=((const unsigned char*)b)[i]) return 0;return 1; }
static int Any(const void *a,unsigned int bytes)
{ unsigned int i;for(i=0;i<bytes;i++) if(((const unsigned char*)a)[i]) return 1;return 0; }
static unsigned int Sum(const unsigned char *p,unsigned int bytes)
{
    unsigned int i,sum=2166136261u;
    // checksum is the eighth DWORD; no offsetof/CRT dependency in kernel mode.
    for(i=0;i<bytes;i++) { sum^=(i>=28 && i<32)?0:p[i];sum*=16777619u; }
    return sum;
}
void AmdgpuStateBootToken(unsigned int id,unsigned char token[16])
{
    static const unsigned char tag[8]={'W','I','N','B','O','O','T','2'};unsigned int i;
    Zero(token,16);Copy(token,tag,8);
    for(i=0;i<4;i++)token[8+i]=(unsigned char)(id>>(8*i));
}
static int Version(const char *v,int optional)
{
    unsigned int i,dots=0,digits=0,part=0;
    if(!v[0]) return optional && !Any(v,32);
    for(i=0;i<32;i++) {
        if(!v[i]) return dots==3 && digits && !Any(v+i,32-i);
        if(v[i]=='.') { if(!digits || dots==3) return 0;dots++;digits=0;part=0; }
        else if(v[i]>='0' && v[i]<='9') {
            if(++digits>5) return 0;
            part=part*10+(unsigned int)(v[i]-'0');if(part>65535) return 0;
        }
        else return 0;
    }
    return 0;
}
static unsigned int Metadata(const AMDGPU_TRANSFER_HEADER *h)
{
    if(h->origin<AMDGPU_TRANSFER_FRESH || h->origin>AMDGPU_TRANSFER_ARCHIVE ||
       h->board_kind>AMDGPU_BOARD_LEGACY || Any(h->reserved,sizeof(h->reserved)) ||
       !Any(h->transaction,16) || !Any(h->source_boot,16) ||
       !Version(h->source_version,h->origin==AMDGPU_TRANSFER_FRESH) || !Version(h->destination_version,0) ||
       !Same(h->catalog,AmdgpuStateCatalogId(),64)) return AMDGPU_STATE_SCHEMA;
    // An INF-only factory initialization has no external device snapshot and
    // cannot authorize board writes. Every installer handoff binds a device.
    if(!Any(h->device_hash,32) && !(h->origin==AMDGPU_TRANSFER_FRESH && h->board_kind==AMDGPU_BOARD_UNPROVEN))
        return AMDGPU_STATE_SCHEMA;
    if(h->origin==AMDGPU_TRANSFER_FRESH) {
        if(h->source_schema || Any(h->source_generation,16) || Any(h->source_version,32) ||
           h->board_kind==AMDGPU_BOARD_LEGACY) return AMDGPU_STATE_SCHEMA;
    } else if(h->origin==AMDGPU_TRANSFER_LEGACY) {
        if(h->source_schema!=1 || Any(h->source_generation,16)) return AMDGPU_STATE_SCHEMA;
    } else if(h->source_schema!=AmdgpuStateSchema() || !Any(h->source_generation,16)) return AMDGPU_STATE_SCHEMA;
    if(h->origin==AMDGPU_TRANSFER_ARCHIVE && h->board_kind!=AMDGPU_BOARD_UNPROVEN)return AMDGPU_STATE_SCHEMA;
    if(h->board_kind!=AMDGPU_BOARD_LEGACY && Any(h->board_record,sizeof(h->board_record))) return AMDGPU_STATE_SCHEMA;
    return AMDGPU_STATE_OK;
}
unsigned int AmdgpuTransferCapture(const AMDGPU_TRANSFER_HEADER *metadata,
    AMDGPU_STATE_FETCH fetch,void *context,void *buffer,unsigned int capacity,unsigned int *bytes)
{
    AMDGPU_TRANSFER_HEADER h;unsigned char *p=(unsigned char*)buffer;
    unsigned int i,used=sizeof(h),count=0,result;
    if(!bytes) return AMDGPU_STATE_INVALID_ARGUMENT;
    *bytes=0;
    if(!metadata || !fetch || !buffer || capacity<sizeof(h) || capacity>AMDGPU_TRANSFER_MAX_BYTES)
        return AMDGPU_STATE_INVALID_ARGUMENT;
    h=*metadata;h.magic=AMDGPU_TRANSFER_MAGIC;h.version=AMDGPU_TRANSFER_VERSION;
    h.bytes=h.count=h.checksum=0;
    result=Metadata(&h);if(result) return result;
    Zero(buffer,sizeof(h)); // No partially captured file has a valid magic.
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(i);AMDGPU_TRANSFER_ENTRY e;AMDGPU_STATE_VALUE v;
        if(spec->kind==AMDGPU_STATE_VOLATILE) continue;
        if(capacity-used<sizeof(e)) return AMDGPU_STATE_BUFFER;
        Zero(&v,sizeof(v));Zero(&e,sizeof(e));e.index=i;
        result=fetch(context,AMDGPU_STATE_OVERRIDE,spec,&v,p+used+sizeof(e),capacity-used-sizeof(e));
        if(result!=AMDGPU_STATE_OK && result!=AMDGPU_STATE_ABSENT) return result;
        if(result==AMDGPU_STATE_OK) {
            // Retain a wrong type for diagnostic attribution, but never accept an
            // unbounded length from a registry value or a malicious callback.
            if(!v.bytes || v.bytes>spec->max_bytes || v.bytes>capacity-used-sizeof(e)) return AMDGPU_STATE_RANGE;
            if(h.origin==AMDGPU_TRANSFER_FRESH) return AMDGPU_STATE_SCHEMA;
            e.present=1;e.type=v.type;e.bytes=v.bytes;
        }
        Copy(p+used,&e,sizeof(e));used+=sizeof(e)+e.bytes;count++;
    }
    h.bytes=used;h.count=count;Copy(buffer,&h,sizeof(h));
    h.checksum=Sum(buffer,used);Copy(buffer,&h,sizeof(h));*bytes=used;
    return AMDGPU_STATE_OK;
}
unsigned int AmdgpuTransferCheck(const void *buffer,unsigned int bytes,AMDGPU_TRANSFER_HEADER *header)
{
    AMDGPU_TRANSFER_HEADER h;const unsigned char *p=(const unsigned char*)buffer;
    unsigned int i,used=sizeof(h),count=0,result;
    if(!p || bytes<sizeof(h) || bytes>AMDGPU_TRANSFER_MAX_BYTES) return AMDGPU_STATE_SCHEMA;
    Copy(&h,p,sizeof(h));
    if(h.magic!=AMDGPU_TRANSFER_MAGIC || h.version!=AMDGPU_TRANSFER_VERSION || h.bytes!=bytes ||
       h.checksum!=Sum(p,bytes)) return AMDGPU_STATE_SCHEMA;
    result=Metadata(&h);if(result) return result;
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(i);AMDGPU_TRANSFER_ENTRY e;
        if(spec->kind==AMDGPU_STATE_VOLATILE) continue;
        if(bytes-used<sizeof(e)) return AMDGPU_STATE_INCOMPLETE;
        Copy(&e,p+used,sizeof(e));used+=sizeof(e);
        if(e.index!=i || e.present>1 || e.bytes>bytes-used || e.bytes>spec->max_bytes ||
           (!e.present && (e.type || e.bytes)) || (e.present && !e.bytes) ||
           (e.present && h.origin==AMDGPU_TRANSFER_FRESH)) return AMDGPU_STATE_SCHEMA;
        used+=e.bytes;count++;
    }
    if(used!=bytes || count!=h.count) return AMDGPU_STATE_SCHEMA;
    if(header) *header=h;
    return AMDGPU_STATE_OK;
}
unsigned int AmdgpuTransferFind(const void *buffer,unsigned int bytes,unsigned int index,
    AMDGPU_STATE_VALUE *value,const void **data)
{
    const unsigned char *p=(const unsigned char*)buffer;unsigned int result,used=sizeof(AMDGPU_TRANSFER_HEADER);
    if(!value || !data) return AMDGPU_STATE_INVALID_ARGUMENT;
    Zero(value,sizeof(*value));*data=0;
    result=AmdgpuTransferCheck(buffer,bytes,0);
    if(result) { value->status=result;return result; }
    while(used<bytes) {
        AMDGPU_TRANSFER_ENTRY e;Copy(&e,p+used,sizeof(e));used+=sizeof(e);
        if(e.index==index) {
            value->status=e.present?AMDGPU_STATE_OK:AMDGPU_STATE_ABSENT;
            value->type=e.type;value->bytes=e.bytes;
            if(e.present) { *data=p+used;value->source=AMDGPU_STATE_OVERRIDE; }
            return value->status;
        }
        used+=e.bytes;
    }
    value->status=AMDGPU_STATE_UNKNOWN;return value->status;
}
typedef struct merge_context {
    const unsigned char *input;
    unsigned int used,conflict;
    AMDGPU_STATE_FETCH fetch;
    void *context;
} MERGE_CONTEXT;
static unsigned int MergeFetch(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,void *data,unsigned int capacity)
{
    MERGE_CONTEXT *m=(MERGE_CONTEXT*)context;AMDGPU_TRANSFER_ENTRY old;
    unsigned int result;
    Copy(&old,m->input+m->used,sizeof(old));m->used+=sizeof(old);
    // The validated source and Capture enumerate the same persistent catalog.
    result=m->fetch(m->context,source,spec,value,data,capacity);
    if(result!=AMDGPU_STATE_OK && result!=AMDGPU_STATE_ABSENT) {m->conflict=old.index;return result;}
    if(result==AMDGPU_STATE_OK && value->bytes>capacity) {m->conflict=old.index;return AMDGPU_STATE_RANGE;}
    if(spec->kind==AMDGPU_STATE_SETTING &&
       ((unsigned int)(result==AMDGPU_STATE_OK)!=old.present ||
        (old.present && (old.type!=value->type || old.bytes!=value->bytes ||
                         !Same(data,m->input+m->used,old.bytes))))) {
        m->conflict=old.index;return AMDGPU_STATE_SCHEMA;
    }
    m->used+=old.bytes;return result;
}
unsigned int AmdgpuTransferMergeFinal(const void *snapshot,unsigned int snapshot_bytes,
    AMDGPU_STATE_FETCH stopped_fetch,void *context,void *out,unsigned int capacity,
    unsigned int *bytes,unsigned int *conflict_index)
{
    AMDGPU_TRANSFER_HEADER h;MERGE_CONTEXT merge;unsigned int result;
    if(!bytes || !conflict_index) return AMDGPU_STATE_INVALID_ARGUMENT;
    *bytes=0;*conflict_index=~0u;
    if(!stopped_fetch || !out || snapshot==out) return AMDGPU_STATE_INVALID_ARGUMENT;
    result=AmdgpuTransferCheck(snapshot,snapshot_bytes,&h);if(result)return result;
    if(h.origin==AMDGPU_TRANSFER_FRESH)return AMDGPU_STATE_INVALID_ARGUMENT;
    Zero(&merge,sizeof(merge));merge.input=snapshot;merge.used=sizeof(h);
    merge.fetch=stopped_fetch;merge.context=context;merge.conflict=~0u;
    result=AmdgpuTransferCapture(&h,MergeFetch,&merge,out,capacity,bytes);
    *conflict_index=merge.conflict;return result;
}
unsigned int AmdgpuTransferBind(const AMDGPU_TRANSFER_HEADER *h,const unsigned char device[32],
    const unsigned char transaction[16],const unsigned char boot[16],const char *version)
{
    unsigned int i;
    if(!h || !device || !transaction || !boot || !version) return AMDGPU_STATE_INVALID_ARGUMENT;
    if(Metadata(h) || !Same(h->device_hash,device,32) || !Same(h->transaction,transaction,16)) return AMDGPU_STATE_SCHEMA;
    for(i=0;i<32;i++) {
        if(h->destination_version[i]!=version[i]) return AMDGPU_STATE_SCHEMA;
        if(!version[i]) break;
    }
    if(i==32) return AMDGPU_STATE_SCHEMA;
    // The legacy first migration must follow an OS restart, never a device reset.
    if(h->origin==AMDGPU_TRANSFER_LEGACY && Same(h->source_boot,boot,16)) return AMDGPU_STATE_INCOMPLETE;
    return AMDGPU_STATE_OK;
}
unsigned int AmdgpuTransferApply(const void *buffer,unsigned int bytes,AMDGPU_TRANSFER_STORE store,void *context,
    unsigned int *invalid_groups,unsigned int *failed_index)
{
    AMDGPU_TRANSFER_HEADER h;const unsigned char *p=(const unsigned char*)buffer;
    unsigned int result,release,used=sizeof(h),groups=0,operation;
    if(!store || !invalid_groups || !failed_index) return AMDGPU_STATE_INVALID_ARGUMENT;
    *invalid_groups=0;*failed_index=~0u;
    result=AmdgpuTransferCheck(buffer,bytes,&h);if(result) return result;
    // No persistent write before structural and semantic validation has finished.
    while(used<bytes) {
        AMDGPU_TRANSFER_ENTRY e;const AMDGPU_STATE_SPEC *spec;
        Copy(&e,p+used,sizeof(e));used+=sizeof(e);spec=AmdgpuStateAt(e.index);
        if(e.present) {
            result=AmdgpuStateValidate(spec,e.type,p+used,e.bytes);
            if(result) {
                if(spec->kind!=AMDGPU_STATE_SETTING || spec->group==AMDGPU_STATE_GROUP_NONE) {
                    *failed_index=e.index;return result;
                }
                groups|=1u<<spec->group;
            }
        }
        used+=e.bytes;
    }
    *invalid_groups=groups;
    result=store(context,AMDGPU_TRANSFER_BEGIN,&h,0,0,0);
    used=sizeof(h);
    while(!result && used<bytes) {
        AMDGPU_TRANSFER_ENTRY e;AMDGPU_STATE_VALUE v;const AMDGPU_STATE_SPEC *spec;
        const void *payload;unsigned char off[4]={0,0,0,0};
        Copy(&e,p+used,sizeof(e));used+=sizeof(e);spec=AmdgpuStateAt(e.index);payload=p+used;
        Zero(&v,sizeof(v));v.status=e.present?AMDGPU_STATE_OK:AMDGPU_STATE_ABSENT;
        v.type=e.type;v.bytes=e.bytes;v.source=e.present?AMDGPU_STATE_OVERRIDE:AMDGPU_STATE_NONE;
        if(h.origin==AMDGPU_TRANSFER_LEGACY && !e.present && spec->legacy_off) {
            v.status=AMDGPU_STATE_OK;v.type=AMDGPU_STATE_DWORD;v.bytes=4;
            v.source=AMDGPU_STATE_OVERRIDE;payload=off;
        }
        result=store(context,AMDGPU_TRANSFER_PUT,&h,spec,&v,payload);
        if(result) *failed_index=e.index;
        used+=e.bytes;
    }
    for(operation=AMDGPU_TRANSFER_FLUSH_DATA;!result && operation<=AMDGPU_TRANSFER_FLUSH_ACTIVE;operation++)
        result=store(context,operation,&h,0,0,0);
    release=store(context,AMDGPU_TRANSFER_RELEASE,&h,0,0,0);
    return result?result:release;
}
