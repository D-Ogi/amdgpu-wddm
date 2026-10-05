// SPDX-License-Identifier: MIT
// Motion estimation, P pictures only. One thread group per macroblock, one candidate motion vector
// per thread, three integer stages followed by a half sample and a quarter sample refinement. The
// result is the only thing the macroblock pass needs from here: it is written into MbInfo.mvx/mvy.
//
// The subpel stages call the same InterpLuma as the reconstruction, so the vector chosen here is
// scored against exactly the prediction the decoder will build.
//
// A vector is snapped back to (0,0) when the zero vector costs no more than gSkipBias extra SAD. That
// is what makes P_Skip reachable in static areas without a second pass: the CPU side can then compare
// the vector against the clause 8.4.1.1 skip predictor, which is (0,0) wherever the neighbourhood is
// also still.

#include "h264_common.hlsli"

groupshared uint gSrcMb[256];
groupshared uint gCost[32];
groupshared uint gSad[32];
groupshared int  gCandX[32];
groupshared int  gCandY[32];
groupshared int  gBestX;
groupshared int  gBestY;
groupshared uint gBestSad;
groupshared uint gZeroSad;

uint SadInteger(int bx, int by, int imvx, int imvy)
{
    uint sad = 0;
    for (int y = 0; y < 16; ++y) {
        const int ry = by + y + imvy;
        for (int x = 0; x < 16; ++x) {
            sad += uint(abs(int(gSrcMb[y * 16 + x]) - int(RefY(bx + x + imvx, ry))));
        }
    }
    return sad;
}

uint SadSubpel(int bx, int by, int mvx, int mvy)
{
    uint sad = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            sad += uint(abs(int(gSrcMb[y * 16 + x]) -
                            int(InterpLuma(bx + x, by + y, mvx, mvy))));
        }
    }
    return sad;
}

// Reduces gCost over [0,count) and publishes the winner. Ties go to the smaller vector.
void Reduce(uint tid, uint count)
{
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        uint bestI = 0;
        for (uint i = 1; i < count; ++i) {
            const bool better = gCost[i] < gCost[bestI] ||
                (gCost[i] == gCost[bestI] &&
                 (abs(gCandX[i]) + abs(gCandY[i]) < abs(gCandX[bestI]) + abs(gCandY[bestI])));
            if (better) {
                bestI = i;
            }
        }
        gBestX = gCandX[bestI];
        gBestY = gCandY[bestI];
        gBestSad = gSad[bestI];
    }
    GroupMemoryBarrierWithGroupSync();
}

[numthreads(32, 1, 1)]
void CSMotionEstimate(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    const uint mbx = gid.x;
    const uint mby = gid.y;
    if (mbx >= gWidthMb || mby >= gHeightMb) {
        return;
    }
    const int bx = int(mbx * 16u);
    const int by = int(mby * 16u);

    // Source macroblock: eight consecutive samples of one row per thread.
    {
        const uint first = tid * 8u;
        const uint ly = first >> 4u;
        const uint lx = first & 15u;
        const uint addr = (uint(by) + ly) * gPadW + uint(bx) + lx;
        const uint w0 = bufSrcY.Load(addr);
        const uint w1 = bufSrcY.Load(addr + 4u);
        // Written out rather than looped: fxc 10.1 fails code generation ("internal error: unexpected
        // input register type", every optimisation level above /Od) for a loop that stores to a
        // groupshared array at a dynamic base index, even when the loop is fully unrollable. The
        // explicit form produces the same eight stores. Measured on this file, 2026-10-03.
        gSrcMb[first + 0u] = w0 & 0xFFu;
        gSrcMb[first + 1u] = (w0 >> 8u) & 0xFFu;
        gSrcMb[first + 2u] = (w0 >> 16u) & 0xFFu;
        gSrcMb[first + 3u] = (w0 >> 24u) & 0xFFu;
        gSrcMb[first + 4u] = w1 & 0xFFu;
        gSrcMb[first + 5u] = (w1 >> 8u) & 0xFFu;
        gSrcMb[first + 6u] = (w1 >> 16u) & 0xFFu;
        gSrcMb[first + 7u] = (w1 >> 24u) & 0xFFu;
    }
    GroupMemoryBarrierWithGroupSync();

    // Stage 1: 5x5 grid, step 8 full samples (reach +-16).
    int cx = 0, cy = 0;
    uint stage;
    for (stage = 0; stage < 3; ++stage) {
        const int step = (stage == 0) ? 8 : ((stage == 1) ? 2 : 1);
        const uint side = (stage == 2) ? 3u : 5u;
        const uint count = side * side;
        const int half = int(side >> 1u);
        if (tid < count) {
            const int dx = int(tid % side) - half;
            const int dy = int(tid / side) - half;
            const int ix = cx + dx * step;
            const int iy = cy + dy * step;
            const uint sad = SadInteger(bx, by, ix, iy);
            gSad[tid] = sad;
            gCandX[tid] = ix;
            gCandY[tid] = iy;
            gCost[tid] = sad + gLambda * uint(abs(ix) + abs(iy));
            if (stage == 0 && dx == 0 && dy == 0) {
                gZeroSad = sad;
            }
        } else {
            gSad[tid] = 0xFFFFFFFFu;
            gCost[tid] = 0xFFFFFFFFu;
            gCandX[tid] = 0;
            gCandY[tid] = 0;
        }
        Reduce(tid, count);
        cx = gBestX;
        cy = gBestY;
    }

    // Half sample then quarter sample refinement, in quarter sample units.
    int qx = cx * 4;
    int qy = cy * 4;
    for (stage = 0; stage < 2; ++stage) {
        const int step = (stage == 0) ? 2 : 1;
        if (tid < 9u) {
            const int dx = int(tid % 3u) - 1;
            const int dy = int(tid / 3u) - 1;
            const int mx = qx + dx * step;
            const int my = qy + dy * step;
            const uint sad = SadSubpel(bx, by, mx, my);
            gSad[tid] = sad;
            gCandX[tid] = mx;
            gCandY[tid] = my;
            gCost[tid] = sad + gLambda * uint(abs(mx) + abs(my)) / 4u;
        } else {
            gSad[tid] = 0xFFFFFFFFu;
            gCost[tid] = 0xFFFFFFFFu;
            gCandX[tid] = 0;
            gCandY[tid] = 0;
        }
        Reduce(tid, 9u);
        qx = gBestX;
        qy = gBestY;
    }

    if (tid == 0) {
        int fx = qx;
        int fy = qy;
        if ((fx != 0 || fy != 0) && gZeroSad <= gBestSad + gSkipBias) {
            fx = 0;
            fy = 0;
        }
        const uint mbIdx = mby * gWidthMb + mbx;
        const uint base = mbIdx * kMbInfoWords * 4u;
        rwMbInfo.Store(base + 3u * 4u, uint(fx));
        rwMbInfo.Store(base + 4u * 4u, uint(fy));
        rwMbInfo.Store(base + 12u * 4u, (fx == 0 && fy == 0) ? gZeroSad : gBestSad);
    }
}
