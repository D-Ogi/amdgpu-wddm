static void reset(BC250_GFX*g,struct amdgpu_device*a){
 memset(g,0,sizeof(*g));memset(a,0,sizeof(*a));g->PagingGate=1;
 allocs=frees=gfxCalls=sdmaCalls=gfxFrees=sdmaFrees=0;
 failGfx=failSdma=failAlloc=badSize=badAlign=0;
}
int main(void){
 BC250_GFX g;struct amdgpu_device a;
 reset(&g,&a);g.PagingGate=0;
 CHECK(SetUp(&g,&a)==0 && g.SetUp && !allocs && !g.PagingCopyStaging.size);
 PagingCopyStorageFree(&g,&a);CHECK(!frees);
 reset(&g,&a);
 CHECK(SetUp(&g,&a)==0 && g.SetUp && allocs==1 && g.PagingCopyStaging.size==4096 && a.doorbell.base==0xABC000);
 CHECK(SetUp(&g,&a)==0 && allocs==1 && gfxCalls==1 && sdmaCalls==1);
 CHECK(PagingCopyStorageInit(&g,&a)==0 && allocs==1);
 PagingCopyStorageFree(&g,&a);CHECK(frees==1 && !g.PagingCopyStaging.size && !g.PagingCopyStaging.cpu && !g.PagingCopyStaging.mc);
 PagingCopyStorageFree(&g,&a);CHECK(frees==1);
 reset(&g,&a);failGfx=1;
 CHECK(SetUp(&g,&a)==-1 && !g.SetUp && gfxCalls==1 && !sdmaCalls && !allocs && !gfxFrees);
 reset(&g,&a);failSdma=1;
 CHECK(SetUp(&g,&a)==-2 && !g.SetUp && gfxFrees==1 && !sdmaFrees && !allocs);
 reset(&g,&a);failAlloc=1;
 CHECK(SetUp(&g,&a)==-12 && !g.SetUp && allocs==1 && gfxFrees==1 && sdmaFrees==1 && !g.PagingCopyStaging.size);
 reset(&g,&a);badSize=1;
 CHECK(SetUp(&g,&a)==BC250_EINVAL && !g.SetUp && frees==1 && gfxFrees==1 && sdmaFrees==1 && !g.PagingCopyStaging.size);
 reset(&g,&a);badAlign=1;
 CHECK(SetUp(&g,&a)==BC250_EINVAL && !g.SetUp && frees==1 && gfxFrees==1 && sdmaFrees==1 && !g.PagingCopyStaging.size);
 printf("PTE staging setup: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
