// SPDX-License-Identifier: MIT
// clprobe - is there an OpenCL platform on this computer, and does one of its GPU devices compute a known kernel
// exactly (BD-083)? The OpenCL loader (OpenCL.dll) is loaded at run time, so the probe also runs on a computer
// without one and says so. Each entry point gets the type of its declaration in the Khronos header, so a call with
// a wrong argument does not compile.
//
//   clprobe [--list] [--device <text>] [--loader <OpenCL.dll path>] [--size <n>] [--bound <s>]
//           [--inject skip-kernel]
//
// The kernel writes c[i] = a[i] * 3 + (b[i] ^ i) over n 32-bit words (default 1048576). The output buffer starts
// as the complement of the expected result, so a kernel that writes nothing cannot pass. --inject skip-kernel
// leaves out the dispatch: the run must end in result=mismatch, which proves the oracle.
// Output: "CLPROBE ..." lines on stdout, the last one
//   CLPROBE VERDICT result=pass|mismatch|fail|build-failed|no-opencl|no-platform|no-device|timeout stage=.. cl=.. detail=..
// Exit code 0 pass, 1 mismatch, 2 fail or build-failed, 3 no OpenCL loader, platform or device, 4 usage, 5 timeout.
#define CL_TARGET_OPENCL_VERSION 120
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <CL/cl.h>
#include <CL/cl_ext.h> // CL_PLATFORM_NOT_FOUND_KHR, the answer of the ICD loader when no vendor is registered
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FN(name) static __typeof__(&name) p_##name
FN(clGetPlatformIDs);
FN(clGetPlatformInfo);
FN(clGetDeviceIDs);
FN(clGetDeviceInfo);
FN(clCreateContext);
FN(clCreateCommandQueue);
FN(clCreateProgramWithSource);
FN(clBuildProgram);
FN(clGetProgramBuildInfo);
FN(clCreateKernel);
FN(clCreateBuffer);
FN(clSetKernelArg);
FN(clEnqueueNDRangeKernel);
FN(clEnqueueReadBuffer);
FN(clFinish);
FN(clReleaseMemObject);
FN(clReleaseKernel);
FN(clReleaseProgram);
FN(clReleaseCommandQueue);
FN(clReleaseContext);

static const char *volatile g_stage = "start";
static unsigned g_bound = 60;

static int Verdict(const char *result, int code, cl_int cl, const char *detail)
{
    printf("CLPROBE VERDICT result=%s stage=%s cl=%d detail=%s\n", result, g_stage, (int)cl, detail ? detail : "-");
    fflush(stdout);
    return code;
}

// A loader or a driver that hangs inside a call cannot hold the run past --bound.
static DWORD WINAPI Watchdog(LPVOID unused)
{
    (void)unused;
    Sleep(g_bound * 1000u);
    printf("CLPROBE VERDICT result=timeout stage=%s cl=0 detail=no-result-within-%us\n", g_stage, g_bound);
    fflush(stdout);
    ExitProcess(5);
}

static const char Source[] =
    "__kernel void probe(__global const uint *a, __global const uint *b, __global uint *c)\n"
    "{\n"
    "    const uint i = (uint)get_global_id(0);\n"
    "    c[i] = a[i] * 3u + (b[i] ^ i);\n"
    "}\n";

static void PlatformText(cl_platform_id p, cl_platform_info what, char *buf, size_t n)
{
    buf[0] = 0;
    if (p_clGetPlatformInfo(p, what, n - 1, buf, NULL) != CL_SUCCESS) strcpy_s(buf, n, "?");
    buf[n - 1] = 0;
}

static void DeviceText(cl_device_id d, cl_device_info what, char *buf, size_t n)
{
    buf[0] = 0;
    if (p_clGetDeviceInfo(d, what, n - 1, buf, NULL) != CL_SUCCESS) strcpy_s(buf, n, "?");
    buf[n - 1] = 0;
}

