// HIP launch-geometry regression. The output address uses explicit launch bounds;
// broken gridDim/blockDim values are data to check, never unbounded write indices.
// HIP <<<grid,block>>> launches uniform full blocks. A partial logical tail is
// an application data extent: the last block's inactive threads still exist and
// retain blockDim/gridDim. This test does not invent nonuniform physical groups.
#if defined(BC250_GEOMETRY_HOST_ONLY)
struct dim3 {
    unsigned x, y, z;
    dim3(unsigned a = 1, unsigned b = 1, unsigned c = 1) : x(a), y(b), z(c) {}
};
#else
#include <hip/hip_runtime.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned GuardWords = 64;
static const unsigned Canary = 0xa5b6c7d8u;
static const unsigned Written = 0x706f7365u;
static const unsigned ExpectedWave = 32; // checked against compiled gfx1013 metadata by the builder
struct Record {
    unsigned written, grid[3], block[3], group[3], thread[3], linear, wave, logical;
};
static_assert(sizeof(Record) == 64, "fixed geometry observation record");

#if !defined(BC250_GEOMETRY_HOST_ONLY)
__global__ void geometry(Record* output, unsigned count,
                         unsigned gx, unsigned gy, unsigned gz,
                         unsigned bx, unsigned by, unsigned bz,
                         unsigned lx, unsigned ly, unsigned lz) {
    const unsigned x = blockIdx.x, y = blockIdx.y, z = blockIdx.z;
    const unsigned tx = threadIdx.x, ty = threadIdx.y, tz = threadIdx.z;
    if (x >= gx || y >= gy || z >= gz || tx >= bx || ty >= by || tz >= bz) return;
    // Independent bounds are small explicit kernel arguments from the host.
    const unsigned index = (((((z * gy + y) * gx + x) * bz + tz) * by + ty) * bx + tx);
    if (index >= count) return;
    Record* r = output + index;
    r->written = Written;
    r->grid[0] = gridDim.x; r->grid[1] = gridDim.y; r->grid[2] = gridDim.z;
    r->block[0] = blockDim.x; r->block[1] = blockDim.y; r->block[2] = blockDim.z;
    r->group[0] = x; r->group[1] = y; r->group[2] = z;
    r->thread[0] = tx; r->thread[1] = ty; r->thread[2] = tz;
    r->wave = warpSize;
    // A wrong builtin-derived index is retained for diagnosis, never dereferenced.
    r->linear = (((((z * (unsigned)gridDim.y + y) * (unsigned)gridDim.x + x) *
                  (unsigned)blockDim.z + tz) * (unsigned)blockDim.y + ty) *
                  (unsigned)blockDim.x + tx);
    // Real threads beyond the application's logical extent do no data write.
    // Every thread still writes the bounded geometry record above, so the host
    // can distinguish a correct inactive tail from work that never executed.
    if (x * (unsigned)blockDim.x + tx < lx && y * (unsigned)blockDim.y + ty < ly &&
        z * (unsigned)blockDim.z + tz < lz)
        r->logical = index ^ Written;
}
#endif

static unsigned count_of(dim3 grid, dim3 block) {
    return grid.x * grid.y * grid.z * block.x * block.y * block.z;
}
static size_t words_of(unsigned count) {
    return 2 * GuardWords + (size_t)count * sizeof(Record) / sizeof(unsigned);
}
static void initialize(unsigned* output, size_t words) {
    for (size_t i = 0; i < words; ++i) output[i] = Canary;
}

static unsigned check_output(const unsigned* output, dim3 grid, dim3 block, dim3 logical, int verbose) {
    const unsigned count = count_of(grid, block);
    const size_t words = words_of(count);
    const Record* records = (const Record*)(output + GuardWords);
    unsigned failures = 0, index = 0, active = 0, canary_failures = 0;
    for (unsigned i = 0; i < GuardWords; ++i) {
        if (output[i] != Canary || output[words - GuardWords + i] != Canary) ++canary_failures;
    }
    // Host oracle advances an ordinal through nested loops; it does not reuse
    // the device's address formula or any observed dimension for loop bounds.
    for (unsigned z = 0; z < grid.z; ++z)
    for (unsigned y = 0; y < grid.y; ++y)
    for (unsigned x = 0; x < grid.x; ++x)
    for (unsigned tz = 0; tz < block.z; ++tz)
    for (unsigned ty = 0; ty < block.y; ++ty)
    for (unsigned tx = 0; tx < block.x; ++tx, ++index) {
        const Record& r = records[index];
        const bool live = x * block.x + tx < logical.x && y * block.y + ty < logical.y &&
                          z * block.z + tz < logical.z;
        active += live ? 1u : 0u;
        if (r.written != Written || r.linear != index || r.wave != ExpectedWave ||
            r.logical != (live ? (index ^ Written) : Canary) ||
            r.grid[0] != grid.x || r.grid[1] != grid.y || r.grid[2] != grid.z ||
            r.block[0] != block.x || r.block[1] != block.y || r.block[2] != block.z ||
            r.group[0] != x || r.group[1] != y || r.group[2] != z ||
            r.thread[0] != tx || r.thread[1] != ty || r.thread[2] != tz) {
            if (verbose && failures < 4)
                printf("geometry: index %u got grid=%u,%u,%u block=%u,%u,%u group=%u,%u,%u "
                       "thread=%u,%u,%u linear=%u warp=%u logical=%08x stamp=%08x\n", index,
                       r.grid[0], r.grid[1], r.grid[2], r.block[0], r.block[1], r.block[2],
                       r.group[0], r.group[1], r.group[2], r.thread[0], r.thread[1], r.thread[2],
                       r.linear, r.wave, r.logical, r.written);
            ++failures;
        }
    }
    if (verbose)
        printf("geometry: grid=%u,%u,%u block=%u,%u,%u logical=%u,%u,%u records=%u "
               "active=%u tail=%u warp=%u mismatches=%u canaries=%u\n",
               grid.x, grid.y, grid.z, block.x, block.y, block.z, logical.x, logical.y, logical.z,
               count, active, count - active, ExpectedWave, failures, canary_failures);
    return failures + canary_failures;
}

