// SPDX-License-Identifier: MIT
// Macroblock pass: prediction, forward transform, quantisation, the normative dequantisation and
// inverse transform, and reconstruction. One thread group of 32 threads per macroblock; threads 0..15
// own the sixteen luma 4x4 blocks in raster order inside the macroblock, threads 16..23 the eight
// chroma 4x4 blocks (16..19 Cb, 20..23 Cr). All 32 threads take part in the sample stages.
//
// Two entry points:
//   CSEncodeIntra - I pictures. Intra_16x16 with all four prediction modes plus the four chroma modes.
//                   Intra prediction reads the unfiltered reconstruction of the neighbouring
//                   macroblocks, so the dispatch walks the picture one anti-diagonal at a time
//                   (mbx + mby == gDiagonal), which is the only conformant way to parallelise it.
//   CSEncodeInter - P pictures. Every macroblock is P_L0_16x16 with the vector cs_me.hlsl chose, so
//                   there is no dependency between macroblocks and the whole picture is one dispatch.
//
// Intra macroblocks inside a P picture are deliberately not produced: they would force the P picture
// onto the wavefront as well. Scene changes are handled by the rate controller forcing an IDR.

#include "h264_common.hlsli"

groupshared uint gSrcY[256];
groupshared uint gSrcC[2][64];
groupshared int  gPredY[256];
groupshared int  gPredC[2][64];
groupshared int  gCoef[24][16];
groupshared int  gLev[24][16];
groupshared int  gDcY[16];
groupshared int  gDcLevY[16];
groupshared int  gDcRecY[16];
groupshared int  gDcLevC[2][4];
groupshared int  gDcRecC[2][4];
groupshared uint gAboveY[17];        // index 0 is the corner sample p[-1,-1]
groupshared uint gLeftY[16];
groupshared uint gAboveC[2][9];
groupshared uint gLeftC[2][8];
groupshared uint gHaveL;
groupshared uint gHaveA;
groupshared int  gDcPredY;
groupshared int  gPlA, gPlB, gPlC;
groupshared int  gDcPredC[2][4];
groupshared int  gPlAC[2], gPlBC[2], gPlCC[2];
groupshared uint gModeY, gModeC;
groupshared uint gRedCost[32];
groupshared uint gNnz[24];
groupshared uint gCbpLuma, gCbpChroma;
groupshared uint gMbCost;

// --- source and neighbour loading ------------------------------------------------------------------
void LoadSource(uint mbx, uint mby, uint tid)
{
    const uint first = tid * 8u;
    const uint ly = first >> 4u;
    const uint lx = first & 15u;
    const uint addr = (mby * 16u + ly) * gPadW + mbx * 16u + lx;
    const uint w0 = bufSrcY.Load(addr);
    const uint w1 = bufSrcY.Load(addr + 4u);
    // Written out rather than looped: fxc 10.1 fails code generation for a loop that stores to a
    // groupshared array at a dynamic base index, even a fully unrollable one. See the same note in
    // cs_me.hlsl.
    gSrcY[first + 0u] = w0 & 0xFFu;
    gSrcY[first + 1u] = (w0 >> 8u) & 0xFFu;
    gSrcY[first + 2u] = (w0 >> 16u) & 0xFFu;
    gSrcY[first + 3u] = (w0 >> 24u) & 0xFFu;
    gSrcY[first + 4u] = w1 & 0xFFu;
    gSrcY[first + 5u] = (w1 >> 8u) & 0xFFu;
    gSrcY[first + 6u] = (w1 >> 16u) & 0xFFu;
    gSrcY[first + 7u] = (w1 >> 24u) & 0xFFu;

    const uint comp = tid >> 4u;
    const uint ci = (tid & 15u) * 4u;
    const uint cy = ci >> 3u;
    const uint cx = ci & 7u;
    const uint caddr = (mby * 8u + cy) * (gPadW >> 1u) + mbx * 8u + cx;
    const uint cw = (comp == 0u) ? bufSrcCb.Load(caddr) : bufSrcCr.Load(caddr);
    gSrcC[comp][ci + 0u] = cw & 0xFFu;
    gSrcC[comp][ci + 1u] = (cw >> 8u) & 0xFFu;
    gSrcC[comp][ci + 2u] = (cw >> 16u) & 0xFFu;
    gSrcC[comp][ci + 3u] = (cw >> 24u) & 0xFFu;
}

