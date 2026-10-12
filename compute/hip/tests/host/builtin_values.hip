#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <hip/hip_bf16.h>
#include <hip/hip_cooperative_groups.h>
__global__ void case_0(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,0u)); }
__global__ void case_1(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,12816u)); }
__global__ void case_2(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,30292u)); }
__global__ void case_3(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,17767u)); }
__global__ void case_4(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,65244u)); }
__global__ void case_5(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,65535u)); }
__global__ void case_6(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,291u)); }
__global__ void case_7(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,30583u)); }
__global__ void case_8(unsigned *p) { *p = (unsigned)(__byte_perm(0x03020100u,0x07060504u,25152u)); }
__global__ void case_9(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x0.0p+0))); }
__global__ void case_10(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(-0x0.0p+0))); }
__global__ void case_11(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.0000000000000p+0))); }
__global__ void case_12(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.0020000000001p+0))); }
__global__ void case_13(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(-0x1.0020000000001p+0))); }
__global__ void case_14(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffc0000000000p+15))); }
__global__ void case_15(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffde000000000p+15))); }
__global__ void case_16(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffe0000000000p+15))); }
__global__ void case_17(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffdffffffff80p+15))); }
__global__ void case_18(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffe0000000080p+15))); }
__global__ void case_19(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffffffffffffep-26))); }
__global__ void case_20(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffbffffffffffp-15))); }
__global__ void case_21(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffc0000000001p-15))); }
__global__ void case_22(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.0000000000000p-24))); }
__global__ void case_23(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.0000000000000p-25))); }
__global__ void case_24(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.0000000000001p-25))); }
__global__ void case_25(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.ffc0000000000p-15))); }
__global__ void case_26(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.56e1fc2f8f359p-997))); }
__global__ void case_27(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(0x1.7e43c8800759cp+996))); }
__global__ void case_28(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(__builtin_inf()))); }
__global__ void case_29(unsigned *p) { *p = (unsigned)(__half_as_ushort(__double2half(-__builtin_inf()))); }
__global__ void case_30(unsigned *p) { *p = (unsigned)(__half_as_ushort(__hmax_nan(__ushort_as_half(0x7e00),__float2half(1)))); }
__global__ void case_31(unsigned *p) { *p = (unsigned)(__half_as_ushort(__hmin_nan(__float2half(1),__ushort_as_half(0x7e00)))); }
__global__ void case_32(unsigned *p) { *p = (unsigned)(__half_as_ushort(__hmax(__float2half(1),__ushort_as_half(0x7e00)))); }
__global__ void case_33(unsigned *p) { *p = (unsigned)(__half_as_ushort(__hmin(__ushort_as_half(0x7e00),__float2half(1)))); }
__global__ void case_34(unsigned *p) { *p = (unsigned)(__half_as_ushort(__hfma_relu(__ushort_as_half(0x7e00),__float2half(1),__float2half(1)))); }
__global__ void case_35(unsigned *p) { *p = (unsigned)(__half_as_ushort(__hmax(__ushort_as_half(0x8000),__ushort_as_half(0)))); }
__global__ void case_36(unsigned *p) { *p = (unsigned)(__half_as_ushort(__hmin(__ushort_as_half(0),__ushort_as_half(0x8000)))); }
__global__ void case_37(unsigned *p) { *p = (unsigned)(alignof(__hip_bfloat162)); }
__global__ void case_38(unsigned *p) { *p = (unsigned)(alignof(__hip_bfloat162_raw)); }
__global__ void case_39(unsigned *p) { *p = (unsigned)(__double2bfloat16(1.0039062500000002).data); }
__global__ void case_40(unsigned *p) { *p = (unsigned)(__double2bfloat16(-1.0039062500000002).data); }
__global__ void atomic_contract(float*p,int*q){atomicExch(p,1.0f);atomicAdd(q,1);atomicAdd_system(q,1);}
__global__ void ballot_contract(unsigned long long*p){*p=__ballot_sync(0x55555555ULL,1);}
__global__ void group_contract(unsigned*p){auto g=cooperative_groups::this_grid(); p[0]=g.is_valid();p[1]=g.size();p[2]=g.thread_rank();}
