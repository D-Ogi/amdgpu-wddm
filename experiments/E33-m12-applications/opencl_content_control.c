/* BC-250 M12 deterministic OpenCL content control. */
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 4096u
#define GROUP 64u
#define CHECK(call) do { cl_int e_ = (call); if (e_ != CL_SUCCESS) { \
    fprintf(stderr, "%s: OpenCL error %d\n", #call, e_); exit(2); } } while (0)

static uint32_t transform(uint32_t x, uint32_t i) {
    x = x * UINT32_C(1664525) + UINT32_C(1013904223) + i;
    return ((x << 7) | (x >> 25)) ^ UINT32_C(0xa5c31f09);
}

static const char *source =
"__kernel void map_values(__global const uint *a, __global uint *b) {\n"
"  uint i = get_global_id(0);\n"
"  uint x = a[i] * 1664525u + 1013904223u + i;\n"
"  b[i] = rotate(x, 7u) ^ 0xa5c31f09u;\n"
"}\n"
"__kernel void reduce_values(__global const uint *b, __global uint *c,\n"
"                            __local uint *s) {\n"
"  uint l = get_local_id(0);\n"
"  s[l] = b[get_global_id(0)];\n"
"  barrier(CLK_LOCAL_MEM_FENCE);\n"
"  for (uint d = get_local_size(0) / 2; d; d >>= 1) {\n"
"    if (l < d) s[l] += s[l + d];\n"
"    barrier(CLK_LOCAL_MEM_FENCE);\n"
"  }\n"
"  if (!l) c[get_group_id(0)] = s[0];\n"
"}\n";

int main(int argc, char **argv) {
    cl_uint np = 0, nd = 0;
    cl_platform_id platforms[16];
    cl_device_id selected = NULL;
    unsigned matches = 0;
    cl_int error;
    char name[1024], version[1024];
    uint32_t input[N], output[N], sums[N / GROUP];
    size_t global = N, local = GROUP;
    int profiling = argc == 3 && strcmp(argv[2], "--profile") == 0;
    cl_event events[2] = {NULL, NULL};
    if ((argc != 2 && !profiling) || !argv[1][0]) {
        fprintf(stderr, "usage: opencl_content_control <expected GPU name substring> [--profile]\n");
        return 2;
    }
    CHECK(clGetPlatformIDs(0, NULL, &np));
    if (np > 16) return 2;
    CHECK(clGetPlatformIDs(np, platforms, NULL));
    for (cl_uint p = 0; p < np; ++p) {
        cl_device_id devices[16];
        error = clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 0, NULL, &nd);
        if (error == CL_DEVICE_NOT_FOUND) continue;
        CHECK(error);
        if (nd > 16) return 2;
        CHECK(clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, nd, devices, NULL));
        for (cl_uint d = 0; d < nd; ++d) {
            CHECK(clGetDeviceInfo(devices[d], CL_DEVICE_NAME, sizeof(name), name, NULL));
            if (strstr(name, argv[1])) { selected = devices[d]; ++matches; }
        }
    }
    if (matches != 1) {
        fprintf(stderr, "Expected exactly one matching GPU, found %u\n", matches);
        return 2;
    }
    CHECK(clGetDeviceInfo(selected, CL_DEVICE_NAME, sizeof(name), name, NULL));
    CHECK(clGetDeviceInfo(selected, CL_DEVICE_VERSION, sizeof(version), version, NULL));
    printf("device=%s\nversion=%s\n", name, version);
    CHECK(clGetDeviceInfo(selected, CL_DRIVER_VERSION, sizeof(version), version, NULL));
    printf("driver=%s\n", version);
    fflush(stdout);
    cl_context context = clCreateContext(NULL, 1, &selected, NULL, NULL, &error);
    CHECK(error);
    cl_command_queue queue = clCreateCommandQueue(context, selected, profiling ? CL_QUEUE_PROFILING_ENABLE : 0, &error);
    CHECK(error);
    cl_program program = clCreateProgramWithSource(context, 1, &source, NULL, &error);
    CHECK(error);
    error = clBuildProgram(program, 1, &selected, "-cl-std=CL1.2", NULL, NULL);
    if (error != CL_SUCCESS) {
        size_t size = 0;
        CHECK(clGetProgramBuildInfo(program, selected, CL_PROGRAM_BUILD_LOG, 0, NULL, &size));
        char *log = (char *)calloc(size + 1, 1);
        if (!log) return 2;
        CHECK(clGetProgramBuildInfo(program, selected, CL_PROGRAM_BUILD_LOG, size, log, NULL));
        fprintf(stderr, "%s\n", log);
        free(log);
        CHECK(error);
    }
    cl_kernel map = clCreateKernel(program, "map_values", &error); CHECK(error);
    cl_kernel reduce = clCreateKernel(program, "reduce_values", &error); CHECK(error);
    for (uint32_t i = 0; i < N; ++i) input[i] = i * UINT32_C(2654435761) ^ (i >> 3);
    cl_mem a = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                             sizeof(input), input, &error); CHECK(error);
    cl_mem b = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(output), NULL, &error); CHECK(error);
    cl_mem c = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(sums), NULL, &error); CHECK(error);
    CHECK(clSetKernelArg(map, 0, sizeof(a), &a));
    CHECK(clSetKernelArg(map, 1, sizeof(b), &b));
    CHECK(clSetKernelArg(reduce, 0, sizeof(b), &b));
    CHECK(clSetKernelArg(reduce, 1, sizeof(c), &c));
    CHECK(clSetKernelArg(reduce, 2, GROUP * sizeof(uint32_t), NULL));
    CHECK(clEnqueueNDRangeKernel(queue, map, 1, NULL, &global, &local, 0, NULL, profiling ? &events[0] : NULL));
    CHECK(clEnqueueNDRangeKernel(queue, reduce, 1, NULL, &global, &local, 0, NULL, profiling ? &events[1] : NULL));
    CHECK(clEnqueueReadBuffer(queue, b, CL_TRUE, 0, sizeof(output), output, 0, NULL, NULL));
    CHECK(clEnqueueReadBuffer(queue, c, CL_TRUE, 0, sizeof(sums), sums, 0, NULL, NULL));
    unsigned failures = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t expected = transform(input[i], i);
        if (output[i] != expected) {
            if (failures < 8) fprintf(stderr, "map[%u]: %08x expected %08x\n", i, output[i], expected);
            ++failures;
        }
    }
    for (uint32_t g = 0; g < N / GROUP; ++g) {
        uint32_t expected = 0;
        for (uint32_t j = 0; j < GROUP; ++j) expected += transform(input[g * GROUP + j], g * GROUP + j);
        if (sums[g] != expected) {
            if (failures < 8) fprintf(stderr, "sum[%u]: %08x expected %08x\n", g, sums[g], expected);
            ++failures;
        }
    }
    if (profiling) {
        const cl_profiling_info fields[4] = {CL_PROFILING_COMMAND_QUEUED,
            CL_PROFILING_COMMAND_SUBMIT, CL_PROFILING_COMMAND_START, CL_PROFILING_COMMAND_END};
        cl_ulong stamps[2][4];
        for (unsigned event = 0; event < 2; ++event) {
            for (unsigned field = 0; field < 4; ++field)
                CHECK(clGetEventProfilingInfo(events[event], fields[field],
                                             sizeof(cl_ulong), &stamps[event][field], NULL));
            printf("profile[%u] queued=%llu submit=%llu start=%llu end=%llu ns\n", event,
                   (unsigned long long)stamps[event][0], (unsigned long long)stamps[event][1],
                   (unsigned long long)stamps[event][2], (unsigned long long)stamps[event][3]);
            for (unsigned field = 1; field < 4; ++field) {
                if (stamps[event][field] < stamps[event][field - 1]) {
                    fprintf(stderr, "Invalid profiling order in event %u\n", event);
                    ++failures;
                }
            }
            CHECK(clReleaseEvent(events[event]));
        }
        if (stamps[1][2] < stamps[0][3]) {
            fprintf(stderr, "Dependent reduction starts before map ends\n");
            ++failures;
        }
    }
    CHECK(clReleaseMemObject(c)); CHECK(clReleaseMemObject(b)); CHECK(clReleaseMemObject(a));
    CHECK(clReleaseKernel(reduce)); CHECK(clReleaseKernel(map));
    CHECK(clReleaseProgram(program)); CHECK(clReleaseCommandQueue(queue)); CHECK(clReleaseContext(context));
    printf("%s: %u map words, %u group sums, %u mismatches\n", failures ? "FAIL" : "PASS", N, N / GROUP, failures);
    return failures ? 1 : 0;
}
