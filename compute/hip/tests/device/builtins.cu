// Live-input builtin conformance, one 64-thread block (two wave32 waves).
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <hip/hip_bf16.h>
#include <hip/hip_cooperative_groups.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
static constexpr unsigned N=64, Stride=40, Guard=16, Canary=0xa5a5a5a5u;
__global__ void builtins(unsigned* out,const unsigned* selectors,const double* values,
                         int* isum,float* fsum,float* exchange) {
  const unsigned t=threadIdx.x;
  if(t>=N) return;
  unsigned* r=out+Guard+t*Stride;
  const int lane=__lane_id();
  for(int k=0;k<6;k++) {
    const int width=1<<k;
    r[4*k]=(unsigned)__shfl(lane,3,width);
    r[4*k+1]=(unsigned)__shfl_up(lane,1,width);
    r[4*k+2]=(unsigned)__shfl_down(lane,1,width);
    r[4*k+3]=(unsigned)__shfl_xor(lane,1,width);
  }
  const unsigned long long mask=(lane&1)?0xaaaaaaaaULL:0x55555555ULL;
  r[24]=(unsigned)__ballot_sync(mask,(lane&3)==0);
  r[25]=(unsigned)__syncthreads_count((t&1)!=0);
  r[26]=(unsigned)__syncthreads_and(t<63);
  r[27]=(unsigned)__syncthreads_or(t==63);
  r[28]=(unsigned)atomicAdd(isum,1);
  r[29]=__float_as_uint(atomicAdd(fsum,1.0f));
  r[30]=__float_as_uint(atomicExch(exchange,1.0f));
  r[31]=__byte_perm(0x03020100u,0x07060504u,selectors[t]);
  r[32]=__half_as_ushort(__double2half(values[t]));
  r[33]=__double2bfloat16(values[t]).data;
  auto g=cooperative_groups::this_grid();
  r[34]=g.thread_rank();r[35]=g.size();r[36]=g.is_valid();r[37]=warpSize;
  r[38]=(unsigned)__syncthreads_count(1);
}
static int errors=0;
static void check(bool ok,const char* what,unsigned index){if(!ok){if(errors<12)std::printf("FAIL %s index %u\n",what,index);++errors;}}
static bool api(hipError_t s,const char* what){if(s!=hipSuccess){std::printf("API FAIL %s: %d\n",what,(int)s);return false;}return true;}
int main(){
  _putenv_s("BC250_HIP_WAIT_TOTAL_MS","10000");
  // Independent exact bit expectations, including values straddling a rounding midpoint.
  const double inputs[]={0.0,-0.0,1.0,1.0004882812500002,-1.0004882812500002,
    65504.0,65520.0,0x1p-24,0x1p-25,1.0039062500000002,-1.0039062500000002};
  const unsigned halves[]={0,0x8000,0x3c00,0x3c01,0xbc01,0x7bff,0x7c00,1,0,0x3c04,0xbc04};
  const unsigned bfloats[]={0,0x8000,0x3f80,0x3f80,0xbf80,0x4780,0x4780,0x3380,0x3300,0x3f81,0xbf81};
  const unsigned masks[]={0x3210,0x7654,0x4567,0xfedc,0xffff,0,0x6240};
  double values[N];unsigned selectors[N];
  for(unsigned t=0;t<N;t++){values[t]=inputs[t%11];selectors[t]=masks[t%7];}
  std::vector<unsigned> out(Guard+N*Stride+Guard,Canary);
  unsigned* d=nullptr;unsigned* ds=nullptr;double* dv=nullptr;int* di=nullptr;float* df=nullptr;float* de=nullptr;
  if(!api(hipMalloc((void**)&d,out.size()*sizeof(unsigned)),"malloc output") ||
     !api(hipMalloc((void**)&ds,sizeof(selectors)),"malloc selectors") ||
     !api(hipMalloc((void**)&dv,sizeof(values)),"malloc values") ||
     !api(hipMalloc((void**)&di,sizeof(int)),"malloc int") ||
     !api(hipMalloc((void**)&df,sizeof(float)),"malloc float") ||
     !api(hipMalloc((void**)&de,sizeof(float)),"malloc exchange")) return 2;
  int zero=0;
  if(!api(hipMemcpy(d,out.data(),out.size()*4,hipMemcpyHostToDevice),"upload output") ||
     !api(hipMemcpy(ds,selectors,sizeof(selectors),hipMemcpyHostToDevice),"upload selectors") ||
     !api(hipMemcpy(dv,values,sizeof(values),hipMemcpyHostToDevice),"upload values") ||
     !api(hipMemcpy(di,&zero,4,hipMemcpyHostToDevice),"zero int") ||
     !api(hipMemcpy(df,&zero,4,hipMemcpyHostToDevice),"zero float") ||
     !api(hipMemcpy(de,&zero,4,hipMemcpyHostToDevice),"zero exchange")) return 2;
  builtins<<<dim3(1),dim3(N),0,nullptr>>>(d,ds,dv,di,df,de);
  if(!api(hipGetLastError(),"launch")||!api(hipDeviceSynchronize(),"synchronize") ||
     !api(hipMemcpy(out.data(),d,out.size()*4,hipMemcpyDeviceToHost),"read output"))return 2;
  int finalI=-1;float finalF=-1,finalE=-1;
  if(!api(hipMemcpy(&finalI,di,4,hipMemcpyDeviceToHost),"read int") ||
     !api(hipMemcpy(&finalF,df,4,hipMemcpyDeviceToHost),"read float") ||
     !api(hipMemcpy(&finalE,de,4,hipMemcpyDeviceToHost),"read exchange"))return 2;
  unsigned seenI[N]={},seenF[N]={},exchangeZeros=0;
  for(unsigned t=0;t<N;t++){
    const unsigned* r=out.data()+Guard+t*Stride;const unsigned lane=t%32;
    for(unsigned k=0;k<6;k++){
      const unsigned width=1u<<k,start=lane&~(width-1),end=start+width;
      check(r[4*k]==start+(3u&(width-1)),"shfl",t);
      check(r[4*k+1]==(lane==start?lane:lane-1),"shfl up",t);
      check(r[4*k+2]==(lane+1>=end?lane:lane+1),"shfl down",t);
      check(r[4*k+3]==((lane^1u)>=end?lane:lane^1u),"shfl xor",t);
    }
    check(r[24]==((lane&1)?0u:0x11111111u),"ballot mask",t);
    check(r[25]==32&&r[26]==0&&r[27]==1&&r[38]==64,"block collectives",t);
    check(r[28]<N,"atomic integer old range",t);if(r[28]<N)++seenI[r[28]];
    float f;std::memcpy(&f,r+29,4);bool valid=f>=0&&f<64&&(unsigned)f==f;
    check(valid,"atomic float old range",t);if(valid)++seenF[(unsigned)f];
    check(r[30]==0||r[30]==0x3f800000u,"exchange old range",t);exchangeZeros+=r[30]==0;
    unsigned expected=0;for(unsigned i=0;i<4;i++)expected|=((selectors[t]>>(4*i))&7u)<<(8*i);
    check(r[31]==expected,"byte perm",t);check(r[32]==halves[t%11],"double half",t);
    check(r[33]==bfloats[t%11],"double bf16",t);
    check(r[34]==t&&r[35]==N&&r[36]==0&&r[37]==32,"group geometry",t);
  }
  for(unsigned i=0;i<N;i++)check(seenI[i]==1&&seenF[i]==1,"atomic old permutation",i);
  for(unsigned i=0;i<Guard;i++)check(out[i]==Canary&&out[out.size()-1-i]==Canary,"guards",i);
  check(finalI==64&&finalF==64&&finalE==1&&exchangeZeros==1,"atomic final values",0);
  for(void* p:{(void*)d,(void*)ds,(void*)dv,(void*)di,(void*)df,(void*)de})if(!api(hipFree(p),"free"))++errors;
  std::printf("builtins: %s, 64 threads, 2 wave32 waves, failures=%d\n",errors?"FAIL":"PASS",errors);
  return errors?1:0;
}
