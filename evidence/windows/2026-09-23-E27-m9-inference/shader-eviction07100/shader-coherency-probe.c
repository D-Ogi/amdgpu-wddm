// Reuse the portable E14 Vulkan setup, shader and exact CPU oracle unchanged.
#define main bc250_reference_main
#define buf_create reference_buf_create
#include "../E14-vulkan-compute-reference/vkcompute.c"
#undef main
#undef buf_create

static void buf_create_type(struct ctx* c,struct gbuf* b,VkDeviceSize size,int requested)
{
 VkBufferCreateInfo bi={0};VkMemoryRequirements req;VkMemoryAllocateInfo ai={0};
 VkMemoryPropertyFlags flags,want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 uint32_t type;
 memset(b,0,sizeof(*b));bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;bi.size=size;
 bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 VK_CHECK(vkCreateBuffer(c->dev,&bi,NULL,&b->buf));
 vkGetBufferMemoryRequirements(c->dev,b->buf,&req);
 type=requested<0?pick_memtype(c,req.memoryTypeBits,want):(uint32_t)requested;
 if(type>=c->memprops.memoryTypeCount || !(req.memoryTypeBits&(1u<<type)))die("memory type is not compatible with this buffer");
 flags=c->memprops.memoryTypes[type].propertyFlags;
 if((flags&want)!=want || (flags&~(want|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT)))
  die("probe requires ordinary host-visible coherent memory without extension properties");
 ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=type;
 VK_CHECK(vkAllocateMemory(c->dev,&ai,NULL,&b->mem));
 VK_CHECK(vkBindBufferMemory(c->dev,b->buf,b->mem,0));
 VK_CHECK(vkMapMemory(c->dev,b->mem,0,VK_WHOLE_SIZE,0,&b->map));b->size=size;
 printf("BUFFER memory_type=%u heap=%u flags=0x%x bytes=%llu\n",type,c->memprops.memoryTypes[type].heapIndex,flags,(unsigned long long)size);
 memset(b->map,0,(size_t)size);
}

int main(int argc,char** argv)
{
 struct ctx c={0};struct gbuf in,intermediate,out;struct gbuf* bind[2];
 struct gpipe first,second;struct step steps[2]={{0}};
 uint32_t push=BIG_N,*expected,seed=SEED_INTHASH,round,i;int stale=0,evict=0,requested=-1,arg;
 if(argc<2 || !strcmp(argv[1],"--help")) {
  printf("usage: shader-coherency-probe <shader-dir> [--stale-input-control] [--memory-type N] [--evict-input]\n");
  return argc<2?2:0;
 }
 for(arg=2;arg<argc;arg++) {
  if(!strcmp(argv[arg],"--evict-input"))evict=1;
  else if(!strcmp(argv[arg],"--stale-input-control"))stale=1;
  else if(!strcmp(argv[arg],"--memory-type") && arg+1<argc) {
   char* end;long value=strtol(argv[++arg],&end,10);
   if(!*argv[arg] || *end || value<0 || value>=32)return 2;
   requested=(int)value;
  } else return 2;
 }
 c.spvdir=argv[1];c.runs=1;
 ctx_init(&c);
 buf_create_type(&c,&in,(VkDeviceSize)BIG_N*4,requested);
 buf_create_type(&c,&intermediate,(VkDeviceSize)BIG_N*4,requested);
 buf_create_type(&c,&out,(VkDeviceSize)BIG_N*4,requested);
 pipe_create(&c,&first,"inthash.spv",2,sizeof(push));
 pipe_create(&c,&second,"inthash.spv",2,sizeof(push));
 bind[0]=&in;bind[1]=&intermediate;pipe_bind(&c,&first,bind);
 bind[0]=&intermediate;bind[1]=&out;pipe_bind(&c,&second,bind);
 steps[0].p=&first;steps[1].p=&second;
 for(i=0;i<2;i++){steps[i].gx=BIG_N/WG;steps[i].gy=steps[i].gz=1;steps[i].push=&push;steps[i].push_size=sizeof(push);}
 expected=(uint32_t*)xmalloc((size_t)BIG_N*4);
 printf("COHERENCY rounds=16 words=%u same_allocations=1 host_coherent=1 dispatches_per_round=2 stale_control=%d\n",BIG_N,stale);
 for(round=0;round<16;round++) {
  char name[32];double us;
  // Previous round's fence has completed before any host access. HOST_COHERENT
  // plus queue submission supplies the host-write domain operation. run_steps
  // records shader-write/read and shader-write/host-read barriers.
  for(i=0;i<BIG_N;i++) {
   uint32_t value=lcg_next(&seed);
   expected[i]=mix32(mix32(value));
   if(!stale || round!=1)((uint32_t*)in.map)[i]=value;
   ((uint32_t*)intermediate.map)[i]=expected[i]^0x55AA55AAu;
   ((uint32_t*)out.map)[i]=~expected[i];
  }
  if(evict && (round==1 || round==5 || round==9)) {
   VK_CHECK(vkDeviceWaitIdle(c.dev));
   if(!SetEnvironmentVariableA("BC250_TEST_EVICT_ON_UNMAP","1"))die("cannot arm eviction diagnostic");
   printf("EVICT_INPUT round=%u bytes=%llu\n",round,(unsigned long long)in.size);fflush(stdout);
   vkUnmapMemory(c.dev,in.mem);in.map=NULL;
   // The isolated diagnostic ICD consumes this flag only after a full witnessed
   // eviction/restoration sequence. A regular ICD must not silently pass.
   {char armed[8];if(GetEnvironmentVariableA("BC250_TEST_EVICT_ON_UNMAP",armed,sizeof(armed)))die("eviction diagnostic ICD did not consume request");}
  }
  us=run_steps(&c,steps,2);
  snprintf(name,sizeof(name),"coherency%02u",round);
  if(!report(&c,name,BIG_N,out.map,expected,(size_t)BIG_N*4,us,NULL))break;
  if(!in.map)VK_CHECK(vkMapMemory(c.dev,in.mem,0,VK_WHOLE_SIZE,0,&in.map));
 }
 printf("COHERENCY completed=%u mismatches=%d\n",round,c.failures);
 free(expected);pipe_destroy(&c,&second);pipe_destroy(&c,&first);
 buf_destroy(&c,&out);buf_destroy(&c,&intermediate);buf_destroy(&c,&in);ctx_fini(&c);
 return c.failures?1:0;
}
