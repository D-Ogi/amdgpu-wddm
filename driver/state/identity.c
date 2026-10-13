#ifdef AMDGPU_STATE_KERNEL
#include <ntifs.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <bcrypt.h>
#include "identity.h"
#include "state.h"
unsigned int AmdgpuStateDeviceIdentity(const unsigned short *id,unsigned int characters,unsigned char hash[32])
{
    unsigned char canonical[1024];unsigned int i;BCRYPT_ALG_HANDLE algorithm=NULL;NTSTATUS status;
    if(!id || !hash || !characters || characters>512)return AMDGPU_STATE_INVALID_ARGUMENT;
    for(i=0;i<characters;i++) {
        unsigned short c=id[i];if(c<33 || c>126)return AMDGPU_STATE_INVALID_ARGUMENT;
        if(c>='a' && c<='z')c=(unsigned short)(c-'a'+'A');
        canonical[2*i]=(unsigned char)c;canonical[2*i+1]=0;
    }
    for(i=0;i<32;i++)hash[i]=0;
    status=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0);
    if(status<0)return AMDGPU_STATE_IO;
    status=BCryptHash(algorithm,NULL,0,canonical,characters*2,hash,32);
    {
        NTSTATUS closed=BCryptCloseAlgorithmProvider(algorithm,0);
        if(status>=0 && closed<0)status=closed;
    }
    if(status<0){for(i=0;i<32;i++)hash[i]=0;return AMDGPU_STATE_IO;}
    return AMDGPU_STATE_OK;
}
