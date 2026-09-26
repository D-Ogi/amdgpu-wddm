#include <windows.h>
#include <d3d9types.h>
#include <d3dumddi.h>
#include <stddef.h>
#include <stdio.h>
int main(){printf("WDK ALLOCATE ABI size=%zu private=%zu privateSize=%zu hResource=%zu hKMResource=%zu NumAllocations=%zu info2=%zu\n",sizeof(D3DDDICB_ALLOCATE),offsetof(D3DDDICB_ALLOCATE,pPrivateDriverData),offsetof(D3DDDICB_ALLOCATE,PrivateDriverDataSize),offsetof(D3DDDICB_ALLOCATE,hResource),offsetof(D3DDDICB_ALLOCATE,hKMResource),offsetof(D3DDDICB_ALLOCATE,NumAllocations),offsetof(D3DDDICB_ALLOCATE,pAllocationInfo2));}
