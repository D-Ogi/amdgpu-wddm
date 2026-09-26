#include <stdio.h>
#include <string.h>
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef size_t SIZE_T;
typedef void* PVOID;
typedef int BOOLEAN,D3DDDIFORMAT,NTSTATUS;
typedef struct {long long QuadPart;} PHYSICAL_ADDRESS;
typedef struct {struct {D3DDDIFORMAT ColorFormat;ULONG Pitch,Width,Height;PHYSICAL_ADDRESS PhysicAddress;} Post;void*Framebuffer;SIZE_T FramebufferLength;ULONG FramebufferCacheProtect;} BC250_DEVICE;
#define D3DDDIFMT_X8R8G8B8 1
#define D3DDDIFMT_A8R8G8B8 2
#define PAGE_READWRITE 4u
#define PAGE_READONLY 2u
#define PAGE_NOCACHE 512u
#define PAGE_WRITECOMBINE 1024u
#define PAGE_SHIFT 12
#define MAXULONGLONG (~0ull)
#define STATUS_SUCCESS 0
#define STATUS_GRAPHICS_INVALID_PIXELFORMAT (-1)
#define STATUS_GRAPHICS_INVALID_STRIDE (-2)
#define STATUS_INSUFFICIENT_RESOURCES (-3)
static unsigned checks,failures,calls,unmaps,protects[2];static int failWc,failNc;
static char test_mapping[8192];
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);}}while(0)
static void*MmMapIoSpaceEx(PHYSICAL_ADDRESS p,SIZE_T n,ULONG flags){(void)p;(void)n;protects[calls++%2]=flags;if((flags&PAGE_WRITECOMBINE)?failWc:failNc)return NULL;return test_mapping;}
static void MmUnmapIoSpace(void*p,SIZE_T n){CHECK(p==test_mapping && n!=0);unmaps++;}