void LoadNeighbours(uint mbx, uint mby)
{
    const uint x0 = mbx * 16u;
    const uint y0 = mby * 16u;
    const uint cx0 = mbx * 8u;
    const uint cy0 = mby * 8u;
    const uint cstride = gPadW >> 1u;
    gHaveL = (mbx > 0u) ? 1u : 0u;
    gHaveA = (mby > 0u) ? 1u : 0u;

    gAboveY[0] = (gHaveL != 0u && gHaveA != 0u)
                     ? LoadByteRW(rwY, (y0 - 1u) * gPadW + x0 - 1u) : 128u;
    uint i;
    for (i = 0; i < 16u; ++i) {
        gAboveY[1u + i] = (gHaveA != 0u) ? LoadByteRW(rwY, (y0 - 1u) * gPadW + x0 + i) : 128u;
        gLeftY[i] = (gHaveL != 0u) ? LoadByteRW(rwY, (y0 + i) * gPadW + x0 - 1u) : 128u;
    }
    for (uint c = 0; c < 2u; ++c) {
        gAboveC[c][0] = (gHaveL != 0u && gHaveA != 0u)
            ? (c == 0u ? LoadByteRW(rwCb, (cy0 - 1u) * cstride + cx0 - 1u)
                       : LoadByteRW(rwCr, (cy0 - 1u) * cstride + cx0 - 1u))
            : 128u;
        for (i = 0; i < 8u; ++i) {
            gAboveC[c][1u + i] = (gHaveA != 0u)
                ? (c == 0u ? LoadByteRW(rwCb, (cy0 - 1u) * cstride + cx0 + i)
                           : LoadByteRW(rwCr, (cy0 - 1u) * cstride + cx0 + i))
                : 128u;
            gLeftC[c][i] = (gHaveL != 0u)
                ? (c == 0u ? LoadByteRW(rwCb, (cy0 + i) * cstride + cx0 - 1u)
                           : LoadByteRW(rwCr, (cy0 + i) * cstride + cx0 - 1u))
                : 128u;
        }
    }
}

