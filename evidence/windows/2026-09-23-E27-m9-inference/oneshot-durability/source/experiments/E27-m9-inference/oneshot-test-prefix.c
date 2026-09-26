#include <stdio.h>
#include <wchar.h>
typedef int BOOLEAN;
typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef const wchar_t* PCWSTR;
typedef void* HANDLE;
#define PASSIVE_LEVEL 0
#define NT_SUCCESS(s) ((s)>=0)
#define GuardLog(...) ((void)0)
static BOOLEAN g_FullWddm;
static int irql,failOpen,failRead,failWrite,failFlush,opens,closes,writes,flushes,checks,failures;
static ULONG persisted,cached;
static void check(int ok,const char*n){checks++;if(!ok){failures++;printf("FAIL %s\n",n);}}
static int KeGetCurrentIrql(void){return irql;}
static NTSTATUS OpenParameters(HANDLE*k){opens++;*k=(HANDLE)1;return failOpen?-1:0;}
static NTSTATUS ReadDword(HANDLE k,PCWSTR n,ULONG*v){(void)k;(void)n;*v=cached;return failRead?-2:0;}
static NTSTATUS WriteDword(HANDLE k,PCWSTR n,ULONG v){(void)k;(void)n;writes++;if(failWrite)return -3;cached=v;return 0;}
static NTSTATUS ZwFlushKey(HANDLE k){(void)k;flushes++;if(failFlush)return -4;persisted=cached;return 0;}
static void ZwClose(HANDLE k){(void)k;closes++;}
