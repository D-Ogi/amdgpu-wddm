"""Compile actual planner and consumer guard with a fake allocation boundary."""
from pathlib import Path
import subprocess
import argparse
ap=argparse.ArgumentParser();ap.add_argument('--source',required=True);ap.add_argument('--out',required=True);args=ap.parse_args()
p=Path(args.out).resolve();p.mkdir(parents=True,exist_ok=True);s=Path(args.source)/'ggml/src';cuda=(s/'ggml-cuda/ggml-cuda.cu').read_text();backend=(s/'ggml-backend.cpp').read_text()
def function(text,name):
 start=text.index(name);start=text.rfind('\n',0,start)+1;brace=text.index('{',start);depth=1;end=brace+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
planner=backend[backend.index('struct ggml_backend_buft_alloc_buffer_n_plan_item'):backend.index('// default implementation of alloc_buffer_n')]
limit=function(cuda,'static size_t ggml_backend_cuda_buffer_type_get_max_size(')
alloc=function(cuda,'static ggml_backend_buffer_t ggml_backend_cuda_buffer_type_alloc_buffer(')
preamble=r'''
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define GGML_UNUSED(x) (void)(x)
#define GGML_PAD(x,a) (((x)+(a)-1)&~((a)-1))
#define GGML_LOG_ERROR(...) ((void)0)
struct ggml_tensor { void* data; ggml_tensor* view_src; size_t bytes; };
struct ggml_backend_cuda_buffer_type_context {int device;};
struct BufferType {void* context;};
using ggml_backend_buffer_type_t=BufferType*;
using ggml_backend_buffer_t=void*;
int calls=0; int device_calls=0; size_t last=0;
struct ggml_backend_cuda_buffer_context {ggml_backend_cuda_buffer_context(int,void*){}};
int ggml_backend_cuda_buffer_interface=0;
using cudaError_t=int; const int cudaSuccess=0;
void ggml_cuda_set_device(int){++device_calls;}
int ggml_cuda_device_malloc(void** p,size_t n,int){++calls;last=n;*p=nullptr;return 1;}
int cudaGetLastError(){return 0;} const char* cudaGetErrorString(int){return "fake";}
void* ggml_backend_buffer_init(BufferType*,int,void*,size_t){return nullptr;}
size_t ggml_backend_buft_get_alignment(BufferType*){return 128;}
size_t ggml_backend_buft_get_alloc_size(BufferType*,ggml_tensor*t){return t->bytes;}
'''
source=preamble+limit+'\nsize_t ggml_backend_buft_get_max_size(BufferType*b){return ggml_backend_cuda_buffer_type_get_max_size(b);}\n'+planner+alloc+r'''
int main(){
 ggml_backend_cuda_buffer_type_context c{0}; BufferType b{&c};
 const size_t cap=0xfffc0000ULL; int failures=0;
 #define CHECK(x) do{if(!(x)){std::printf("FAIL CHECK %s\n",#x);++failures;}}while(0)
 ggml_tensor a{nullptr,nullptr,cap-128},d{nullptr,nullptr,128},e{nullptr,nullptr,129};
 ggml_tensor*ts[]={&a,&d,&e}; auto plan=ggml_backend_buft_alloc_buffer_n_plan(&b,ts,3);
#ifdef BC250_HIP_MAX_ALLOC_BYTES
 CHECK(plan.size()==2); CHECK(plan[0].size==cap); CHECK(plan[1].size==256);
 ggml_backend_cuda_buffer_type_alloc_buffer(&b,cap+1); CHECK(calls==0);CHECK(device_calls==0);
 ggml_backend_cuda_buffer_type_alloc_buffer(&b,cap);CHECK(calls==1);CHECK(last==cap);
 ggml_tensor huge{nullptr,nullptr,cap+1};ggml_tensor* one[]={&huge};
 auto hp=ggml_backend_buft_alloc_buffer_n_plan(&b,one,1);CHECK(hp.size()==1);CHECK(hp[0].size==cap+128);
 ggml_backend_cuda_buffer_type_alloc_buffer(&b,hp[0].size);CHECK(calls==1);
#else
 CHECK(plan.size()==1);CHECK(plan[0].size==cap+256);
 ggml_backend_cuda_buffer_type_alloc_buffer(&b,cap+1);CHECK(calls==1);CHECK(last==cap+1);
#endif
 std::printf("failures=%d\n",failures);return failures?1:0;
}
'''
(p/'planner-test.cpp').write_text(source)
# The no-op guard mutant must compile and fail runtime checks.
(p/'planner-mutant.cpp').write_text(source.replace('if (size > ggml_backend_cuda_buffer_type_get_max_size(buft))','if (false)'))
print('generated actual-source planner, callback, allocation guard and guard mutant')
