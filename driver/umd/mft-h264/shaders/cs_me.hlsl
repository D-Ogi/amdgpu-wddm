// SPDX-License-Identifier: MIT
// Motion estimation, P pictures only. One thread group per macroblock, three integer stages followed
// by a half sample and a quarter sample refinement. The result is the only thing the macroblock pass
// needs from here: it is written into MbInfo.mvx/mvy.
//
// The subpel stages score against exactly the prediction the decoder will build, as InterpLuma in
// h264_common.hlsli does, but they read the interpolated samples out of groupshared windows instead
// of recomputing the six-tap filters for every candidate. The windows hold the same integers
// InterpLuma computes, so the chosen vector and every byte downstream of it are unchanged; see
// CacheSubpelWindow for the derivation of their extent and for why the horizontal intermediates are
// kept unclipped.
//
// Before the windows existed, the two refinement stages were 70 % of a 1080p P picture's GPU time
// (5.237 ms of 7.453 on the development PC) because nine candidates of 256 samples each called
// InterpLuma per sample, and a centre sample costs 36 clamped loads and seven six-tap filters. Nine
// lanes of thirty-two also did all of that work. Now every lane sums eight samples of every
// candidate and nine lanes reduce the thirty-two partial sums.
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

// The sub-pel reference windows, all centred on the integer winner (bx+cx, by+cy).
//   gInt   [y+3][x+3]    integer samples, x and y in [-3,19]
//   gBRaw  [y+3][xi+1]   the horizontal six-tap, unclipped, xi in [-1,16], y in [-3,19]
//   gH     [yi+1][xi+1]  the half sample h, clipped, xi and yi in [-1,16]
//   gJ     [yi+1][xi+1]  the centre sample j, clipped, xi and yi in [-1,16]
static const uint kWinInt = 23u;    // integer samples per row of gInt
static const uint kWinHalf = 18u;   // half sample positions per row of gBRaw, gH and gJ
groupshared int gInt[23 * 23];
groupshared int gBRaw[23 * 18];
groupshared int gH[18 * 18];
groupshared int gJ[18 * 18];
// One partial sum per candidate per lane: nine candidates, thirty-two lanes, eight samples each.
groupshared uint gPart[9 * 32];

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

// Fills the four windows above. One group, thirty-two threads, three barriers.
//
// Extent, which is the part that must not be guessed. A candidate of the two refinement stages is
// (cx*4 + ex, cy*4 + ey) with ex and ey in [-3,3]: the half sample stage moves cx*4 by -2, 0 or +2,
// and the quarter sample stage by one more step either way. For a sample at (bx+x, by+y) with x and
// y in [0,15] the integer position is xi = bx + x + (mvx >> 2), which is bx+cx+x for ex >= 0 and one
// less for ex < 0, so xi - (bx+cx) lies in [-1,15]; the xf == 3 case reads the neighbour one column
// further, which makes it [-1,16]; and the six-tap of clause 8.4.2.2.1 reaches xi-2 to xi+3. So the
// integer samples needed are [-3,19] in both axes, 23 by 23, and the half sample positions needed
// are [-1,16] in both axes, 18 by 18.
//
// gInt holds what RefY returns, clamp included, so a window that hangs over the edge of the coded
// picture carries the replicated samples clause 8.4.2.2.1 prescribes and every six-tap over it gives
// the same integer the per-sample form gives.
void CacheSubpelWindow(uint tid, int bx, int by, int cx, int cy)
{
    const int ox0 = bx + cx - 3;
    const int oy0 = by + cy - 3;
    uint i;
    for (i = tid; i < kWinInt * kWinInt; i += 32u) {
        const uint yy = i / kWinInt;
        const uint xx = i - yy * kWinInt;
        gInt[i] = int(RefY(ox0 + int(xx), oy0 + int(yy)));
    }
    GroupMemoryBarrierWithGroupSync();
    // gBRaw is the horizontal six-tap before any rounding or clipping. The centre sample j is the
    // vertical six-tap of exactly these unclipped intermediates (J1 in h264_common.hlsli), while the
    // half sample b is one rounding and one clip of the same value. Caching a clipped b here instead
    // would change every j that sits next to a saturating edge, so b is clipped on read and this
    // plane stays raw.
    for (i = tid; i < kWinInt * kWinHalf; i += 32u) {
        const uint yy = i / kWinHalf;
        const uint xc = i - yy * kWinHalf;
        const uint r = yy * kWinInt + xc;
        gBRaw[i] = Tap6(gInt[r], gInt[r + 1u], gInt[r + 2u],
                        gInt[r + 3u], gInt[r + 4u], gInt[r + 5u]);
    }
    // gH is the half sample h: the vertical six-tap of the integer samples, rounded and clipped.
    for (i = tid; i < kWinHalf * kWinHalf; i += 32u) {
        const uint yr = i / kWinHalf;
        const uint c = (i - yr * kWinHalf) + 2u;
        gH[i] = int(Clip255((Tap6(gInt[(yr + 0u) * kWinInt + c], gInt[(yr + 1u) * kWinInt + c],
                                  gInt[(yr + 2u) * kWinInt + c], gInt[(yr + 3u) * kWinInt + c],
                                  gInt[(yr + 4u) * kWinInt + c], gInt[(yr + 5u) * kWinInt + c])
                             + 16) >> 5));
    }
    GroupMemoryBarrierWithGroupSync();
    // gJ is the centre sample j: the vertical six-tap of gBRaw, rounded and clipped.
    for (i = tid; i < kWinHalf * kWinHalf; i += 32u) {
        const uint yr = i / kWinHalf;
        const uint xc = i - yr * kWinHalf;
        gJ[i] = int(Clip255((Tap6(gBRaw[(yr + 0u) * kWinHalf + xc], gBRaw[(yr + 1u) * kWinHalf + xc],
                                  gBRaw[(yr + 2u) * kWinHalf + xc], gBRaw[(yr + 3u) * kWinHalf + xc],
                                  gBRaw[(yr + 4u) * kWinHalf + xc], gBRaw[(yr + 5u) * kWinHalf + xc])
                             + 512) >> 10));
    }
    GroupMemoryBarrierWithGroupSync();
}

