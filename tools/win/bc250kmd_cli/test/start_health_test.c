#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "bc250kmd_escape.h"
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((LONG)(s)>=0)
#endif
#define BC250_CONTROL_API
#define BC250_DEFAULT_HWID L"fixture"
static int mode,accessFlag,calls,failures,checks;
static int SendEscapeFlags(const WCHAR* id,void* p,unsigned size,LONG* status,int access)
{
    BC250_ESCAPE_START_HEALTH* d=p;
    (void)id;(void)size;
    calls++;accessFlag=access;*status=0;
    if(mode==1){*status=(LONG)0xC000000E;return 1;}
    if(mode==2){*status=(LONG)0xC000009E;return 0;}
    if(mode==3)return 0; /* untouched UNKNOWN_COMMAND */
    d->Status=BC250_ESCAPE_STATUS_DONE;
    d->Generation=d->ExpectedGeneration;d->Epoch=d->ExpectedEpoch;
    d->Flags=BC250_START_HEALTH_REQUIRED|BC250_START_HEALTH_CONFIRMED;
    if(mode==4)d->AbiVersion++;
    if(mode==5)d->Generation++;
    if(mode==6)d->Epoch++;
    if(mode==7)d->Flags=BC250_START_HEALTH_REQUIRED;
    if(mode==8){d->Status=BC250_ESCAPE_STATUS_REFUSED;d->NtStatus=0xC0000185;}
    if(mode==9){d->Flags=0;d->Completed=0;}
    if(mode==10)d->NtStatus=0xC0000185;
    if(mode==11)d->Flags=BC250_START_HEALTH_CONFIRMED;
    return 0;
}
// ACTUAL_START_HEALTH
static void Check(int ok,int line,const char* text){checks++;if(!ok){failures++;printf("FAIL line%d: %s\n",line,text);}}
#define CHECK(x) Check((x),__LINE__,#x)
int main(void)
{
    BC250_ESCAPE_START_HEALTH d;
    CHECK(sizeof(d)==96);
    CHECK(Bc250StartHealth(0,0,0,NULL,sizeof(d))<0 && calls==0);
    CHECK(Bc250StartHealth(0,0,0,&d,95)<0 && calls==0);
    CHECK(Bc250StartHealth(2,0,0,&d,sizeof(d))<0 && calls==0);
    CHECK(Bc250StartHealth(0,0,0,&d,sizeof(d))==0 && accessFlag==0);
    CHECK(Bc250StartHealth(1,123,7,&d,sizeof(d))==0 && accessFlag==1 && d.Generation==123 && d.Epoch==7);
    for(mode=1;mode<=8;mode++)CHECK(Bc250StartHealth(1,123,7,&d,sizeof(d))<0);
    mode=9;CHECK(Bc250StartHealth(0,0,0,&d,sizeof(d))==0 && d.Flags==0);
    for(mode=10;mode<=11;mode++)CHECK(Bc250StartHealth(1,123,7,&d,sizeof(d))<0);
    printf("Start health client: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
