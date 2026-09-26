#define BC250_GPU_PROBE_NO_MAIN
#include "gpu-residency-probe.c"
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static char readyPath[MAX_PATH],releasePath[MAX_PATH];
static void ResetPaths(void){DeleteFileA(readyPath);DeleteFileA(releasePath);}
static void WriteGateRelease(DWORD pid){FILE*f=fopen(releasePath,"wb");CHECK(f!=NULL);if(f){fprintf(f,"%lu\r\n",pid);CHECK(fclose(f)==0);}}
static DWORD WINAPI ReleaseWorker(LPVOID arg){ULONGLONG start=GetTickCount64();while(GetTickCount64()-start<3000){if(GetFileAttributesA(readyPath)!=INVALID_FILE_ATTRIBUTES){WriteGateRelease(GetCurrentProcessId()+(DWORD)(ULONG_PTR)arg);return 0;}Sleep(10);}return 1;}
static BOOL Parse(int argc,const char**args,GPU_OPTIONS*o){return ParseGpuOptions(argc,(char**)args,o);}
int main(int argc,char**argv){
 GPU_OPTIONS o;PROBE p={0},saved;BUFFER dst={0},savedDst;HANDLE thread;DWORD code;char data[512];FILE*f;size_t n;
 const char*def[]={"probe"};
 const char*resident[]={"probe","12884901888","vram","--resident-only"};
 const char*gate[]={"probe","67108864","vram","--resume-gate","a.ready","a.release","600000"};
 const char*badTimeout[]={"probe","4096","gtt","--resume-gate","a","b","1800001"};
 const char*zeroTimeout[]={"probe","4096","gtt","--resume-gate","a","b","0"};
 const char*missing[]={"probe","4096","vram","--resume-gate","a","b"};
 const char*same[]={"probe","4096","vram","--resume-gate","A","a","1000"};
 const char*conflict[]={"probe","4096","vram","--resident-only","--resume-gate","a","b","1000"};
 const char*unknown[]={"probe","4096","vram","--unknown"};
 const char*unaligned[]={"probe","4097","vram"};
 const char*tooBig[]={"probe","1073745920","vram","--resume-gate","a","b","1000"};
 if(argc!=2)return 2;
 CHECK(Parse(1,def,&o)&&o.Bytes==65536&&o.Heap==AMDGPU_GEM_DOMAIN_VRAM&&!o.ReadyPath);
 CHECK(Parse(4,resident,&o)&&o.Bytes==(12ull<<30)&&o.ResidentOnly);
 CHECK(Parse(7,gate,&o)&&o.Bytes==(64ull<<20)&&o.GateTimeoutMs==600000&&!o.ResidentOnly);
 CHECK(!Parse(7,badTimeout,&o));CHECK(!Parse(7,zeroTimeout,&o));CHECK(!Parse(6,missing,&o));CHECK(!Parse(7,same,&o));
 CHECK(!Parse(8,conflict,&o));CHECK(!Parse(4,unknown,&o));CHECK(!Parse(3,unaligned,&o));CHECK(!Parse(7,tooBig,&o));
 CHECK(snprintf(readyPath,sizeof(readyPath),"%s/gate-test-%lu.ready",argv[1],GetCurrentProcessId())<(int)sizeof(readyPath));
 CHECK(snprintf(releasePath,sizeof(releasePath),"%s/gate-test-%lu.release",argv[1],GetCurrentProcessId())<(int)sizeof(releasePath));
 ResetPaths();ZeroMemory(&o,sizeof(o));o.ReadyPath=readyPath;o.ReleasePath=releasePath;o.GateTimeoutMs=5000;
 p.hAdapter=11;p.hDevice=12;p.hContext=13;p.hPagingQueue=14;p.hPagingFenceObject=15;p.hFence=16;
 p.Data.hAllocation=21;p.Data.MappedVa=0x100000;p.Command.hAllocation=22;p.Command.MappedVa=0x200000;
 dst.hAllocation=23;dst.MappedVa=0x300000;saved=p;savedDst=dst;
 thread=CreateThread(NULL,0,ReleaseWorker,NULL,0,NULL);CHECK(thread!=NULL);
 CHECK(WaitResumeGate(&p,&dst,64,&o));
 if(thread){CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CHECK(GetExitCodeThread(thread,&code)&&code==0);CloseHandle(thread);}
 CHECK(!memcmp(&p,&saved,sizeof(p))&&!memcmp(&dst,&savedDst,sizeof(dst)));
 f=fopen(readyPath,"rb");CHECK(f!=NULL);if(f){n=fread(data,1,sizeof(data)-1,f);data[n]=0;fclose(f);CHECK(strstr(data,"first_full_readback=PASS")&&strstr(data,"sequence=64"));}
 CHECK(!WaitResumeGate(&p,&dst,64,&o)); // stale release must fail before waiting
 DeleteFileA(releasePath);CHECK(!WaitResumeGate(&p,&dst,64,&o)); // never overwrite ready evidence
 ResetPaths();o.GateTimeoutMs=100;CHECK(!WaitResumeGate(&p,&dst,64,&o)); // bounded timeout
 ResetPaths();o.GateTimeoutMs=5000;thread=CreateThread(NULL,0,ReleaseWorker,(LPVOID)1,0,NULL);CHECK(thread!=NULL);CHECK(!WaitResumeGate(&p,&dst,64,&o));if(thread){CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);}
 ResetPaths();printf("resume gate actual-source: %u checks, %u failures; no GPU calls\n",checks,failures);return failures?1:0;
}

