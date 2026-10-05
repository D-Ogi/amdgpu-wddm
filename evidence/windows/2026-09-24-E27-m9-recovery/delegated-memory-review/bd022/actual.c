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

static BOOLEAN IsPostFormatSupported(D3DDDIFORMAT Format)
{
    return Format == D3DDDIFMT_X8R8G8B8 || Format == D3DDDIFMT_A8R8G8B8;
}
NTSTATUS DisplayMapFramebuffer(_Inout_ BC250_DEVICE* Device)
{
    // M3 copies 32-bit pixels and nothing else. Any other firmware format is refused, not converted.
    if (!IsPostFormatSupported(Device->Post.ColorFormat)) return STATUS_GRAPHICS_INVALID_PIXELFORMAT;
    if (Device->Post.Pitch < Device->Post.Width * 4) return STATUS_GRAPHICS_INVALID_STRIDE;

    Device->FramebufferLength = (SIZE_T)Device->Post.Pitch * Device->Post.Height;
    Device->FramebufferCacheProtect = PAGE_WRITECOMBINE;
    Device->Framebuffer = MmMapIoSpaceEx(Device->Post.PhysicAddress, Device->FramebufferLength,
                                         PAGE_READWRITE | PAGE_WRITECOMBINE);
    // Record the successful choice so aliases of these physical pages agree.
    if (Device->Framebuffer == NULL) {
        Device->FramebufferCacheProtect = PAGE_NOCACHE;
        Device->Framebuffer = MmMapIoSpaceEx(Device->Post.PhysicAddress, Device->FramebufferLength,
                                             PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (Device->Framebuffer == NULL)
    {
        Device->FramebufferLength = 0;
        Device->FramebufferCacheProtect = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}
void DisplayUnmapFramebuffer(_Inout_ BC250_DEVICE* Device)
{
    // The bugcheck display writes through this pointer: take it away before the mapping goes.
    PVOID mapping = Device->Framebuffer;
    SIZE_T length = Device->FramebufferLength;

    Device->Framebuffer = NULL;
    Device->FramebufferLength = 0;
    Device->FramebufferCacheProtect = 0;
    if (mapping != NULL) MmUnmapIoSpace(mapping, length);
}
ULONG VramMappingProtection(_In_ const BC250_DEVICE* Device, ULONGLONG Physical, SIZE_T Length, ULONG Access)
{
    ULONGLONG post=(ULONGLONG)Device->Post.PhysicAddress.QuadPart;
    ULONGLONG last,postLast;
    if (Length==0 || (ULONGLONG)(Length-1)>MAXULONGLONG-Physical) return 0;
    if (Device->Framebuffer==NULL || Device->FramebufferLength==0) return Access|PAGE_NOCACHE;
    if ((ULONGLONG)(Device->FramebufferLength-1)>MAXULONGLONG-post) return 0;
    last=(Physical+Length-1)>>PAGE_SHIFT;
    postLast=(post+Device->FramebufferLength-1)>>PAGE_SHIFT;
    if ((Physical>>PAGE_SHIFT)<=postLast && (post>>PAGE_SHIFT)<=last) {
        // Do not extend this cache choice onto unrelated physical pages.
        // Callers spanning ownership domains must split their mapping.
        if ((Physical>>PAGE_SHIFT)<(post>>PAGE_SHIFT) || last>postLast) return 0;
        if (Device->FramebufferCacheProtect!=PAGE_NOCACHE &&
            Device->FramebufferCacheProtect!=PAGE_WRITECOMBINE) return 0;
        return Access|Device->FramebufferCacheProtect;
    }
    return Access|PAGE_NOCACHE;
}
PVOID VramMapCpuRange(_In_ const BC250_DEVICE* Device, PHYSICAL_ADDRESS Physical,
                      SIZE_T Length, ULONG Access)
{
    ULONG protection;
    if (Access!=PAGE_READONLY && Access!=PAGE_READWRITE) return NULL;
    protection=VramMappingProtection(Device,(ULONGLONG)Physical.QuadPart,Length,Access);
    if (protection==0) return NULL;
    return MmMapIoSpaceEx(Physical,Length,protection);
}

int main(void){BC250_DEVICE d={0};unsigned i;const ULONGLONG base=0xC0000000ull;
 d.Post.ColorFormat=1;d.Post.Width=16;d.Post.Pitch=64;d.Post.Height=65;d.Post.PhysicAddress.QuadPart=(long long)(base+128);
 CHECK(DisplayMapFramebuffer(&d)==STATUS_SUCCESS && calls==1 && d.FramebufferCacheProtect==PAGE_WRITECOMBINE);
 CHECK(protects[0]==(PAGE_READWRITE|PAGE_WRITECOMBINE));
 {PHYSICAL_ADDRESS address;unsigned before;address.QuadPart=(long long)base;
  CHECK(VramMapCpuRange(&d,address,4096,PAGE_READONLY)!=NULL && protects[(calls-1)%2]==(PAGE_READONLY|PAGE_WRITECOMBINE));
  before=calls;CHECK(VramMapCpuRange(&d,address,0,PAGE_READONLY)==NULL && calls==before);
  CHECK(VramMapCpuRange(&d,address,4096,PAGE_READWRITE|PAGE_NOCACHE)==NULL && calls==before);
  address.QuadPart=(long long)(base-1);CHECK(VramMapCpuRange(&d,address,2,PAGE_READONLY)==NULL && calls==before);
  address.QuadPart=0x200000000ll;CHECK(VramMapCpuRange(&d,address,4096,PAGE_READWRITE)!=NULL && protects[(calls-1)%2]==(PAGE_READWRITE|PAGE_NOCACHE));
 }

 for(i=0;i<8192;i+=4)CHECK(VramMappingProtection(&d,base+i,4,PAGE_READONLY)==(PAGE_READONLY|PAGE_WRITECOMBINE));
 CHECK(VramMappingProtection(&d,base-4096,4096,PAGE_READWRITE)==(PAGE_READWRITE|PAGE_NOCACHE));
 CHECK(VramMappingProtection(&d,base+8192,4096,PAGE_READWRITE)==(PAGE_READWRITE|PAGE_NOCACHE));
 CHECK(VramMappingProtection(&d,base-1,2,PAGE_READONLY)==0);
 CHECK(VramMappingProtection(&d,base+8191,2,PAGE_READWRITE)==0);
 CHECK(VramMappingProtection(&d,0x200000000ull,4096,PAGE_READONLY)==(PAGE_READONLY|PAGE_NOCACHE));
 CHECK(VramMappingProtection(&d,base,0,PAGE_READONLY)==0);
 CHECK(VramMappingProtection(&d,MAXULONGLONG,2,PAGE_READONLY)==0);
 DisplayUnmapFramebuffer(&d);CHECK(!d.Framebuffer && !d.FramebufferLength && !d.FramebufferCacheProtect && unmaps==1);
 CHECK(VramMappingProtection(&d,base,4096,PAGE_READWRITE)==(PAGE_READWRITE|PAGE_NOCACHE));
 calls=0;failWc=1;CHECK(DisplayMapFramebuffer(&d)==STATUS_SUCCESS && calls==2 && d.FramebufferCacheProtect==PAGE_NOCACHE);
 CHECK(protects[1]==(PAGE_READWRITE|PAGE_NOCACHE));
 {PHYSICAL_ADDRESS address;address.QuadPart=(long long)base;
  CHECK(VramMapCpuRange(&d,address,4096,PAGE_READONLY)!=NULL && protects[(calls-1)%2]==(PAGE_READONLY|PAGE_NOCACHE));
 }

 CHECK(VramMappingProtection(&d,base,8192,PAGE_READONLY)==(PAGE_READONLY|PAGE_NOCACHE));
 DisplayUnmapFramebuffer(&d);calls=0;failNc=1;
 CHECK(DisplayMapFramebuffer(&d)==STATUS_INSUFFICIENT_RESOURCES && !d.Framebuffer && !d.FramebufferLength && !d.FramebufferCacheProtect);
 DisplayUnmapFramebuffer(&d);CHECK(unmaps==2);
 printf("POST cache: %u checks, %u failures\n",checks,failures);return failures?1:0;}
