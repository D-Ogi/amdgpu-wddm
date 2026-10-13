#include "state.h"
#include "catalog.generated.h"

unsigned int AmdgpuStateCount(void) { return AMDGPU_STATE_COUNT; }
unsigned int AmdgpuStateSchema(void) { return AMDGPU_STATE_SCHEMA_VERSION; }
const char *AmdgpuStateCatalogId(void) { return AMDGPU_STATE_CATALOG_ID; }
const AMDGPU_STATE_SPEC *AmdgpuStateAt(unsigned int i)
{ return i < AMDGPU_STATE_COUNT ? &StateCatalog[i] : 0; }

static unsigned int Fold(unsigned int c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; }
static int Same(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (Fold((unsigned char)*a++) != Fold((unsigned char)*b++)) return 0; }
    return *a == *b;
}
const AMDGPU_STATE_SPEC *AmdgpuStateFind(const char *path, const char *name)
{
    unsigned int i;
    for (i=0; i<AMDGPU_STATE_COUNT; ++i)
        if (Same(path,StateCatalog[i].path) && Same(name,StateCatalog[i].name)) return &StateCatalog[i];
    return 0;
}
static unsigned int Dword(const unsigned char *p)
{ return p[0] | ((unsigned int)p[1]<<8) | ((unsigned int)p[2]<<16) | ((unsigned int)p[3]<<24); }
static unsigned int Word(const unsigned char *p) { return p[0] | ((unsigned int)p[1]<<8); }

unsigned int AmdgpuStateValidate(const AMDGPU_STATE_SPEC *s, unsigned int type, const void *data, unsigned int bytes)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned int i,v;
    if (!s) return AMDGPU_STATE_UNKNOWN;
    if (!data || !bytes || bytes > s->max_bytes) return AMDGPU_STATE_RANGE;
    if (type != s->type) return AMDGPU_STATE_TYPE;
    if (type == AMDGPU_STATE_DWORD) {
        if (bytes != 4) return AMDGPU_STATE_TYPE;
        v = Dword(p);
        if (v < s->min || v > s->max) return AMDGPU_STATE_RANGE;
        if (s->allowed_count) {
            for (i=0; i<s->allowed_count; ++i) if (v == s->allowed[i]) return AMDGPU_STATE_OK;
            return AMDGPU_STATE_RANGE;
        }
    } else if (type == AMDGPU_STATE_QWORD) {
        if (bytes != 8) return AMDGPU_STATE_TYPE;
    } else if (type == AMDGPU_STATE_SZ || type == AMDGPU_STATE_MULTI_SZ) {
        if ((bytes & 1) || bytes < (type == AMDGPU_STATE_SZ ? 2u : 4u) || Word(p+bytes-2))
            return AMDGPU_STATE_TYPE;
        if (type == AMDGPU_STATE_MULTI_SZ && Word(p+bytes-4)) return AMDGPU_STATE_TYPE;
        // A single string cannot conceal a suffix behind an embedded NUL.
        for (i=0; i+2<bytes; i+=2) {
            v=Word(p+i);
            if (type == AMDGPU_STATE_SZ && !v) return AMDGPU_STATE_TYPE;
            if (v >= 0xd800 && v <= 0xdbff) {
                if (i+4>=bytes || Word(p+i+2)<0xdc00 || Word(p+i+2)>0xdfff) return AMDGPU_STATE_TYPE;
                i+=2;
            } else if (v>=0xdc00 && v<=0xdfff) return AMDGPU_STATE_TYPE;
            // MULTI_SZ has exactly one terminator, not a hidden tail after it.
            if (type == AMDGPU_STATE_MULTI_SZ && !v && !Word(p+i+2) && i+4 != bytes)
                return AMDGPU_STATE_TYPE;
        }
    } else if (type != AMDGPU_STATE_BINARY) return AMDGPU_STATE_TYPE;
    return AMDGPU_STATE_OK;
}

