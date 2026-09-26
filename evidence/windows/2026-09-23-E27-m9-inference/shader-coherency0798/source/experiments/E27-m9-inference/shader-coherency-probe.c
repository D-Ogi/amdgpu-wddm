// Reuse the portable E14 Vulkan setup, shader and exact CPU oracle unchanged.
#define main bc250_reference_main
#include "../E14-vulkan-compute-reference/vkcompute.c"
#undef main

int main(int argc,char** argv)
{
 struct ctx c={0};struct gbuf in,intermediate,out;struct gbuf* bind[2];
 struct gpipe first,second;struct step steps[2]={{0}};
 uint32_t push=BIG_N,*expected,seed=SEED_INTHASH,round,i;int stale=0;
 if(argc<2 || !strcmp(argv[1],"--help")) {
  printf("usage: shader-coherency-probe <shader-dir> [--stale-input-control]\n");
  return argc<2?2:0;
 }
 if(argc>3 || (argc==3 && strcmp(argv[2],"--stale-input-control")))return 2;
 stale=argc==3;c.spvdir=argv[1];c.runs=1;
 ctx_init(&c);
 buf_create(&c,&in,(VkDeviceSize)BIG_N*4);
 buf_create(&c,&intermediate,(VkDeviceSize)BIG_N*4);
 buf_create(&c,&out,(VkDeviceSize)BIG_N*4);
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
  us=run_steps(&c,steps,2);
  snprintf(name,sizeof(name),"coherency%02u",round);
  if(!report(&c,name,BIG_N,out.map,expected,(size_t)BIG_N*4,us,NULL))break;
 }
 printf("COHERENCY completed=%u mismatches=%d\n",round,c.failures);
 free(expected);pipe_destroy(&c,&second);pipe_destroy(&c,&first);
 buf_destroy(&c,&out);buf_destroy(&c,&intermediate);buf_destroy(&c,&in);ctx_fini(&c);
 return c.failures?1:0;
}
