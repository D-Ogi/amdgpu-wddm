#include <windows.h>
#include <stdio.h>
static unsigned checks,failures,writes,flushes,closes,creates;
static LSTATUS openError,createError,writeError,flushError;
static DWORD cached,durable;
static REGSAM accessMask;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static LSTATUS FakeOpen(HKEY root,LPCWSTR path,DWORD options,REGSAM access,PHKEY key)
{(void)root;(void)path;(void)options;(void)access;*key=(HKEY)1;return openError;}
static LSTATUS FakeCreate(HKEY root,LPCWSTR path,DWORD reserved,LPWSTR cls,DWORD options,REGSAM access,const LPSECURITY_ATTRIBUTES security,PHKEY key,LPDWORD disposition)
{(void)root;(void)path;(void)reserved;(void)cls;(void)options;(void)security;creates++;accessMask=access;*key=(HKEY)2;*disposition=REG_OPENED_EXISTING_KEY;return createError;}
static LSTATUS FakeWrite(HKEY key,LPCWSTR name,DWORD reserved,DWORD type,const BYTE* data,DWORD bytes)
{(void)key;(void)name;(void)reserved;CHECK(type==REG_DWORD&&bytes==sizeof(DWORD));writes++;if(!writeError)cached=*(const DWORD*)data;return writeError;}
static LSTATUS FakeFlush(HKEY key)
{CHECK(key==(HKEY)2);flushes++;if(!(accessMask&KEY_QUERY_VALUE))return ERROR_ACCESS_DENIED;if(!flushError)durable=cached;return flushError;}
static LSTATUS FakeClose(HKEY key){(void)key;closes++;return ERROR_SUCCESS;}
#define RegOpenKeyExW FakeOpen
#define RegCreateKeyExW FakeCreate
#define RegSetValueExW FakeWrite
#define RegFlushKey FakeFlush
#define RegCloseKey FakeClose
#define BC250_SERVICE_KEY L"fixture-service"
#define BC250_PARAMETERS L"fixture-parameters"
#include "confirm_actual.inc"
static void Reset(void){writes=flushes=closes=creates=0;openError=createError=writeError=flushError=0;cached=durable=2;accessMask=0;}
int main(void)
{
    Reset();CHECK(Confirm()==0&&cached==0&&durable==0&&writes==1&&flushes==1&&closes==2);
    CHECK((accessMask&(KEY_QUERY_VALUE|KEY_SET_VALUE))==(KEY_QUERY_VALUE|KEY_SET_VALUE));
    Reset();writeError=ERROR_ACCESS_DENIED;CHECK(Confirm()==1&&durable==2&&flushes==0&&closes==2);
    Reset();flushError=ERROR_REGISTRY_IO_FAILED;CHECK(Confirm()==1&&cached==0&&durable==2&&flushes==1&&closes==2);
    Reset();openError=ERROR_FILE_NOT_FOUND;CHECK(Confirm()==2&&creates==0&&writes==0);
    Reset();openError=ERROR_ACCESS_DENIED;CHECK(Confirm()==1&&creates==0&&writes==0);
    Reset();createError=ERROR_ACCESS_DENIED;CHECK(Confirm()==1&&writes==0&&flushes==0&&closes==1);
    printf("CLI confirmation: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
