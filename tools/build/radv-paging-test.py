"""Test extracted production paging blocks; no driver or GPU is loaded."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

SOURCE_PATH = "src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c"

HEADER = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s)>=0)
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define VK_ERROR_OUT_OF_DEVICE_MEMORY -2
#define RADEON_FLAG_INTERNAL 1
#define MIN2(a,b) ((a)<(b)?(a):(b))
typedef int NTSTATUS;
typedef struct {unsigned hPagingQueue,NumAllocations;unsigned *AllocationList;struct{unsigned CantTrimFurther,MustSucceed;} Flags;uint64_t PagingFenceValue;} D3DDDI_MAKERESIDENT;
typedef struct {unsigned hDevice,ObjectCount;unsigned *ObjectHandleArray;const uint64_t *FenceValueArray;} D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU;
typedef struct {unsigned hPagingQueue;uint64_t BaseAddress,SizeInPages;unsigned hAllocation;struct {unsigned Zero;} Protection;uint64_t PagingFenceValue;} D3DDDI_MAPGPUVIRTUALADDRESS;
struct bo {struct{unsigned handle;uint64_t size,va;} base;unsigned handle;uint64_t size;};
struct ws {int host;unsigned device_h,paging_fence_h,paging_queue_h;struct{struct bo*bo;}null_prt;};
static int resident_status,wait_status,wait_calls,map_calls;static uint64_t resident_value,wait_value;
static uint64_t map_values[3];static int map_status[3];
static int MakeResident(D3DDDI_MAKERESIDENT*p){p->PagingFenceValue=resident_value;return resident_status;}
static int WaitForSynchronizationObjectFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU*p){wait_calls++;wait_value=*p->FenceValueArray;return wait_status;}
static int MapGpuVirtualAddress(D3DDDI_MAPGPUVIRTUALADDRESS*p){int n=map_calls++;p->PagingFenceValue=map_values[n];return map_status[n];}
#define BC250_WDDM_CALL(host,fn,arg) fn(arg)
static struct bo actual_bo={{1,8192,0},0,0},null_bo={{2,4096,0},2,4096};static struct ws actual_ws={0,1,2,3,{&null_bo}};
'''

TEST_MAIN = r'''
static int checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line %d: %s\n",__LINE__,#x);}}while(0)
static void reset(void){resident_status=wait_status=wait_calls=map_calls=0;resident_value=wait_value=0;for(int i=0;i<3;i++){map_values[i]=0;map_status[i]=0;}}
int main(void){
 for(int f=0;f<2;f++){
  int(*run)(uint64_t)=f?imported:create;
  reset();CHECK(run(17)==1);CHECK(wait_calls==1 && wait_value==17);
  reset();resident_status=0x103;resident_value=23;CHECK(run(17)==1);CHECK(wait_calls==1&&wait_value==23);
  reset();resident_status=0x103;resident_value=12;CHECK(run(17)==1);CHECK(wait_calls==1&&wait_value==17);
  reset();CHECK(run(0)==1);CHECK(wait_calls==0);
  reset();resident_status=-1;CHECK(run(17)==-2);CHECK(wait_calls==0);
  reset();wait_status=-1;CHECK(run(17)==-2);CHECK(wait_calls==1);
  reset();resident_value=999;CHECK(run(17)==1);CHECK(wait_value==17);
 }
 reset();map_values[0]=23;map_status[0]=0x103;CHECK(sparse(17));CHECK(wait_calls==1&&wait_value==23);
 reset();CHECK(sparse(0));CHECK(wait_calls==0);
 reset();map_status[0]=-1;CHECK(!sparse(17));CHECK(wait_calls==0);
 reset();wait_status=-1;CHECK(!sparse(17));CHECK(wait_calls==1);
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
'''


def extract(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def generate(source):
    create = extract(source, "   uint64_t paging_fence_value = map.PagingFenceValue;", "   if (ws->debug_all_bos)")
    sparse = extract(source, "   uint64_t fence = map.PagingFenceValue;", "   return true;")
    imported = extract(source, "   /* Make the allocation resident. An opened allocation", "   free(pdata);")
    common = "struct ws *ws = &actual_ws; struct bo *bo = &actual_bo; D3DDDI_MAPGPUVIRTUALADDRESS map = {0}; map.PagingFenceValue = value; int status = 0; "
    functions = [
        "static int create(uint64_t value) {" + common + "int result = 0; bool all_resident = true; unsigned flags = 0;" + create + "return 1; error_va_alloc: return result;}\n",
        "static int imported(uint64_t value) {" + common + "int result = 0;" + imported + "return 1; error_map: return result;}\n",
        "static int sparse(uint64_t value) {" + common + "uint64_t low = 4096, high = 8192;" + sparse + "return true;}\n",
    ]
    return HEADER + "".join(functions) + TEST_MAIN


def run_logged(command, directory, stem, timeout):
    start = time.monotonic()
    try:
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=timeout)
        output = result.stdout + result.stderr
        status = {"command": command, "exit_code": result.returncode, "timed_out": False}
    except subprocess.TimeoutExpired as error:
        output = str(error)
        status = {"command": command, "exit_code": None, "timed_out": True}
    status["seconds"] = time.monotonic() - start
    (directory / (stem + ".log")).write_text(output, encoding="utf-8")
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--revision")
    args = parser.parse_args()
    source_dir = args.source.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    if args.revision:
        raw = subprocess.check_output(["git", "-C", str(source_dir), "show", args.revision + ":" + SOURCE_PATH], timeout=30)
    else:
        raw = (source_dir / SOURCE_PATH).read_bytes()
    revision = subprocess.check_output(["git", "-C", str(source_dir), "rev-parse", args.revision or "HEAD"], text=True, timeout=30).strip()
    record = {"source": str(source_dir / SOURCE_PATH), "revision": revision, "worktree_source": not bool(args.revision), "source_sha256": hashlib.sha256(raw).hexdigest()}
    generated = output / "paging-test.c"
    generated.write_text(generate(raw.decode("utf-8-sig")), encoding="utf-8")
    record["generated_sha256"] = hashlib.sha256(generated.read_bytes()).hexdigest()
    executable = output / "paging-test.exe"
    record["compile"] = run_logged(["cl", "/nologo", "/std:c11", "/W4", "/WX", "/TC", str(generated), "/Fo" + str(output / "paging-test.obj"), "/Fe" + str(executable)], output, "build", 60)
    if record["compile"]["exit_code"] == 0:
        record["run"] = run_logged([str(executable)], output, "result", 15)
        print((output / "result.log").read_text(), end="")
    (output / "record.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return 0 if record.get("run", {}).get("exit_code") == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