#if defined(BC250_GEOMETRY_HOST_ONLY)
int main(void) {
    dim3 grid(3, 5, 7), block(8, 4, 2), logical(23, 18, 13);
    const unsigned count = count_of(grid, block);
    unsigned* output = (unsigned*)malloc(words_of(count) * sizeof(unsigned));
    if (!output) return 2;
    initialize(output, words_of(count));
    Record* records = (Record*)(output + GuardWords);
    for (unsigned index = 0; index < count; ++index) {
        unsigned q = index;
        Record& r = records[index];
        r.written = Written; r.linear = index;
        r.grid[0] = 3; r.grid[1] = 5; r.grid[2] = 7;
        r.block[0] = 8; r.block[1] = 4; r.block[2] = 2;
        r.thread[0] = q % 8; q /= 8;
        r.thread[1] = q % 4; q /= 4;
        r.thread[2] = q % 2; q /= 2;
        r.group[0] = q % 3; q /= 3;
        r.group[1] = q % 5; q /= 5;
        r.group[2] = q;
        r.wave = ExpectedWave;
        const bool live = r.group[0] * 8 + r.thread[0] < 23 &&
                          r.group[1] * 4 + r.thread[1] < 18 &&
                          r.group[2] * 2 + r.thread[2] < 13;
        r.logical = live ? (index ^ Written) : Canary;
    }
    unsigned failed = check_output(output, grid, block, logical, 0) != 0;
    unsigned controls = 1;
    for (unsigned axis = 0; axis < 3; ++axis) {
        records[17].grid[axis] *= records[17].block[axis];
        failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
        records[17].grid[axis] /= records[17].block[axis];
        ++records[17].block[axis];
        failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
        --records[17].block[axis];
        ++records[17].thread[axis];
        failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
        --records[17].thread[axis];
        ++records[17].group[axis];
        failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
        --records[17].group[axis];
    }
    ++records[17].linear;
    failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
    --records[17].linear;
    records[17].written = Canary;
    failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
    records[17].written = Written;
    output[0] ^= 1;
    failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
    output[0] ^= 1;
    output[words_of(count) - 1] ^= 1;
    failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
    output[words_of(count) - 1] ^= 1;
    ++records[17].wave;
    failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
    --records[17].wave;
    records[17].logical ^= 1;
    failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
    records[17].logical ^= 1;
    records[count - 1].logical ^= 1; // a logical-tail write must also be detected
    failed += check_output(output, grid, block, logical, 0) == 0; ++controls;
    records[count - 1].logical ^= 1;
    failed += check_output(output, grid, block, logical, 0) != 0; ++controls;
    free(output);
    printf("geometry oracle: %u controls, %u failures\n", controls, failed);
    return failed ? 1 : 0;
}
#else
static int hip_ok(hipError_t result, const char* call) {
    if (result == hipSuccess) return 1;
    printf("geometry: %s: %s (%d)\n", call, hipGetErrorName(result), (int)result);
    return 0;
}

static int run_case(dim3 grid, dim3 block, dim3 logical) {
    const unsigned count = count_of(grid, block);
    const size_t words = words_of(count), bytes = words * sizeof(unsigned);
    unsigned* output = (unsigned*)malloc(bytes);
    unsigned* device = NULL;
    int failure = 1;
    if (!output) return 1;
    initialize(output, words);
    if (!hip_ok(hipMalloc((void**)&device, bytes), "hipMalloc")) goto done;
    if (!hip_ok(hipMemcpy(device, output, bytes, hipMemcpyHostToDevice), "upload")) goto done;
    (void)hipGetLastError();
    geometry<<<grid, block, 0, nullptr>>>((Record*)(device + GuardWords), count,
                              grid.x, grid.y, grid.z, block.x, block.y, block.z,
                              logical.x, logical.y, logical.z);
    if (!hip_ok(hipGetLastError(), "launch")) goto done;
    if (!hip_ok(hipDeviceSynchronize(), "synchronize")) goto done;
    if (!hip_ok(hipMemcpy(output, device, bytes, hipMemcpyDeviceToHost), "readback")) goto done;
    failure = check_output(output, grid, block, logical, 1) ? 1 : 0;
done:
    if (device && !hip_ok(hipFree(device), "hipFree")) failure = 1;
    free(output);
    return failure;
}

int main(void) {
    // The operator also bounds the complete process externally. A host wait
    // deadline alone cannot terminate a GPU that has already stopped responding.
    if (_putenv_s("BC250_HIP_WAIT_TOTAL_MS", "10000") != 0) return 2;
    int failures = 0;
    failures += run_case(dim3(5, 1, 1), dim3(8, 1, 1), dim3(29, 1, 1));
    failures += run_case(dim3(5, 4, 1), dim3(4, 3, 1), dim3(17, 9, 1));
    failures += run_case(dim3(3, 5, 7), dim3(8, 4, 2), dim3(23, 18, 13));
    failures += run_case(dim3(7, 3, 5), dim3(3, 5, 2), dim3(21, 15, 10));
    failures += run_case(dim3(5, 2, 3), dim3(7, 3, 2), dim3(35, 6, 6));
    printf("geometry: %s (%d failed cases)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
#endif
