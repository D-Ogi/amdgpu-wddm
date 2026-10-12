/* Synthetic firmware and optional caller-supplied private ROM; no firmware bytes are printed. */
#define _CRT_SECURE_NO_WARNINGS
#include "../../shim/bc250_atom_image.h"
#include "../../shim/bc250_atom_preflight.h"
#include "../../shim/bc250_atom.h"
#include "../../shim/bc250_atom_firmware.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned Checks, Failures;
#define CHECK(e) do { ++Checks; if (!(e)) { ++Failures; printf("line %u: %s\n", __LINE__, #e); } } while (0)
static void W16(unsigned char *b, unsigned p, unsigned v)
{ b[p]=(unsigned char)v; b[p+1]=(unsigned char)(v>>8); }
static void W32(unsigned char *b, unsigned p, unsigned v)
{ W16(b,p,v); W16(b,p+2,v>>16); }
static void Sum(unsigned char *b, unsigned n, unsigned at)
{
    unsigned sum=0, i;
    b[at]=0;
    for (i=0;i<n;++i) sum+=b[i];
    b[at]=(unsigned char)(0u-sum);
}
static void Rom(unsigned char *b)
{
    memset(b,0,512);
    W16(b,0,0xaa55); b[2]=1;
    W16(b,0x18,0x120); memcpy(b+0x120,"PCIR",4);
    W16(b,0x124,0x1002); W16(b,0x126,0x13fe); W16(b,0x12a,24); W16(b,0x130,1);
    W16(b,0x48,0x60); W16(b,0x60,0x24); memcpy(b+0x64,"ATOM",4);
    W16(b,0x7e,0x90); W16(b,0x80,0xa4);
    W16(b,0x90,20); b[0x92]=2; b[0x93]=1;
    W16(b,0xa4,12); b[0xa6]=2; b[0xa7]=1;
    W16(b,0x94+4*2,0xb0); W16(b,0xb0,7); b[0xb2]=1; b[0xb3]=5; b[0xb6]=91;
    W16(b,0xa8+2*2,0xc0); W16(b,0xc0,8); b[0xc2]=1; b[0xc3]=1;
    Sum(b,512,511);
}
static unsigned Vfct(unsigned char *b, const unsigned char *rom, unsigned copies)
{
    unsigned i,n=76+copies*(28+512);
    memset(b,0,n); memcpy(b,"VFCT",4); W32(b,4,n); b[8]=1; W32(b,52,76);
    for (i=0;i<copies;++i) {
        unsigned p=76+i*(28+512);
        W32(b,p,5); W32(b,p+4,0); W32(b,p+8,0);
        W16(b,p+12,0x1002); W16(b,p+14,0x13fe); W32(b,p+24,512);
        memcpy(b+p+28,rom,512);
    }
    Sum(b,n,9); return n;
}