// --- intra prediction values (clauses 8.3.3 and 8.3.4) ---------------------------------------------
void PrepareIntra()
{
    uint i;
    int sumA = 0, sumL = 0;
    for (i = 0; i < 16u; ++i) {
        sumA += int(gAboveY[1u + i]);
        sumL += int(gLeftY[i]);
    }
    if (gHaveA != 0u && gHaveL != 0u) {
        gDcPredY = (sumA + sumL + 16) >> 5;
    } else if (gHaveA != 0u) {
        gDcPredY = (sumA + 8) >> 4;
    } else if (gHaveL != 0u) {
        gDcPredY = (sumL + 8) >> 4;
    } else {
        gDcPredY = 128;
    }

    int H = 0, V = 0;
    for (i = 0; i < 8u; ++i) {
        // Clause 8.3.3.4: both sums reach p[-1,-1] at i == 7. In gAboveY the corner is element 0, so
        // 1 + 6 - i lands on it by itself; gLeftY has no corner element, hence the explicit pick.
        // The index is clamped as well as guarded because the compiler evaluates both arms of a
        // literal-unrolled conditional.
        const uint liy = (i < 7u) ? (6u - i) : 0u;
        const int xr = int(gAboveY[1u + 6u - i]);
        const int yr = (i == 7u) ? int(gAboveY[0]) : int(gLeftY[liy]);
        H += int(i + 1u) * (int(gAboveY[1u + 8u + i]) - xr);
        V += int(i + 1u) * (int(gLeftY[8u + i]) - yr);
    }
    gPlA = 16 * (int(gLeftY[15]) + int(gAboveY[16]));
    gPlB = (5 * H + 32) >> 6;
    gPlC = (5 * V + 32) >> 6;

    for (uint c = 0; c < 2u; ++c) {
        for (uint blk = 0; blk < 4u; ++blk) {
            const uint xO = (blk & 1u) * 4u;
            const uint yO = (blk >> 1u) * 4u;
            int sa = 0, sl = 0;
            for (i = 0; i < 4u; ++i) {
                sa += int(gAboveC[c][1u + xO + i]);
                sl += int(gLeftC[c][yO + i]);
            }
            const bool useBoth = (xO == 0u && yO == 0u) || (xO > 0u && yO > 0u);
            int v;
            if (useBoth) {
                if (gHaveA != 0u && gHaveL != 0u)      v = (sa + sl + 4) >> 3;
                else if (gHaveA != 0u)                 v = (sa + 2) >> 2;
                else if (gHaveL != 0u)                 v = (sl + 2) >> 2;
                else                                   v = 128;
            } else if (xO > 0u) {                       // xO > 0 and yO == 0: above first
                if (gHaveA != 0u)                      v = (sa + 2) >> 2;
                else if (gHaveL != 0u)                 v = (sl + 2) >> 2;
                else                                   v = 128;
            } else {                                   // xO == 0 and yO > 0: left first
                if (gHaveL != 0u)                      v = (sl + 2) >> 2;
                else if (gHaveA != 0u)                 v = (sa + 2) >> 2;
                else                                   v = 128;
            }
            gDcPredC[c][blk] = v;
        }
        int hc = 0, vc = 0;
        for (i = 0; i < 4u; ++i) {
            // Clause 8.3.4.4, same corner case as the luma plane mode above.
            const uint lic = (i < 3u) ? (2u - i) : 0u;
            const int xr = int(gAboveC[c][1u + 2u - i]);
            const int yr = (i == 3u) ? int(gAboveC[c][0]) : int(gLeftC[c][lic]);
            hc += int(i + 1u) * (int(gAboveC[c][1u + 4u + i]) - xr);
            vc += int(i + 1u) * (int(gLeftC[c][4u + i]) - yr);
        }
        gPlAC[c] = 16 * (int(gLeftC[c][7]) + int(gAboveC[c][8]));
        gPlBC[c] = (34 * hc + 32) >> 6;
        gPlCC[c] = (34 * vc + 32) >> 6;
    }
}

// Single assignment, single return: fxc cannot prove a multi-return function initialises its result
// and then fails code generation when the optimiser runs (the same defect as in InterpLuma).
int PredIntraY(uint mode, uint x, uint y)
{
    int v;
    if (mode == 0u) {
        v = int(gAboveY[1u + x]);                                      // Intra_16x16_Vertical
    } else if (mode == 1u) {
        v = int(gLeftY[y]);                                            // Intra_16x16_Horizontal
    } else if (mode == 2u) {
        v = gDcPredY;                                                  // Intra_16x16_DC
    } else {
        v = int(Clip255((gPlA + gPlB * (int(x) - 7) + gPlC * (int(y) - 7) + 16) >> 5));
    }
    return v;
}

int PredIntraC(uint comp, uint mode, uint x, uint y)
{
    int v;
    if (mode == 0u) {
        v = gDcPredC[comp][(y >> 2u) * 2u + (x >> 2u)];                 // Intra_Chroma_DC
    } else if (mode == 1u) {
        v = int(gLeftC[comp][y]);                                       // Horizontal
    } else if (mode == 2u) {
        v = int(gAboveC[comp][1u + x]);                                 // Vertical
    } else {
        v = int(Clip255((gPlAC[comp] + gPlBC[comp] * (int(x) - 3) +
                         gPlCC[comp] * (int(y) - 3) + 16) >> 5));
    }
    return v;
}