// HalfSample of h264_common.hlsli, out of the windows. (rxi,ryi) is relative to the window centre
// (bx+cx, by+cy) and lies in [-1,16] for every candidate the refinement stages can reach.
uint HalfCached(int rxi, int ryi, uint hx, uint hy)
{
    uint v;
    if (hy == 0u) {
        v = (hx == 0u) ? uint(gInt[(ryi + 3) * int(kWinInt) + (rxi + 3)])
                       : Clip255((gBRaw[(ryi + 3) * int(kWinHalf) + (rxi + 1)] + 16) >> 5);
    } else {
        v = (hx == 0u) ? uint(gH[(ryi + 1) * int(kWinHalf) + (rxi + 1)])
                       : uint(gJ[(ryi + 1) * int(kWinHalf) + (rxi + 1)]);
    }
    return v;
}

// InterpLuma of h264_common.hlsli, out of the windows. (px,py) is the position inside the macroblock,
// 0..15, and the shape of the four cases is the one of Table 8-12 that file derives.
uint InterpCached(int px, int py, int mvx, int mvy, int cx, int cy)
{
    const int rxi = px + (mvx >> 2) - cx;
    const int ryi = py + (mvy >> 2) - cy;
    const uint xf = uint(mvx) & 3u;
    const uint yf = uint(mvy) & 3u;
    const uint hx = xf >> 1u;
    const uint hy = yf >> 1u;
    const uint ox = xf & 1u;
    const uint oy = yf & 1u;
    const int nx = (xf == 3u) ? 1 : 0;
    const int ny = (yf == 3u) ? 1 : 0;

    uint result;
    if (ox == 0u && oy == 0u) {
        result = HalfCached(rxi, ryi, hx, hy);
    } else if (oy == 0u) {
        result = (HalfCached(rxi, ryi, hx, hy) + HalfCached(rxi + nx, ryi, 1u - hx, hy) + 1u) >> 1u;
    } else if (ox == 0u) {
        result = (HalfCached(rxi, ryi, hx, hy) + HalfCached(rxi, ryi + ny, hx, 1u - hy) + 1u) >> 1u;
    } else {
        result = (HalfCached(rxi, ryi + ny, 1u, 0u) + HalfCached(rxi + nx, ryi, 0u, 1u) + 1u) >> 1u;
    }
    return result;
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

    // Half sample then quarter sample refinement, in quarter sample units, scored out of the windows.
    CacheSubpelWindow(tid, bx, by, cx, cy);
    const uint first = tid * 8u;
    const uint sy = first >> 4u;
    const uint sx = first & 15u;
    int qx = cx * 4;
    int qy = cy * 4;
    for (stage = 0; stage < 2; ++stage) {
        const int step = (stage == 0) ? 2 : 1;
        // Every lane sums its eight source samples for all nine candidates. Integer addition is
        // associative, so the cross-lane sum below is the same number the single-lane loop produced,
        // and no partial can overflow: 256 differences of at most 255 reach 65280.
        for (uint c = 0; c < 9u; ++c) {
            const int dx = int(c % 3u) - 1;
            const int dy = int(c / 3u) - 1;
            const int mx = qx + dx * step;
            const int my = qy + dy * step;
            uint part = 0;
            [unroll] for (uint k = 0; k < 8u; ++k) {
                part += uint(abs(int(gSrcMb[first + k]) -
                                 int(InterpCached(int(sx + k), int(sy), mx, my, cx, cy))));
            }
            gPart[c * 32u + tid] = part;
        }
        GroupMemoryBarrierWithGroupSync();
        if (tid < 9u) {
            const int dx = int(tid % 3u) - 1;
            const int dy = int(tid / 3u) - 1;
            const int mx = qx + dx * step;
            const int my = qy + dy * step;
            uint sad = 0;
            for (uint k = 0; k < 32u; ++k) {
                sad += gPart[tid * 32u + k];
            }
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
