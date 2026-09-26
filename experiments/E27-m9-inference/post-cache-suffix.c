
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