uint ModeAvailableY(uint mode)
{
    if (mode == 0u) return gHaveA;
    if (mode == 1u) return gHaveL;
    if (mode == 2u) return 1u;
    return (gHaveA != 0u && gHaveL != 0u) ? 1u : 0u;
}
uint ModeAvailableC(uint mode)
{
    if (mode == 0u) return 1u;
    if (mode == 1u) return gHaveL;
    if (mode == 2u) return gHaveA;
    return (gHaveA != 0u && gHaveL != 0u) ? 1u : 0u;
}

// --- the body shared by both entry points ----------------------------------------------------------
void EncodeMb(uint mbx, uint mby, uint tid, bool intra)
{
    const uint mbIdx = mby * gWidthMb + mbx;
    const uint mbInfoBase = mbIdx * kMbInfoWords * 4u;
    const uint levelsBase = mbIdx * kLevelsWordsPerMb * 4u;

    LoadSource(mbx, mby, tid);
    if (tid == 0u) {
        if (intra) {
            LoadNeighbours(mbx, mby);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0u && intra) {
        PrepareIntra();
    }
    GroupMemoryBarrierWithGroupSync();

    int mvx = 0, mvy = 0;
    if (!intra) {
        mvx = int(rwMbInfo.Load(mbInfoBase + 3u * 4u));
        mvy = int(rwMbInfo.Load(mbInfoBase + 4u * 4u));
    }

    // ---- mode decision (intra only): threads 0..3 luma modes, 4..7 chroma modes -------------------
    if (intra) {
        gRedCost[tid] = 0xFFFFFFFFu;
        GroupMemoryBarrierWithGroupSync();
        if (tid < 4u) {
            if (ModeAvailableY(tid) != 0u) {
                uint sad = 0;
                for (uint y = 0; y < 16u; ++y) {
                    for (uint x = 0; x < 16u; ++x) {
                        sad += uint(abs(int(gSrcY[y * 16u + x]) - PredIntraY(tid, x, y)));
                    }
                }
                gRedCost[tid] = sad;
            }
        } else if (tid < 8u) {
            const uint mode = tid - 4u;
            if (ModeAvailableC(mode) != 0u) {
                uint sad = 0;
                for (uint c = 0; c < 2u; ++c) {
                    for (uint y = 0; y < 8u; ++y) {
                        for (uint x = 0; x < 8u; ++x) {
                            sad += uint(abs(int(gSrcC[c][y * 8u + x]) - PredIntraC(c, mode, x, y)));
                        }
                    }
                }
                gRedCost[tid] = sad;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (tid == 0u) {
            uint bestY = 2u, bestC = 0u;
            uint cY = gRedCost[2], cC = gRedCost[4];
            for (uint m = 0; m < 4u; ++m) {
                if (gRedCost[m] < cY) { cY = gRedCost[m]; bestY = m; }
                if (gRedCost[4u + m] < cC) { cC = gRedCost[4u + m]; bestC = m; }
            }
            gModeY = bestY;
            gModeC = bestC;
            gMbCost = cY;
        }
        GroupMemoryBarrierWithGroupSync();
    } else if (tid == 0u) {
        gModeY = 0u;
        gModeC = 0u;
        gMbCost = rwMbInfo.Load(mbInfoBase + 12u * 4u);
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- prediction -------------------------------------------------------------------------------
    {
        const uint first = tid * 8u;
        const uint ly = first >> 4u;
        const uint lx = first & 15u;
        // No [unroll] here: the inter arm expands InterpLuma's sixteen fractional cases, and fxc
        // refuses to unroll a body that large. A real loop of eight is cheaper than the refusal.
        for (uint i = 0; i < 8u; ++i) {
            const uint x = lx + i;
            gPredY[ly * 16u + x] = intra ? PredIntraY(gModeY, x, ly)
                                         : int(InterpLuma(int(mbx * 16u + x), int(mby * 16u + ly),
                                                          mvx, mvy));
        }
        const uint comp = tid >> 4u;
        const uint ci = (tid & 15u) * 4u;
        const uint cy = ci >> 3u;
        const uint cx = ci & 7u;
        for (uint k = 0; k < 4u; ++k) {
            const uint x = cx + k;
            gPredC[comp][cy * 8u + x] =
                intra ? PredIntraC(comp, gModeC, x, cy)
                      : int(InterpChroma(comp, int(mbx * 8u + x), int(mby * 8u + cy), mvx, mvy));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- forward transform ------------------------------------------------------------------------
    const bool isLuma = tid < 16u;
    const bool isChroma = (tid >= 16u) && (tid < 24u);
    const uint comp = isChroma ? ((tid - 16u) >> 2u) : 0u;
    const uint cblk = isChroma ? ((tid - 16u) & 3u) : 0u;
    const uint qp = isChroma ? gQpC : gQpY;

    if (isLuma || isChroma) {
        int r[16];
        if (isLuma) {
            const uint bx = (tid & 3u) * 4u;
            const uint by = (tid >> 2u) * 4u;
            [unroll] for (uint i = 0; i < 16u; ++i) {
                const uint p = (by + (i >> 2u)) * 16u + bx + (i & 3u);
                r[i] = int(gSrcY[p]) - gPredY[p];
            }
        } else {
            const uint bx = (cblk & 1u) * 4u;
            const uint by = (cblk >> 1u) * 4u;
            [unroll] for (uint i = 0; i < 16u; ++i) {
                const uint p = (by + (i >> 2u)) * 8u + bx + (i & 3u);
                r[i] = int(gSrcC[comp][p]) - gPredC[comp][p];
            }
        }
        Forward4x4(r);
        [unroll] for (uint i = 0; i < 16u; ++i) {
            gCoef[tid][i] = r[i];
        }
        if (isLuma) {
            gDcY[tid] = r[0];
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- DC transforms ---------------------------------------------------------------------------
    if (intra && tid == 0u) {
        // [unroll] on every loop in this block: without it fxc 10.1 fails code generation
        // ("Internal error: invalid read of more specific predicate") for the single-thread DC
        // transform. Same class of defect as the groupshared store loops in cs_me.hlsl.
        int d[16];
        uint i;
        [unroll] for (i = 0; i < 16u; ++i) {
            d[i] = gDcY[i];
        }
        Hadamard4x4(d);
        [unroll] for (i = 0; i < 16u; ++i) {
            gDcLevY[i] = QuantLumaDc(d[i], gQpY, true);
        }
        [unroll] for (i = 0; i < 8u; ++i) {
            rwLevels.Store(levelsBase + (kLevelsOffLumaDc + i) * 4u,
                           PackLevels(gDcLevY[kZigZag[i * 2u]], gDcLevY[kZigZag[i * 2u + 1u]]));
        }
        int c[16];
        [unroll] for (i = 0; i < 16u; ++i) {
            c[i] = gDcLevY[i];
        }
        Hadamard4x4(c);
        [unroll] for (i = 0; i < 16u; ++i) {
            gDcRecY[i] = DequantLumaDc(c[i], gQpY);
        }
    }
    if (tid == 1u || tid == 2u) {
        const uint c = tid - 1u;
        const int c0 = gCoef[16u + c * 4u + 0u][0];
        const int c1 = gCoef[16u + c * 4u + 1u][0];
        const int c2 = gCoef[16u + c * 4u + 2u][0];
        const int c3 = gCoef[16u + c * 4u + 3u][0];
        const int f0 = c0 + c1 + c2 + c3;
        const int f1 = c0 - c1 + c2 - c3;
        const int f2 = c0 + c1 - c2 - c3;
        const int f3 = c0 - c1 - c2 + c3;
        gDcLevC[c][0] = QuantChromaDc(f0, gQpC, intra);
        gDcLevC[c][1] = QuantChromaDc(f1, gQpC, intra);
        gDcLevC[c][2] = QuantChromaDc(f2, gQpC, intra);
        gDcLevC[c][3] = QuantChromaDc(f3, gQpC, intra);
        const int l0 = gDcLevC[c][0], l1 = gDcLevC[c][1], l2 = gDcLevC[c][2], l3 = gDcLevC[c][3];
        gDcRecC[c][0] = DequantChromaDc(l0 + l1 + l2 + l3, gQpC);
        gDcRecC[c][1] = DequantChromaDc(l0 - l1 + l2 - l3, gQpC);
        gDcRecC[c][2] = DequantChromaDc(l0 + l1 - l2 - l3, gQpC);
        gDcRecC[c][3] = DequantChromaDc(l0 - l1 - l2 + l3, gQpC);
        rwLevels.Store(levelsBase + (kLevelsOffChromaDc + c * 2u) * 4u, PackLevels(l0, l1));
        rwLevels.Store(levelsBase + (kLevelsOffChromaDc + c * 2u + 1u) * 4u, PackLevels(l2, l3));
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- AC quantisation -------------------------------------------------------------------------
    const uint firstScan = (isChroma || intra) ? 1u : 0u;
    if (isLuma || isChroma) {
        uint nnz = 0;
        gLev[tid][0] = 0;
        for (uint s = firstScan; s < 16u; ++s) {
            const uint r = kZigZag[s];
            const int lev = Quant4x4(gCoef[tid][r], qp, PosClass(r >> 2u, r & 3u), intra);
            gLev[tid][s] = lev;
            if (lev != 0) {
                ++nnz;
            }
        }
        gNnz[tid] = nnz;
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- coded block pattern ---------------------------------------------------------------------
    if (tid == 0u) {
        uint i;
        if (intra) {
            uint any = 0;
            for (i = 0; i < 16u; ++i) {
                any |= gNnz[i];
            }
            gCbpLuma = (any != 0u) ? 15u : 0u;
        } else {
            uint bits = 0;
            for (i = 0; i < 16u; ++i) {
                if (gNnz[i] != 0u) {
                    bits |= 1u << (((i >> 3u) * 2u) + ((i & 3u) >> 1u));
                }
            }
            gCbpLuma = bits;
        }
        uint anyAc = 0;
        for (i = 16u; i < 24u; ++i) {
            anyAc |= gNnz[i];
        }
        uint anyDc = 0;
        for (i = 0; i < 4u; ++i) {
            anyDc |= uint(abs(gDcLevC[0][i])) | uint(abs(gDcLevC[1][i]));
        }
        gCbpChroma = (anyAc != 0u) ? 2u : ((anyDc != 0u) ? 1u : 0u);
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- level emission and reconstruction -------------------------------------------------------
    if (isLuma || isChroma) {
        bool keep;
        uint wordBase;
        if (isLuma) {
            const uint b8 = ((tid >> 3u) * 2u) + ((tid & 3u) >> 1u);
            keep = intra ? (gCbpLuma != 0u) : (((gCbpLuma >> b8) & 1u) != 0u);
            wordBase = kLevelsOffLumaAc + tid * 8u;
        } else {
            keep = (gCbpChroma == 2u);
            wordBase = kLevelsOffChromaAc + (comp * 4u + cblk) * 8u;
        }
        uint w;
        for (w = 0; w < 8u; ++w) {
            const int lo = keep ? gLev[tid][w * 2u] : 0;
            const int hi = keep ? gLev[tid][w * 2u + 1u] : 0;
            rwLevels.Store(levelsBase + (wordBase + w) * 4u, PackLevels(lo, hi));
        }

        int d[16];
        [unroll] for (w = 0; w < 16u; ++w) {
            d[w] = 0;
        }
        if (keep) {
            for (uint s = firstScan; s < 16u; ++s) {
                const uint r = kZigZag[s];
                d[r] = Dequant4x4(gLev[tid][s], qp, PosClass(r >> 2u, r & 3u));
            }
        }
        if (isLuma && intra) {
            d[0] = gDcRecY[tid];
        } else if (isChroma) {
            d[0] = (gCbpChroma != 0u) ? gDcRecC[comp][cblk] : 0;
        }
        Inverse4x4(d);

        if (isLuma) {
            const uint bx = (tid & 3u) * 4u;
            const uint by = (tid >> 2u) * 4u;
            [unroll] for (uint row = 0; row < 4u; ++row) {
                uint b[4];
                [unroll] for (uint k = 0; k < 4u; ++k) {
                    const uint p = (by + row) * 16u + bx + k;
                    b[k] = Clip255(gPredY[p] + ((d[row * 4u + k] + 32) >> 6));
                }
                rwY.Store((mby * 16u + by + row) * gPadW + mbx * 16u + bx,
                          PackBytes(b[0], b[1], b[2], b[3]));
            }
        } else {
            const uint bx = (cblk & 1u) * 4u;
            const uint by = (cblk >> 1u) * 4u;
            const uint cstride = gPadW >> 1u;
            [unroll] for (uint row = 0; row < 4u; ++row) {
                uint b[4];
                [unroll] for (uint k = 0; k < 4u; ++k) {
                    const uint p = (by + row) * 8u + bx + k;
                    b[k] = Clip255(gPredC[comp][p] + ((d[row * 4u + k] + 32) >> 6));
                }
                const uint addr = (mby * 8u + by + row) * cstride + mbx * 8u + bx;
                const uint packed = PackBytes(b[0], b[1], b[2], b[3]);
                if (comp == 0u) {
                    rwCb.Store(addr, packed);
                } else {
                    rwCr.Store(addr, packed);
                }
            }
        }
        gNnz[tid] = keep ? gNnz[tid] : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- macroblock info -------------------------------------------------------------------------
    if (tid == 0u) {
        const uint flags = intra ? (1u | (gModeY << 1u) | (gModeC << 3u)) : 0u;
        rwMbInfo.Store(mbInfoBase + 0u * 4u, flags);
        rwMbInfo.Store(mbInfoBase + 1u * 4u, gCbpLuma);
        rwMbInfo.Store(mbInfoBase + 2u * 4u, gCbpChroma);
        if (intra) {
            rwMbInfo.Store(mbInfoBase + 3u * 4u, 0u);
            rwMbInfo.Store(mbInfoBase + 4u * 4u, 0u);
        }
        uint i;
        for (i = 0; i < 4u; ++i) {
            rwMbInfo.Store(mbInfoBase + (5u + i) * 4u,
                           gNnz[i * 4u] | (gNnz[i * 4u + 1u] << 8u) |
                           (gNnz[i * 4u + 2u] << 16u) | (gNnz[i * 4u + 3u] << 24u));
        }
        for (i = 0; i < 2u; ++i) {
            rwMbInfo.Store(mbInfoBase + (9u + i) * 4u,
                           gNnz[16u + i * 4u] | (gNnz[16u + i * 4u + 1u] << 8u) |
                           (gNnz[16u + i * 4u + 2u] << 16u) | (gNnz[16u + i * 4u + 3u] << 24u));
        }
        uint dcNnzY = 0, dcNnzCb = 0, dcNnzCr = 0;
        if (intra) {
            for (i = 0; i < 16u; ++i) {
                if (gDcLevY[i] != 0) { ++dcNnzY; }
            }
        }
        for (i = 0; i < 4u; ++i) {
            if (gCbpChroma != 0u && gDcLevC[0][i] != 0) { ++dcNnzCb; }
            if (gCbpChroma != 0u && gDcLevC[1][i] != 0) { ++dcNnzCr; }
        }
        rwMbInfo.Store(mbInfoBase + 11u * 4u, dcNnzY | (dcNnzCb << 8u) | (dcNnzCr << 16u));
        rwMbInfo.Store(mbInfoBase + 12u * 4u, gMbCost);
    }
}

[numthreads(32, 1, 1)]
void CSEncodeIntra(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    const uint mbx = gDiagonalBase + gid.x;
    if (mbx > gDiagonal) {
        return;
    }
    const uint mby = gDiagonal - mbx;
    if (mbx >= gWidthMb || mby >= gHeightMb) {
        return;
    }
    EncodeMb(mbx, mby, tid, true);
}

[numthreads(32, 1, 1)]
void CSEncodeInter(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    if (gid.x >= gWidthMb || gid.y >= gHeightMb) {
        return;
    }
    EncodeMb(gid.x, gid.y, tid, false);
}
