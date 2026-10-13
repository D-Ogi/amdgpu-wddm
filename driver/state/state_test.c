#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "state.h"

static unsigned int failures,checks,reads;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL CHECK %u: %s\n",__LINE__,#x);}}while(0)
typedef struct fixture { unsigned int result[2],type[2],bytes[2],number[2]; } FIXTURE;
static unsigned int Fetch(void *context,unsigned int source,const AMDGPU_STATE_SPEC *s,
    AMDGPU_STATE_VALUE *v,void *data,unsigned int capacity)
{
    FIXTURE *f=context;unsigned int i=source==AMDGPU_STATE_FACTORY;
    (void)s;reads++;
    v->os_error=100+i;v->type=f->type[i];v->bytes=f->bytes[i];
    if(f->result[i]!=AMDGPU_STATE_OK)return f->result[i];
    if(capacity<4)return AMDGPU_STATE_BUFFER;
    memcpy(data,&f->number[i],4);return AMDGPU_STATE_OK;
}
static unsigned int Query(const char *name,unsigned int mode,FIXTURE *f,AMDGPU_STATE_VALUE *v,unsigned int *out)
{ reads=0;return AmdgpuStateQuery(AmdgpuStateFind("",name),mode,Fetch,f,v,out,4); }
int main(int argc,char **argv)
{
    FIXTURE f={{AMDGPU_STATE_ABSENT,AMDGPU_STATE_OK},{4,4},{4,4},{0,1}};
    AMDGPU_STATE_VALUE v;
    unsigned int out=0,i,n;
    const AMDGPU_STATE_SPEC *s;
    CHECK(AmdgpuStateSchema()==2 && strlen(AmdgpuStateCatalogId())==64 && AmdgpuStateCount()>=177);
    CHECK(AmdgpuStateAt(AmdgpuStateCount())==0);
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==0 && out==1 && reads==2 && v.source==AMDGPU_STATE_FACTORY);
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_OVERRIDE,&f,&v,&out)==AMDGPU_STATE_ABSENT && reads==1 && !v.bytes);
    f.result[0]=AMDGPU_STATE_OK;f.number[0]=0;
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==0 && out==0 && reads==1 && v.source==AMDGPU_STATE_OVERRIDE);
    // Every error stays an error; none is a missing override that enables tuning.
    for(i=AMDGPU_STATE_UNKNOWN;i<=AMDGPU_STATE_INVALID_ARGUMENT;i++){
        f.result[0]=i;
        CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==i && reads==1 && !v.bytes && v.os_error==100);
    }
    f.result[0]=AMDGPU_STATE_OK;f.type[0]=1;
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==AMDGPU_STATE_TYPE && reads==1);
    f.type[0]=4;f.bytes[0]=3;
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==AMDGPU_STATE_TYPE && reads==1);
    f.bytes[0]=8;
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==AMDGPU_STATE_BUFFER && reads==1);
    f.bytes[0]=4;f.number[0]=2;
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==AMDGPU_STATE_RANGE && reads==1);
    f.result[0]=AMDGPU_STATE_ABSENT;f.number[1]=0;
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==AMDGPU_STATE_SCHEMA);
    f.result[1]=AMDGPU_STATE_ABSENT;
    CHECK(Query("CpuTune",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==AMDGPU_STATE_INCOMPLETE);
    CHECK(Query("EnableRingVmFlush",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==0 && out==1 && reads==1 && v.source==AMDGPU_STATE_CODE);
    CHECK(Query("CpuPending",AMDGPU_STATE_READ_RECORD,&f,&v,&out)==AMDGPU_STATE_ABSENT && reads==1 && !v.bytes);
    CHECK(Query("CpuPending",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==AMDGPU_STATE_INVALID_ARGUMENT && !reads);
    CHECK(Query("CpuTune",AMDGPU_STATE_READ_RECORD,&f,&v,&out)==AMDGPU_STATE_INVALID_ARGUMENT && !reads);
    CHECK(Query("Typo",AMDGPU_STATE_QUERY_EFFECTIVE,&f,&v,&out)==AMDGPU_STATE_UNKNOWN && !reads);
    CHECK(AmdgpuStateFind("..","CpuTune")==0 && AmdgpuStateFind(0,"CpuTune")==0);
    CHECK(AmdgpuStateFind("","cputune")==AmdgpuStateFind("","CpuTune"));
    for(i=0;i<AmdgpuStateCount();i++){
        s=AmdgpuStateAt(i);
        CHECK(AmdgpuStateFind(s->path,s->name)==s);
        CHECK(AmdgpuStateMayWrite(s,0,0,s->type,&out,4)==AMDGPU_STATE_ACCESS);
        if(s->access==AMDGPU_STATE_DRIVER)CHECK(AmdgpuStateMayWrite(s,1,0,s->type,&out,4)==AMDGPU_STATE_ACCESS);
        if(s->kind!=AMDGPU_STATE_SETTING)CHECK(!s->factory_present && !s->code_present);
        if(s->type==AMDGPU_STATE_DWORD){
            out=s->allowed_count?s->allowed[0]:s->min;
            CHECK(AmdgpuStateValidate(s,4,&out,4)==0);
            CHECK(AmdgpuStateValidate(s,4,&out,3)!=0);
            CHECK(AmdgpuStateValidate(s,3,&out,4)==AMDGPU_STATE_TYPE);
            if(s->factory_present){out=s->factory;CHECK(AmdgpuStateValidate(s,4,&out,4)==0);}
            if(s->code_present){out=s->code;CHECK(AmdgpuStateValidate(s,4,&out,4)==0);}
            for(n=0;n<s->allowed_count;n++){out=s->allowed[n];CHECK(AmdgpuStateValidate(s,4,&out,4)==0);}
        }
    }
    {
        unsigned char text[]={65,0,0,0,66,0,0,0};
        unsigned char pair[]={0x00,0xd8,0x00,0xdc,0,0};
        s=AmdgpuStateFind("StartRecord","Step");
        CHECK(AmdgpuStateValidate(s,1,text,4)==0);
        CHECK(AmdgpuStateValidate(s,1,text,3)==AMDGPU_STATE_TYPE);
        CHECK(AmdgpuStateValidate(s,1,text,8)==AMDGPU_STATE_TYPE);
        CHECK(AmdgpuStateValidate(s,1,pair,6)==0);
        CHECK(AmdgpuStateValidate(s,1,pair,4)==AMDGPU_STATE_TYPE);
        pair[3]=0;
        CHECK(AmdgpuStateValidate(s,1,pair,6)==AMDGPU_STATE_TYPE);
        s=AmdgpuStateFind("StartRecord","Log");
        CHECK(AmdgpuStateValidate(s,7,text,8)==AMDGPU_STATE_TYPE);
        text[4]=0;
        CHECK(AmdgpuStateValidate(s,7,text,6)==0);
        text[6]=66;
        CHECK(AmdgpuStateValidate(s,7,text,8)==AMDGPU_STATE_TYPE);
    }
    s=AmdgpuStateFind("HangRecovery\\Waves","Snapshot");CHECK(s && s->private_read);
    s=AmdgpuStateFind("CuModeBoot","Valid");CHECK(s && s->kind==AMDGPU_STATE_VOLATILE);
    s=AmdgpuStateFind("","CpuTune");CHECK(s->legacy_off==1);
    {
        unsigned int bytes=AmdgpuStatePackageBytes();
        unsigned char *package=malloc(bytes+1); FILE *file=NULL;
        CHECK(argc==2 && package!=NULL);
        if(argc==2 && package) (void)fopen_s(&file,argv[1],"rb");
        CHECK(file!=NULL);
        if(file) {
            CHECK(fread(package,1,bytes+1,file)==bytes);fclose(file);
            CHECK(AmdgpuStatePackageCheck(package,bytes)==0);
            CHECK(AmdgpuStatePackageCheck(package,bytes-1)==AMDGPU_STATE_SCHEMA);
            CHECK(AmdgpuStatePackageCheck(package,bytes+1)==AMDGPU_STATE_SCHEMA);
            CHECK(AmdgpuStatePackageRead(package,bytes,s,&v,&out,4)==0 && out==1);
            // Any mutation is a schema mismatch, including spare fields and the
            // defaults of values other than the one the caller asks for.
            for(i=0;i<bytes;i++) {
                package[i]^=1;
                CHECK(AmdgpuStatePackageCheck(package,bytes)==AMDGPU_STATE_SCHEMA);
                CHECK(AmdgpuStatePackageRead(package,bytes,s,&v,&out,4)==AMDGPU_STATE_SCHEMA && v.bytes==0);
                package[i]^=1;
            }
        }
        free(package);
    }
    printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