typedef struct FIRMWARE_IO {
    unsigned char vfct[1200];
    uint32_t length;
    unsigned calls,failCall,allocs,failAlloc,live,identities,changed,duplicates,none;
    unsigned enumShort,fetchShort,enumHuge,fetchHuge,corrupt;
} FIRMWARE_IO;
static int FirmwareCall(FIRMWARE_IO *f) { return ++f->calls == f->failCall; }
static int FirmwareIdentity(void *p,BC250_ATOM_IDENTITY *id)
{
    FIRMWARE_IO *f=p;
    if (FirmwareCall(f)) return -1;
    id->Bus=5; id->Device=0; id->Function=0; id->VendorId=0x1002; id->DeviceId=0x13fe;
    if (++f->identities==2 && f->changed) ++id->Bus;
    return 0;
}
static int FirmwareEnumerate(void *p,void *buffer,uint32_t capacity,uint32_t *returned)
{
    FIRMWARE_IO *f=p;
    if (FirmwareCall(f)) return -1;
    *returned=f->enumHuge?BC250_ATOM_MAX_TABLE_IDS+4u:12u;
    if (buffer) {
        CHECK(capacity==12);
        W32(buffer,0,0); W32(buffer,4,f->none?0:BC250_ATOM_VFCT_ID);
        W32(buffer,8,f->duplicates?BC250_ATOM_VFCT_ID:0);
        if (f->enumShort) --*returned;
    }
    return 0;
}
static int FirmwareFetch(void *p,void *buffer,uint32_t capacity,uint32_t *returned)
{
    FIRMWARE_IO *f=p;
    if (FirmwareCall(f)) return -1;
    *returned=f->fetchHuge?BC250_ATOM_MAX_VFCT+1u:f->length;
    if (buffer) {
        CHECK(capacity==f->length);
        memcpy(buffer,f->vfct,f->length);
        if (f->fetchShort) --*returned;
        if (f->corrupt) ((unsigned char*)buffer)[9]^=1;
    }
    return 0;
}
static void *FirmwareAllocate(void *p,size_t bytes)
{
    FIRMWARE_IO *f=p;
    void *b;
    if (++f->allocs==f->failAlloc) return NULL;
    b=malloc(bytes);
    if (b) ++f->live;
    return b;
}
static void FirmwareFree(void *p,void *b)
{ FIRMWARE_IO *f=p; CHECK(b && f->live); --f->live; free(b); }
static void FirmwareTests(void)
{
    unsigned char rom[512];
    FIRMWARE_IO f={0};
    BC250_ATOM_FIRMWARE_IO io={&f,FirmwareIdentity,FirmwareEnumerate,FirmwareFetch,FirmwareAllocate,FirmwareFree};
    BC250_ATOM_FIRMWARE owned={0};
    unsigned i,calls;
    Rom(rom); f.length=Vfct(f.vfct,rom,1);
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_OK);
    CHECK(f.live==1 && f.identities==2 && f.calls==6 && f.allocs==3);
    CHECK(owned.Storage==owned.Image.Bytes && owned.Image.Length==512 && owned.Identity.Bus==5);
    CHECK(!memcmp(owned.Image.Bytes,rom,512));
    /* Capturing into a live object must not erase or leak its old ownership. */
    calls=f.calls;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_ARGUMENT && f.calls==calls && f.live==1);
    Bc250AtomFirmwareRelease(&io,&owned);
    CHECK(!f.live && !owned.Storage && !owned.Image.Bytes);
    Bc250AtomFirmwareRelease(&io,&owned);
    CHECK(!f.live);
    for (i=1;i<=6;++i) {
        memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.failCall=i;
        CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_IO);
        CHECK(f.calls==i && !f.live && !owned.Storage && !owned.Image.Bytes);
    }
    for (i=1;i<=3;++i) {
        memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.failAlloc=i;
        CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_MEMORY);
        CHECK(!f.live && !owned.Storage && !owned.Image.Bytes);
    }
    memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.changed=1;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_IDENTITY_MISMATCH && !f.live);
    memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.duplicates=1;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_AMBIGUOUS && !f.live && f.calls==3);
    memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.none=1;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_NOT_FOUND && !f.live && f.calls==3);
    memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.enumShort=1;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_BOUNDS && !f.live);
    memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.fetchShort=1;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_BOUNDS && !f.live);
    memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.enumHuge=1;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_BOUNDS && !f.live && !f.allocs);
    memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.fetchHuge=1;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_BOUNDS && !f.live && f.allocs==1);
    memset(&f,0,sizeof(f)); f.length=Vfct(f.vfct,rom,1); f.corrupt=1;
    CHECK(Bc250AtomFirmwareCapture(&io,&owned)==BC250_ATOM_CHECKSUM && !f.live);
}