static unsigned int Fetch(unsigned int source, const AMDGPU_STATE_SPEC *s, AMDGPU_STATE_FETCH fetch,
    void *context, AMDGPU_STATE_VALUE *v, void *data, unsigned int capacity)
{
    unsigned int result;
    v->type=v->bytes=v->os_error=0; v->source=source;
    result=fetch(context,source,s,v,data,capacity);
    if (result == AMDGPU_STATE_OK) {
        if (v->bytes > capacity) result=AMDGPU_STATE_BUFFER;
        else result=AmdgpuStateValidate(s,v->type,data,v->bytes);
        if (result==AMDGPU_STATE_OK && source==AMDGPU_STATE_FACTORY &&
            Dword((const unsigned char*)data)!=s->factory) result=AMDGPU_STATE_SCHEMA;
    }
    return result;
}
unsigned int AmdgpuStateQuery(const AMDGPU_STATE_SPEC *s, unsigned int mode,
    AMDGPU_STATE_FETCH fetch, void *context, AMDGPU_STATE_VALUE *v, void *data, unsigned int capacity)
{
    unsigned int result;
    if (!v) return AMDGPU_STATE_INVALID_ARGUMENT;
    v->status=AMDGPU_STATE_INVALID_ARGUMENT; v->os_error=v->type=v->bytes=v->source=0;
    if (!s) { v->status=AMDGPU_STATE_UNKNOWN; return v->status; }
    if (!data || !capacity || !fetch || mode>AMDGPU_STATE_READ_RECORD) return v->status;
    if (mode == AMDGPU_STATE_READ_RECORD && s->kind == AMDGPU_STATE_SETTING) return v->status;
    if (mode != AMDGPU_STATE_READ_RECORD && s->kind != AMDGPU_STATE_SETTING) return v->status;
    result=Fetch(AMDGPU_STATE_OVERRIDE,s,fetch,context,v,data,capacity);
    if (result==AMDGPU_STATE_ABSENT && mode==AMDGPU_STATE_QUERY_EFFECTIVE) {
        if (s->factory_present) {
            result=Fetch(AMDGPU_STATE_FACTORY,s,fetch,context,v,data,capacity);
            // An omitted declared package default means a damaged/mixed package.
            if (result==AMDGPU_STATE_ABSENT) result=AMDGPU_STATE_INCOMPLETE;
        } else if (s->code_present) {
            if (capacity<4) result=AMDGPU_STATE_BUFFER;
            else {
                unsigned char *p=(unsigned char*)data;
                p[0]=(unsigned char)s->code; p[1]=(unsigned char)(s->code>>8);
                p[2]=(unsigned char)(s->code>>16); p[3]=(unsigned char)(s->code>>24);
                v->source=AMDGPU_STATE_CODE; v->type=AMDGPU_STATE_DWORD; v->bytes=4;
                result=AmdgpuStateValidate(s,v->type,data,4);
            }
        }
    }
    v->status=result;
    if (result!=AMDGPU_STATE_OK) { v->bytes=0; v->source=AMDGPU_STATE_NONE; }
    return result;
}
unsigned int AmdgpuStateMayWrite(const AMDGPU_STATE_SPEC *s, unsigned int privileged,
    unsigned int driver_writer, unsigned int type, const void *data, unsigned int bytes)
{
    if (!s) return AMDGPU_STATE_UNKNOWN;
    if (!privileged || (s->access!=AMDGPU_STATE_ADMIN && !driver_writer)) return AMDGPU_STATE_ACCESS;
    return AmdgpuStateValidate(s,type,data,bytes);
}

unsigned int AmdgpuStatePackageBytes(void) { return 80u+12u*AMDGPU_STATE_COUNT; }
unsigned int AmdgpuStatePackageCheck(const void *data, unsigned int bytes)
{
    static const unsigned char magic[8]={'A','M','D','S','T','0','2',0};
    const unsigned char *p=(const unsigned char*)data;
    unsigned int i;
    if (!p || bytes!=AmdgpuStatePackageBytes()) return AMDGPU_STATE_SCHEMA;
    for(i=0;i<8;i++) if(p[i]!=magic[i]) return AMDGPU_STATE_SCHEMA;
    if(Dword(p+8)!=AmdgpuStateSchema() || Dword(p+12)!=AMDGPU_STATE_COUNT) return AMDGPU_STATE_SCHEMA;
    for(i=0;i<64;i++) if(p[16+i]!=(unsigned char)AMDGPU_STATE_CATALOG_ID[i]) return AMDGPU_STATE_SCHEMA;
    for(i=0;i<AMDGPU_STATE_COUNT;i++) {
        const AMDGPU_STATE_SPEC *s=&StateCatalog[i];
        if(Dword(p+80+12*i)!=i || Dword(p+84+12*i)!=s->factory_present ||
           Dword(p+88+12*i)!=s->factory) return AMDGPU_STATE_SCHEMA;
    }
    return AMDGPU_STATE_OK;
}
unsigned int AmdgpuStatePackageRead(const void *package, unsigned int bytes,
    const AMDGPU_STATE_SPEC *s, AMDGPU_STATE_VALUE *v, void *data, unsigned int capacity)
{
    unsigned int i,j,result;
    const unsigned char *p=(const unsigned char*)package;
    if(!v) return AMDGPU_STATE_INVALID_ARGUMENT;
    v->type=v->bytes=v->source=v->os_error=0;
    v->status=AMDGPU_STATE_INVALID_ARGUMENT;
    if(!s || !data) return v->status;
    result=AmdgpuStatePackageCheck(package,bytes);
    if(result!=AMDGPU_STATE_OK) { v->status=result; return result; }
    for(i=0;i<AMDGPU_STATE_COUNT;i++) if(s==&StateCatalog[i]) break;
    if(i==AMDGPU_STATE_COUNT) return v->status;
    if(!s->factory_present) result=AMDGPU_STATE_ABSENT;
    else if(capacity<4) result=AMDGPU_STATE_BUFFER;
    else {
        for(j=0;j<4;j++) ((unsigned char*)data)[j]=p[88+12*i+j];
        v->type=AMDGPU_STATE_DWORD; v->bytes=4; v->source=AMDGPU_STATE_FACTORY;
    }
    v->status=result;
    return result;
}
