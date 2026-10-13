// Executes the production stored-choice reader, with only the OS query replaced.
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "bc250_fan.h"
typedef long NTSTATUS;
typedef unsigned long ULONG;
#define STATUS_SUCCESS 0L
#define STATUS_OBJECT_NAME_NOT_FOUND ((long)0xC0000034)
#define STATUS_ACCESS_DENIED ((long)0xC0000022)
#define STATUS_DATA_ERROR ((long)0xC000003E)
#define NT_SUCCESS(s) ((s)>=0)
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define FAN_SETTING_MODE L"FanMode"
#define FAN_SETTING_PROFILE L"FanProfile"
#define FAN_SETTING_POINTS L"FanCurvePoints"
#define FAN_SETTING_BOOST L"FanLoadBoost"
#define AMDGPU_STATE_GROUP_FAN 4
static const wchar_t *g_FanCurveSetting[BC250_FAN_POINTS_MAX]={
    L"FanCurve0",L"FanCurve1",L"FanCurve2",L"FanCurve3",L"FanCurve4",L"FanCurve5",L"FanCurve6",L"FanCurve7"};
static NTSTATUS queryStatus[12];
static ULONG data[12];
static unsigned groupEnabled = 1;
static unsigned DriverStateGroupEnabled(unsigned group)
{ return group == AMDGPU_STATE_GROUP_FAN && groupEnabled; }
static NTSTATUS GuardQuerySetting(const wchar_t *name,ULONG *value)
{
    unsigned i=0;
    if(!wcscmp(name,FAN_SETTING_MODE))i=0;
    else if(!wcscmp(name,FAN_SETTING_PROFILE))i=1;
    else if(!wcscmp(name,FAN_SETTING_POINTS))i=2;
    else if(!wcscmp(name,FAN_SETTING_BOOST))i=11;
    else {for(i=0;i<8;i++)if(!wcscmp(name,g_FanCurveSetting[i]))break;i+=3;}
    if(i>=12)return STATUS_DATA_ERROR;
    if(NT_SUCCESS(queryStatus[i]))*value=data[i];
    return queryStatus[i];
}
#include "../fan_choice.h"
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("FAIL line %d: %s\n",__LINE__,#x);}}while(0)
static void Reset(void)
{unsigned i;memset(data,0,sizeof(data));for(i=0;i<12;i++)queryStatus[i]=STATUS_OBJECT_NAME_NOT_FOUND;
 queryStatus[11]=0;data[11]=1;groupEnabled=1;}
int main(void)
{
    struct bc250_fan_request choice;ULONG points,boost;unsigned i;
    Reset();CHECK(FanReadChoice(&choice,&points)==STATUS_OBJECT_NAME_NOT_FOUND);
    queryStatus[0]=STATUS_ACCESS_DENIED;CHECK(FanReadChoice(&choice,&points)==STATUS_ACCESS_DENIED);
    Reset();queryStatus[0]=0;data[0]=BC250_FAN_MODE_CURVE;
    CHECK(FanReadChoice(&choice,&points)==0 && choice.profile==BC250_FAN_PROFILE_STANDARD);
    queryStatus[1]=STATUS_ACCESS_DENIED;CHECK(!NT_SUCCESS(FanReadChoice(&choice,&points)));
    queryStatus[1]=0;data[1]=BC250_FAN_PROFILE_CUSTOM;
    CHECK(FanReadChoice(&choice,&points)==STATUS_DATA_ERROR);
    queryStatus[2]=0;data[2]=2;
    CHECK(FanReadChoice(&choice,&points)==STATUS_DATA_ERROR);
    queryStatus[3]=queryStatus[4]=0;data[3]=(40u<<8)|30u;data[4]=(85u<<8)|100u;
    CHECK(FanReadChoice(&choice,&points)==0 && points==2);
    for(i=0;i<5;i++){
        queryStatus[i]=STATUS_ACCESS_DENIED;CHECK(!NT_SUCCESS(FanReadChoice(&choice,&points)));queryStatus[i]=0;
    }
    data[4]=(39u<<8)|100u;CHECK(FanReadChoice(&choice,&points)==STATUS_DATA_ERROR);
    data[4]=(85u<<8)|100u|0x10000u;CHECK(FanReadChoice(&choice,&points)==STATUS_DATA_ERROR);
    data[4]=(85u<<8)|100u;data[3]=(40u<<8)|0u;CHECK(FanReadChoice(&choice,&points)==STATUS_DATA_ERROR);
    data[2]=9;CHECK(FanReadChoice(&choice,&points)==STATUS_DATA_ERROR);
    data[0]=BC250_FAN_MODE_FIXED;CHECK(FanReadChoice(&choice,&points)==STATUS_DATA_ERROR);
    data[0]=BC250_FAN_MODE_COUNT;CHECK(FanReadChoice(&choice,&points)==STATUS_DATA_ERROR);
    data[0]=BC250_FAN_MODE_BOARD;CHECK(FanReadChoice(&choice,&points)==0);
    Reset();CHECK(FanReadConfiguredChoice(&choice,&points,&boost)==STATUS_OBJECT_NAME_NOT_FOUND && boost==1);
    queryStatus[0]=0;data[0]=BC250_FAN_MODE_CURVE;
    CHECK(FanReadConfiguredChoice(&choice,&points,&boost)==0 && boost==1);
    queryStatus[11]=STATUS_ACCESS_DENIED;
    CHECK(FanReadConfiguredChoice(&choice,&points,&boost)==STATUS_ACCESS_DENIED && boost==0);
    queryStatus[11]=STATUS_OBJECT_NAME_NOT_FOUND;
    CHECK(FanReadConfiguredChoice(&choice,&points,&boost)==STATUS_DATA_ERROR);
    queryStatus[11]=0;data[11]=0;
    CHECK(FanReadConfiguredChoice(&choice,&points,&boost)==0 && boost==0);
    groupEnabled=0;CHECK(FanReadConfiguredChoice(&choice,&points,&boost)==STATUS_DATA_ERROR);
    printf("fan stored choice: %u checks, %u failures\n",checks,failed);
    return failed?1:0;
}