static int Usage(const char *why)
{
    printf("CLPROBE USAGE %s\n"
           "clprobe [--list] [--device <text>] [--loader <OpenCL.dll path>] [--size <n>] [--bound <s>]"
           " [--inject skip-kernel]\n",
           why);
    return 4;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *loader = NULL;
    char want[256] = "";
    int listOnly = 0, skipKernel = 0;
    unsigned n = 1u << 20;
    for (int i = 1; i < argc; ++i) {
        const wchar_t *a = argv[i];
        const wchar_t *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!wcscmp(a, L"--list")) {
            listOnly = 1;
        } else if (!wcscmp(a, L"--device") && v) {
            if (WideCharToMultiByte(CP_UTF8, 0, v, -1, want, (int)sizeof(want), NULL, NULL) <= 0)
                return Usage("--device text too long");
            ++i;
        } else if (!wcscmp(a, L"--loader") && v) {
            loader = v;
            ++i;
        } else if (!wcscmp(a, L"--size") && v) {
            n = (unsigned)wcstoul(v, NULL, 10);
            if (n < 1024 || n > (1u << 24)) return Usage("--size must be 1024..16777216");
            ++i;
        } else if (!wcscmp(a, L"--bound") && v) {
            g_bound = (unsigned)wcstoul(v, NULL, 10);
            if (g_bound < 1 || g_bound > 170) return Usage("--bound must be 1..170");
            ++i;
        } else if (!wcscmp(a, L"--inject") && v && !wcscmp(v, L"skip-kernel")) {
            skipKernel = 1;
            ++i;
        } else {
            return Usage("unknown option or missing value");
        }
    }
    HANDLE dog = CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
    if (dog) CloseHandle(dog);
    printf("CLPROBE start size=%u bound=%u list=%d inject=%s device=%s\n", n, g_bound, listOnly,
           skipKernel ? "skip-kernel" : "-", want[0] ? want : "-");

    g_stage = "loader";
    // System32 first, where a driver's or a system loader is; then the normal search order, which includes the
    // folder of this program, for an application-local loader.
    HMODULE lib = loader ? LoadLibraryW(loader) : LoadLibraryExW(L"OpenCL.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib && !loader) lib = LoadLibraryW(L"OpenCL.dll");
    if (!lib) {
        char d[64];
        sprintf_s(d, sizeof(d), "OpenCL.dll-not-loaded-error-%lu", GetLastError());
        return Verdict("no-opencl", 3, 0, d);
    }
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(lib, path, MAX_PATH);
    printf("CLPROBE loader %ls\n", path);
#define LOAD(name)                                                                                                    \
    if (!(p_##name = (__typeof__(p_##name))(void *)GetProcAddress(lib, #name))) return Verdict("fail", 2, 0, "missing-" #name)
    LOAD(clGetPlatformIDs);
    LOAD(clGetPlatformInfo);
    LOAD(clGetDeviceIDs);
    LOAD(clGetDeviceInfo);
    LOAD(clCreateContext);
    LOAD(clCreateCommandQueue);
    LOAD(clCreateProgramWithSource);
    LOAD(clBuildProgram);
    LOAD(clGetProgramBuildInfo);
    LOAD(clCreateKernel);
    LOAD(clCreateBuffer);
    LOAD(clSetKernelArg);
    LOAD(clEnqueueNDRangeKernel);
    LOAD(clEnqueueReadBuffer);
    LOAD(clFinish);
    LOAD(clReleaseMemObject);
    LOAD(clReleaseKernel);
    LOAD(clReleaseProgram);
    LOAD(clReleaseCommandQueue);
    LOAD(clReleaseContext);

    g_stage = "platforms";
    cl_uint np = 0;
    cl_int e = p_clGetPlatformIDs(0, NULL, &np);
    if (e == CL_PLATFORM_NOT_FOUND_KHR || (e == CL_SUCCESS && np == 0))
        return Verdict("no-platform", 3, e, "the-loader-found-no-platform");
    if (e != CL_SUCCESS) return Verdict("fail", 2, e, "clGetPlatformIDs");
    cl_platform_id plats[16];
    if (np > 16) np = 16;
    e = p_clGetPlatformIDs(np, plats, NULL);
    if (e != CL_SUCCESS) return Verdict("fail", 2, e, "clGetPlatformIDs");
    cl_device_id dev = NULL;
    char text[512], name[256] = "";
    for (cl_uint p = 0; p < np; ++p) {
        char pname[256], vendor[256], version[256];
        PlatformText(plats[p], CL_PLATFORM_NAME, pname, sizeof(pname));
        PlatformText(plats[p], CL_PLATFORM_VENDOR, vendor, sizeof(vendor));
        PlatformText(plats[p], CL_PLATFORM_VERSION, version, sizeof(version));
        printf("CLPROBE platform %u name=\"%s\" vendor=\"%s\" version=\"%s\"\n", p, pname, vendor, version);
        cl_uint nd = 0;
        e = p_clGetDeviceIDs(plats[p], CL_DEVICE_TYPE_ALL, 0, NULL, &nd);
        if (e == CL_DEVICE_NOT_FOUND || nd == 0) {
            printf("CLPROBE platform %u devices=0 cl=%d\n", p, (int)e);
            continue;
        }
        if (e != CL_SUCCESS) return Verdict("fail", 2, e, "clGetDeviceIDs");
        cl_device_id devs[16];
        if (nd > 16) nd = 16;
        e = p_clGetDeviceIDs(plats[p], CL_DEVICE_TYPE_ALL, nd, devs, NULL);
        if (e != CL_SUCCESS) return Verdict("fail", 2, e, "clGetDeviceIDs");
        for (cl_uint d = 0; d < nd; ++d) {
            cl_device_type type = 0;
            cl_uint units = 0;
            cl_ulong mem = 0;
            char dname[256], dversion[256], driver[256];
            p_clGetDeviceInfo(devs[d], CL_DEVICE_TYPE, sizeof(type), &type, NULL);
            p_clGetDeviceInfo(devs[d], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(units), &units, NULL);
            p_clGetDeviceInfo(devs[d], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(mem), &mem, NULL);
            DeviceText(devs[d], CL_DEVICE_NAME, dname, sizeof(dname));
            DeviceText(devs[d], CL_DEVICE_VERSION, dversion, sizeof(dversion));
            DeviceText(devs[d], CL_DRIVER_VERSION, driver, sizeof(driver));
            printf("CLPROBE device %u.%u name=\"%s\" type=0x%llx units=%u global_mib=%llu version=\"%s\" driver=\"%s\"\n", p,
                   d, dname, (unsigned long long)type, units, (unsigned long long)(mem >> 20), dversion, driver);
            if (!dev && (type & CL_DEVICE_TYPE_GPU) && (!want[0] || strstr(dname, want))) {
                dev = devs[d];
                strcpy_s(name, sizeof(name), dname);
            }
        }
    }
    if (!dev) return Verdict("no-device", 3, 0, want[0] ? "no-GPU-device-with-that-name" : "no-GPU-device");
    printf("CLPROBE chosen \"%s\"\n", name);
    if (listOnly) return Verdict("pass", 0, 0, "list");

    g_stage = "context";
    cl_context ctx = p_clCreateContext(NULL, 1, &dev, NULL, NULL, &e);
    if (!ctx || e != CL_SUCCESS) return Verdict("fail", 2, e, "clCreateContext");
    g_stage = "queue";
    cl_command_queue q = p_clCreateCommandQueue(ctx, dev, 0, &e);
    if (!q || e != CL_SUCCESS) return Verdict("fail", 2, e, "clCreateCommandQueue");
    g_stage = "build";
    const char *src = Source;
    cl_program prog = p_clCreateProgramWithSource(ctx, 1, &src, NULL, &e);
    if (!prog || e != CL_SUCCESS) return Verdict("fail", 2, e, "clCreateProgramWithSource");
    ULONGLONG t0 = GetTickCount64();
    e = p_clBuildProgram(prog, 1, &dev, "", NULL, NULL);
    if (e != CL_SUCCESS) {
        text[0] = 0;
        p_clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof(text) - 1, text, NULL);
        text[sizeof(text) - 1] = 0;
        printf("CLPROBE build-log %s\n", text);
        return Verdict("build-failed", 2, e, "clBuildProgram");
    }
    printf("CLPROBE built in %llu ms\n", (unsigned long long)(GetTickCount64() - t0));
    g_stage = "kernel";
    cl_kernel k = p_clCreateKernel(prog, "probe", &e);
    if (!k || e != CL_SUCCESS) return Verdict("fail", 2, e, "clCreateKernel");

    g_stage = "host-data";
    uint32_t *a = (uint32_t *)malloc((size_t)n * 4), *b = (uint32_t *)malloc((size_t)n * 4);
    uint32_t *expect = (uint32_t *)malloc((size_t)n * 4), *out = (uint32_t *)malloc((size_t)n * 4);
    if (!a || !b || !expect || !out) return Verdict("fail", 2, 0, "host-allocation");
    uint32_t x = 0x2545F491u;
    for (unsigned i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        a[i] = x;
        x = x * 1664525u + 1013904223u;
        b[i] = x;
        expect[i] = a[i] * 3u + (b[i] ^ (uint32_t)i);
        out[i] = ~expect[i];
    }
    g_stage = "buffers";
    const size_t bytes = (size_t)n * 4;
    cl_mem ba = p_clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes, a, &e);
    if (!ba || e != CL_SUCCESS) return Verdict("fail", 2, e, "clCreateBuffer-a");
    cl_mem bb = p_clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes, b, &e);
    if (!bb || e != CL_SUCCESS) return Verdict("fail", 2, e, "clCreateBuffer-b");
    cl_mem bc = p_clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, bytes, out, &e);
    if (!bc || e != CL_SUCCESS) return Verdict("fail", 2, e, "clCreateBuffer-c");
    g_stage = "arguments";
    if ((e = p_clSetKernelArg(k, 0, sizeof(cl_mem), &ba)) != CL_SUCCESS ||
        (e = p_clSetKernelArg(k, 1, sizeof(cl_mem), &bb)) != CL_SUCCESS ||
        (e = p_clSetKernelArg(k, 2, sizeof(cl_mem), &bc)) != CL_SUCCESS)
        return Verdict("fail", 2, e, "clSetKernelArg");
    g_stage = "dispatch";
    t0 = GetTickCount64();
    const size_t global = n;
    if (!skipKernel) {
        e = p_clEnqueueNDRangeKernel(q, k, 1, NULL, &global, NULL, 0, NULL, NULL);
        if (e != CL_SUCCESS) return Verdict("fail", 2, e, "clEnqueueNDRangeKernel");
    }
    g_stage = "read";
    memset(out, 0, bytes);
    e = p_clEnqueueReadBuffer(q, bc, CL_TRUE, 0, bytes, out, 0, NULL, NULL);
    if (e != CL_SUCCESS) return Verdict("fail", 2, e, "clEnqueueReadBuffer");
    e = p_clFinish(q);
    if (e != CL_SUCCESS) return Verdict("fail", 2, e, "clFinish");
    const unsigned long long ms = GetTickCount64() - t0;

    g_stage = "compare";
    unsigned bad = 0, untouched = 0, first = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (out[i] == expect[i]) continue;
        if (!bad) first = i;
        ++bad;
        untouched += out[i] == ~expect[i];
    }
    printf("CLPROBE result words=%u mismatched=%u untouched=%u dispatch_read_ms=%llu\n", n, bad, untouched, ms);
    if (bad)
        printf("CLPROBE first-mismatch index=%u got=0x%08x want=0x%08x\n", first, out[first], expect[first]);
    p_clReleaseMemObject(bc);
    p_clReleaseMemObject(bb);
    p_clReleaseMemObject(ba);
    p_clReleaseKernel(k);
    p_clReleaseProgram(prog);
    p_clReleaseCommandQueue(q);
    p_clReleaseContext(ctx);
    free(a);
    free(b);
    free(expect);
    free(out);
    sprintf_s(text, sizeof(text), "device=%s;words=%u;mismatched=%u", name, n, bad);
    for (char *c = text; *c; ++c)
        if (*c == ' ') *c = '_';
    return bad ? Verdict("mismatch", 1, 0, text) : Verdict("pass", 0, 0, text);
}