typedef struct FAKE_IO {
    unsigned value,reads,writes,failWrite,owned,delays;
    uint64_t now;
} FAKE_IO;
static int Owned(void *p) { return ((FAKE_IO*)p)->owned!=0; }
static int Allowed(void *p,uint32_t reg,int write) { (void)p; (void)write; return reg==32; }
static int Read(void *p,uint32_t reg,uint32_t *value)
{ FAKE_IO *f=p; CHECK(reg==32); ++f->reads; *value=f->value; return 0; }
static int Write(void *p,uint32_t reg,uint32_t value)
{ FAKE_IO *f=p; CHECK(reg==32); ++f->writes; f->value=value; return f->failWrite?-1:0; }
static uint64_t Now(void *p) { return ((FAKE_IO*)p)->now++; }
static int Delay(void *p,uint32_t us)
{ FAKE_IO *f=p; f->now+=us; ++f->delays; return 0; }
static void ExecTests(BC250_ATOM_PLAN *plan)
{
    unsigned char b[512];
    BC250_ATOM_IMAGE image;
    BC250_ATOM_RESULT result;
    FAKE_IO fake={0};
    BC250_ATOM_CALLBACKS io={&fake,Owned,Allowed,Read,Write,Now,Delay};
    BC250_ATOM_LIMITS limits={50,1000,10000};
    uint32_t params[4]={0};
    void *ctx=calloc(1,Bc250AtomContextSize());
    unsigned p=0xb6;
    CHECK(ctx!=NULL); if (!ctx) return;
    Rom(b);
    /* MOV MM_INDEX,32; MOV MM_DATA,pattern; MOV PS[0],REG[32]; EOT. */
    b[p++]=1; b[p++]=5; W16(b,p,0); p+=2; W32(b,p,32); p+=4;
    b[p++]=1; b[p++]=5; W16(b,p,1); p+=2; W32(b,p,0xa5123499); p+=4;
    b[p++]=2; b[p++]=0; b[p++]=0; W16(b,p,32); p+=2; b[p++]=91;
    W16(b,0xb0,p-0xb0); Sum(b,512,511);
    CHECK(Bc250AtomImageOpen(b,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OK);
    fake.owned=1;
    CHECK(Bc250AtomExecute(ctx,Bc250AtomContextSize(),&image,4,plan,&io,&limits,params,sizeof(params),&result)==BC250_ATOM_OK);
    CHECK(params[0]==0xa5123499 && fake.writes==1 && fake.reads==1 && result.Instructions==4);
    {
        BC250_ATOM_COMMAND commands[2]={{4,params,sizeof(params)},{4,params,sizeof(params)}};
        BC250_ATOM_LIMITS batchLimits=limits;
        fake.writes=fake.reads=0;
        CHECK(Bc250AtomExecuteBatch(ctx,Bc250AtomContextSize(),&image,plan,&io,&batchLimits,commands,2,&result)==BC250_ATOM_OK);
        CHECK(result.Instructions==8 && result.Writes==2 && result.Reads==2 && fake.writes==2);
        /* A bad second command must be caught before the first command writes. */
        commands[1].Root=8; fake.writes=fake.reads=0;
        CHECK(Bc250AtomExecuteBatch(ctx,Bc250AtomContextSize(),&image,plan,&io,&batchLimits,commands,2,&result)==BC250_ATOM_ARGUMENT);
        CHECK(!result.Writes && !fake.writes && !fake.reads);
        commands[1].Root=4; commands[1].ParameterBytes=3;
        CHECK(Bc250AtomExecuteBatch(ctx,Bc250AtomContextSize(),&image,plan,&io,&batchLimits,commands,2,&result)==BC250_ATOM_ARGUMENT);
        CHECK(!result.Writes && !fake.writes);
        commands[1].ParameterBytes=sizeof(params); batchLimits.Instructions=6;
        CHECK(Bc250AtomExecuteBatch(ctx,Bc250AtomContextSize(),&image,plan,&io,&batchLimits,commands,2,&result)==BC250_ATOM_BUDGET);
        CHECK(result.Instructions==6 && result.Writes==2 && fake.writes==2);
        /* A recursive second entry is also found before the first entry runs. */
        W16(b,0x94+5*2,0xe0); W16(b,0xe0,9); b[0xe2]=1; b[0xe3]=1;
        b[0xe6]=82; b[0xe7]=5; b[0xe8]=91; Sum(b,512,511);
        CHECK(Bc250AtomImageOpen(b,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
        commands[1].Root=5; batchLimits=limits; fake.writes=fake.reads=0;
        CHECK(Bc250AtomExecuteBatch(ctx,Bc250AtomContextSize(),&image,plan,&io,&batchLimits,commands,2,&result)==BC250_ATOM_RECURSION);
        CHECK(!result.Writes && !fake.writes && !fake.reads);
        W16(b,0x94+5*2,0); Sum(b,512,511);
        CHECK(Bc250AtomImageOpen(b,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
        CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OK);
    }
    fake.owned=0; fake.writes=fake.reads=0;
    CHECK(Bc250AtomExecute(ctx,Bc250AtomContextSize(),&image,4,plan,&io,&limits,params,sizeof(params),&result)==BC250_ATOM_CANCELLED);
    CHECK(!fake.writes && !fake.reads);
    fake.owned=1; fake.failWrite=1;
    CHECK(Bc250AtomExecute(ctx,Bc250AtomContextSize(),&image,4,plan,&io,&limits,params,sizeof(params),&result)==BC250_ATOM_IO);
    CHECK(fake.writes==1 && !fake.reads && result.Writes==1);
    fake.failWrite=0; fake.writes=0;
    /* A different image cannot borrow the plan's acceptance. */
    { BC250_ATOM_IMAGE other=image; other.Bytes=b+1;
      CHECK(Bc250AtomExecute(ctx,Bc250AtomContextSize(),&other,4,plan,&io,&limits,params,sizeof(params),&result)==BC250_ATOM_ARGUMENT); }
    CHECK(!fake.writes);
    Rom(b); W16(b,0xb0,9); b[0xb6]=67; W16(b,0xb7,6); Sum(b,512,511);
    CHECK(Bc250AtomImageOpen(b,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OK);
    CHECK(Bc250AtomExecute(ctx,Bc250AtomContextSize(),&image,4,plan,&io,&limits,params,sizeof(params),&result)==BC250_ATOM_BUDGET);
    CHECK(result.Instructions==limits.Instructions && !fake.writes);
    Rom(b); W16(b,0xb0,9); b[0xb6]=80; b[0xb7]=255; b[0xb8]=91; Sum(b,512,511);
    CHECK(Bc250AtomImageOpen(b,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OK);
    CHECK(Bc250AtomExecute(ctx,Bc250AtomContextSize(),&image,4,plan,&io,&limits,params,sizeof(params),&result)==BC250_ATOM_BUDGET);
    CHECK(!fake.delays);
    {
        BC250_ATOM_COMMAND commands[2]={{4,params,sizeof(params)},{4,params,sizeof(params)}};
        b[0xb7]=1; Sum(b,512,511);
        CHECK(Bc250AtomImageOpen(b,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
        CHECK(Bc250AtomExecuteBatch(ctx,Bc250AtomContextSize(),&image,plan,&io,&limits,commands,2,&result)==BC250_ATOM_BUDGET);
        CHECK(result.DelayUs==1000 && fake.delays==1);
    }
    /* Dynamic shift of 32 must refuse before a read/modify/write reaches hardware. */
    Rom(b); W16(b,0xb0,12); b[0xb6]=109; b[0xb7]=1; W16(b,0xb8,32); b[0xba]=0; b[0xbb]=91;
    Sum(b,512,511); params[0]=32;
    CHECK(Bc250AtomImageOpen(b,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OK);
    CHECK(Bc250AtomExecute(ctx,Bc250AtomContextSize(),&image,4,plan,&io,&limits,params,sizeof(params),&result)==BC250_ATOM_PARAMETER);
    CHECK(!fake.writes);
    free(ctx);
}

int main(int argc, char **argv)
{
    unsigned char rom[512], bad[512], vfct[1200];
    BC250_ATOM_IMAGE image;
    BC250_ATOM_TABLE table;
    BC250_ATOM_IDENTITY id={5,0,0,0x1002,0x13fe};
    unsigned i,n;
    BC250_ATOM_PLAN *plan=calloc(1,sizeof(*plan));
    CHECK(plan!=NULL);
    if (!plan) return 1;
    FirmwareTests();
    ExecTests(plan);
    Rom(rom);
    CHECK(Bc250AtomImageOpen(rom,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(image.CommandCount==8 && image.DataCount==4);
    CHECK(Bc250AtomGetTable(&image,4,1,&table)==BC250_ATOM_OK);
    CHECK(table.Offset==0xb0 && table.Length==7 && table.Format==1 && table.Content==5);
    CHECK(Bc250AtomGetTable(&image,2,0,&table)==BC250_ATOM_OK && table.Length==8);
    CHECK(Bc250AtomGetTable(&image,8,1,&table)==BC250_ATOM_BOUNDS);
    CHECK(Bc250AtomGetTable(&image,0,1,&table)==BC250_ATOM_NOT_FOUND && !table.Offset);
    CHECK(Bc250AtomGetTable(&image,~0u,1,&table)==BC250_ATOM_BOUNDS);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OK && plan->Instructions==1 && plan->TableCount==1);
    /* Conditional branch into a source operand must fail before execution. */
    memcpy(bad,rom,512); W16(bad,0xb0,15);
    bad[0xb6]=68; W16(bad,0xb7,11); /* JUMP_EQUAL to the immediate in MOV_PS. */
    bad[0xb9]=2; bad[0xba]=0x25; bad[0xbb]=0; bad[0xbc]=1; bad[0xbd]=91; bad[0xbe]=91;
    Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OPCODE);
    /* A short self-loop is structurally valid; the interpreter must bound execution. */
    memcpy(bad,rom,512); W16(bad,0xb0,9); bad[0xb6]=67; W16(bad,0xb7,6); Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OK && plan->Instructions==1);
    /* Recursion is distinct from an absent optional called table. */
    memcpy(bad,rom,512); W16(bad,0xb0,9); bad[0xb6]=82; bad[0xb7]=4; bad[0xb8]=91; Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_RECURSION);
    bad[0xb7]=0; Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_OK);
    /* Both sides of a branch are validated even if input arguments would skip one. */
    bad[0xb6]=56; bad[0xb7]=0; Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_IO);
    bad[0xb6]=1; bad[0xb7]=0; bad[0xb8]=0; Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomPreflight(&image,4,plan)==BC250_ATOM_BOUNDS);
    for (i=0;i<512;++i) {
        CHECK(Bc250AtomImageOpen(rom,i,0x1002,0x13fe,&image)!=BC250_ATOM_OK && !image.Bytes);
    }
    CHECK(Bc250AtomImageOpen(rom,512,0x1002,0xffff,&image)==BC250_ATOM_IDENTITY_MISMATCH);
    memcpy(bad,rom,512); bad[300]^=1;
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_CHECKSUM);
    memcpy(bad,rom,512); W16(bad,0x18,511); Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_BOUNDS);
    memcpy(bad,rom,512); W16(bad,0x48,510); Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_BOUNDS);
    memcpy(bad,rom,512); W16(bad,0x90,0xffff); Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_BOUNDS);
    memcpy(bad,rom,512); bad[0x92]=1; Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_VERSION);
    memcpy(bad,rom,512); W16(bad,0xb0,0xffff); Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomGetTable(&image,4,1,&table)==BC250_ATOM_BOUNDS && !table.Offset);
    memcpy(bad,rom,512); bad[0xb5]=3; Sum(bad,512,511);
    CHECK(Bc250AtomImageOpen(bad,512,0x1002,0x13fe,&image)==BC250_ATOM_OK);
    CHECK(Bc250AtomGetTable(&image,4,1,&table)==BC250_ATOM_PARAMETER && !table.Offset);
    n=Vfct(vfct,rom,1);
    CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_OK);
    CHECK(image.Bytes==vfct+104 && image.Length==512);
    for (i=0;i<n;++i) CHECK(Bc250AtomVfctSelect(vfct,i,&id,&image)!=BC250_ATOM_OK && !image.Bytes);
    /* Each of the five adapter identity fields is required. */
    for (i=0;i<5;++i) {
        unsigned offset=76+(i<3?4*i:12+2*(i-3));
        n=Vfct(vfct,rom,1); vfct[offset]^=1; Sum(vfct,n,9);
        CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_NOT_FOUND);
    }
    n=Vfct(vfct,rom,2);
    CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_AMBIGUOUS && !image.Bytes);
    /* A malformed suffix must not hide behind the first matching image. */
    W32(vfct,76+540+24,~0u); Sum(vfct,n,9);
    CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_BOUNDS && !image.Bytes);
    n=Vfct(vfct,rom,1); W32(vfct,100,~0u); Sum(vfct,n,9);
    CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_BOUNDS);
    n=Vfct(vfct,rom,1); W32(vfct,52,75); Sum(vfct,n,9);
    CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_BOUNDS);
    n=Vfct(vfct,rom,1); W32(vfct,56,77); Sum(vfct,n,9);
    CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_BOUNDS);
    n=Vfct(vfct,rom,1); vfct[9]^=1;
    CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_CHECKSUM);
    n=Vfct(vfct,rom,1); vfct[104+0x126]^=1; Sum(vfct+104,512,511); Sum(vfct,n,9);
    CHECK(Bc250AtomVfctSelect(vfct,n,&id,&image)==BC250_ATOM_IDENTITY_MISMATCH);
    if (argc==2) {
        FILE *f=fopen(argv[1],"rb");
        unsigned char *bytes=malloc(BC250_ATOM_MAX_IMAGE+1u);
        size_t size=0;
        const unsigned roots[]={4,12,35,49,76};
        CHECK(f && bytes);
        if (f && bytes) {
            size=fread(bytes,1,BC250_ATOM_MAX_IMAGE+1u,f);
            CHECK(!ferror(f));
            CHECK(Bc250AtomImageOpen(bytes,size,0x1002,0x13fe,&image)==BC250_ATOM_OK);
            if (image.Bytes) for (i=0;i<sizeof(roots)/sizeof(roots[0]);++i) {
                CHECK(Bc250AtomGetTable(&image,roots[i],1,&table)==BC250_ATOM_OK);
                printf("private ROM command %u revision %u.%u bytes %u\n",roots[i],table.Format,table.Content,table.Length);
                CHECK(Bc250AtomPreflight(&image,roots[i],plan)==BC250_ATOM_OK);
                printf("preflight status %u, tables %u, instructions %u, parameter DWORDs %u, failure table %u offset %u\n",
                    plan->Status,plan->TableCount,plan->Instructions,plan->Tables[roots[i]].TotalParameters,
                    plan->FailedTable,plan->FailedOffset);
            }
        }
        if (f) fclose(f);
        free(bytes);
    }
    printf("ATOM image: %u checks, %u failures\n",Checks,Failures);
    free(plan);
    return Failures?1:0;
}
