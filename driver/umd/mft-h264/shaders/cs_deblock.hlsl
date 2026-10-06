// SPDX-License-Identifier: MIT
// Deblocking filter, ITU-T Rec. H.264 clause 8.7, in place on the reconstruction.
//
// The filter is specified macroblock by macroblock in raster order, all vertical edges of a macroblock
// before all its horizontal edges, with samples already filtered feeding the later edges. That makes a
// frame-wide "all vertical then all horizontal" pass non-conformant.
//
// The wavefront is not the anti-diagonal the intra pass uses. Unlike intra prediction, which only reads
// its neighbours, this filter writes into them: the vertical edge at the left macroblock boundary
// modifies the left neighbour's last four columns, and the horizontal edge at the top boundary modifies
// the macroblock above. So macroblock (x,y) reads samples that (x+1,y-1) has already written - the four
// columns x0+12..x0+15 of the rows above it, which that macroblock's own left edge filtering changes -
// and the dependency set is {(x-1,y), (x,y-1), (x+1,y-1)}. A schedule t(x,y) = a*x + b*y satisfies all
// three only when b > a > 0, so this walks t = mbx + 2*mby. Two macroblocks on one such wave differ by
// (-2,+1), 32 luma samples apart in x, and their 20x20 working neighbourhoods are disjoint.
//
// Filtering on the anti-diagonal instead put (1,0) and (0,1) in the same wave, which race over the
// 4x3 corner where (1,0)'s left edge meets (0,1)'s top edge: that corner was the whole difference
// against the inbox decoder (ours 27, decoder 28 at (15,13) of a 64x48 intra picture).
//
// Within a macroblock the vertical edges touch only their own sample row and the horizontal edges only
// their own column, so the group works on a groupshared copy of the neighbourhood and writes it back as
// whole 32-bit words at the end: no byte read-modify-write race between threads.
//
// Intra prediction of the current picture reads the unfiltered reconstruction (clause 8.3), so this
// pass runs after the whole picture is reconstructed, and its output is both the output picture and the
// reference for the next picture.

// The reconstruction views of this pass are globallycoherent and one more slot carries the row
// progress counters: CSDeblockRows below runs the whole wavefront inside one dispatch, so a thread
// group reads samples another group of the same dispatch wrote and no dispatch boundary separates
// the two. See h264_common.hlsli.
#define BC250_DEBLOCK_PASS
#include "h264_common.hlsli"

// Table 8-16.
static const int kAlpha[52] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      4,   4,   5,   6,   7,   8,   9,  10,  12,  13,  15,  17,  20,  22,  25,  28,
     32,  36,  40,  45,  50,  56,  63,  71,  80,  90, 101, 113, 127, 144, 162, 182,
    203, 226, 255, 255,
};
static const int kBeta[52] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      2,   2,   2,   3,   3,   3,   3,   4,   4,   4,   6,   6,   7,   7,   8,   8,
      9,   9,  10,  10,  11,  11,  12,  12,  13,  13,  14,  14,  15,  15,  16,  16,
     17,  17,  18,  18,
};
// Table 8-17, tc0 for bS 1, 2 and 3. Rows below indexA 16 are never reached because alpha is 0 there.
//
// Rows 35 and up were one off before: {2,2,4} was repeated at 34 and 35 and every later row sat one
// index too low, so the last row (13,17,25) was missing. The decoder oracle localised it exactly -
// all-intra pictures were bit exact at indexA 33..36 and 39 and differed at 37, 38 and 40..51, and
// indexA 35 passed only because its one wrong column is bS 2, which an I slice never uses.
static const int kTc0[52][3] = {
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,1},{0,1,1},{1,1,1},
    {1,1,1},{1,1,1},{1,1,1},{1,1,2},{1,1,2},{1,1,2},{1,1,2},{1,2,3},
    {1,2,3},{2,2,3},{2,2,4},{2,3,4},{2,3,4},{3,3,5},{3,4,6},{3,4,6},
    {4,5,7},{4,5,8},{4,6,9},{5,7,10},{6,8,11},{6,8,13},{7,10,14},{8,11,16},
    {9,12,18},{10,13,20},{11,15,23},{13,17,25},
};

groupshared int gL[20 * 20];      // luma, x and y from -4 to 15
groupshared int gC[2][12 * 12];   // chroma, x and y from -4 to 7

uint LumaAt(int x, int y)
{
    const int cx = clamp(x, 0, int(gPadW) - 1);
    const int cy = clamp(y, 0, int(gPadH) - 1);
    return LoadByteRW(rwY, uint(cy) * gPadW + uint(cx));
}
uint ChromaAt(uint comp, int x, int y)
{
    const int cx = clamp(x, 0, int(gPadW >> 1u) - 1);
    const int cy = clamp(y, 0, int(gPadH >> 1u) - 1);
    const uint addr = uint(cy) * (gPadW >> 1u) + uint(cx);
    return (comp == 0u) ? LoadByteRW(rwCb, addr) : LoadByteRW(rwCr, addr);
}

// One packed non-zero count out of an MbInfo nnz word.
//
// Written as a four-way select, not as `(word >> ((byteIndex) * 8u)) & 0xFFu`. Measured with fxc 10.1
// at /O3: the shifted form behaved as 0 for byteIndex 3 at the left macroblock edge, so a left
// neighbour's coefficients in its rightmost 4x4 block column never raised bS to 2 and those edges went
// unfiltered. A literal `>> 24u` on the same address returned the right byte, and this select returns
// it too; what the compiler did with the shifted form was not traced into the DXBC. Both the inbox
// decoder and ffmpeg disagreed with us on exactly those edges, and agree now.
uint NnzByte(uint word, uint byteIndex)
{
    uint v = word & 0xFFu;
    if (byteIndex == 1u) { v = (word >> 8u) & 0xFFu; }
    else if (byteIndex == 2u) { v = (word >> 16u) & 0xFFu; }
    else if (byteIndex == 3u) { v = (word >> 24u) & 0xFFu; }
    return v;
}

// clause 8.7.2.1 for a progressive frame with one reference list and refIdxL0 always 0.
uint DeriveBs(int px, int py, int qx, int qy, bool mbEdge)
{
    const uint pBase = ((uint(py) >> 4u) * gWidthMb + (uint(px) >> 4u)) * kMbInfoWords * 4u;
    const uint qBase = ((uint(qy) >> 4u) * gWidthMb + (uint(qx) >> 4u)) * kMbInfoWords * 4u;
    const uint pIntra = bufMbInfoRO.Load(pBase) & 1u;
    const uint qIntra = bufMbInfoRO.Load(qBase) & 1u;
    if (pIntra != 0u || qIntra != 0u) {
        return mbEdge ? 4u : 3u;
    }
    const uint pr = (((uint(py) >> 2u) & 3u) * 4u) + ((uint(px) >> 2u) & 3u);
    const uint qr = (((uint(qy) >> 2u) & 3u) * 4u) + ((uint(qx) >> 2u) & 3u);
    const uint pn = NnzByte(bufMbInfoRO.Load(pBase + (5u + (pr >> 2u)) * 4u), pr & 3u);
    const uint qn = NnzByte(bufMbInfoRO.Load(qBase + (5u + (qr >> 2u)) * 4u), qr & 3u);
    if (pn != 0u || qn != 0u) {
        return 2u;
    }
    const int pmx = int(bufMbInfoRO.Load(pBase + 3u * 4u));
    const int pmy = int(bufMbInfoRO.Load(pBase + 4u * 4u));
    const int qmx = int(bufMbInfoRO.Load(qBase + 3u * 4u));
    const int qmy = int(bufMbInfoRO.Load(qBase + 4u * 4u));
    if (abs(pmx - qmx) >= 4 || abs(pmy - qmy) >= 4) {
        return 1u;
    }
    return 0u;
}

// clauses 8.7.2.3 and 8.7.2.4. s holds p3,p2,p1,p0,q0,q1,q2,q3.
void FilterEdge(inout int s[8], uint bS, int alpha, int beta, int tc0, bool chroma)
{
    if (bS == 0u) {
        return;
    }
    const int p3 = s[0], p2 = s[1], p1 = s[2], p0 = s[3];
    const int q0 = s[4], q1 = s[5], q2 = s[6], q3 = s[7];
    if (!(abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta)) {
        return;
    }
    const int ap = abs(p2 - p0);
    const int aq = abs(q2 - q0);
    if (bS < 4u) {
        const int tC = chroma ? (tc0 + 1)
                              : (tc0 + ((ap < beta) ? 1 : 0) + ((aq < beta) ? 1 : 0));
        const int delta = clamp((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tC, tC);
        s[3] = clamp(p0 + delta, 0, 255);
        s[4] = clamp(q0 - delta, 0, 255);
        if (!chroma && ap < beta) {
            s[2] = p1 + clamp((p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1, -tc0, tc0);
        }
        if (!chroma && aq < beta) {
            s[5] = q1 + clamp((q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1, -tc0, tc0);
        }
        return;
    }
    if (chroma) {
        s[3] = (2 * p1 + p0 + q1 + 2) >> 2;
        s[4] = (2 * q1 + q0 + p1 + 2) >> 2;
        return;
    }
    const bool near = abs(p0 - q0) < ((alpha >> 2) + 2);
    if (ap < beta && near) {
        s[3] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
        s[2] = (p2 + p1 + p0 + q0 + 2) >> 2;
        s[1] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
    } else {
        s[3] = (2 * p1 + p0 + q1 + 2) >> 2;
    }
    if (aq < beta && near) {
        s[4] = (q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3;
        s[5] = (q2 + q1 + q0 + p0 + 2) >> 2;
        s[6] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
    } else {
        s[4] = (2 * q1 + q0 + p1 + 2) >> 2;
    }
}

// One macroblock of clause 8.7, in place on the reconstruction. Every caller below has first
// established that the dependency set {(mbx-1,mby), (mbx,mby-1), (mbx+1,mby-1)} is already
// filtered and visible to this group.
void DeblockMacroblock(uint mbx, uint mby, uint tid)
{
    const int x0 = int(mbx * 16u);
    const int y0 = int(mby * 16u);
    const int cx0 = int(mbx * 8u);
    const int cy0 = int(mby * 8u);
    const bool haveL = mbx > 0u;
    const bool haveA = mby > 0u;

    uint i;
    for (i = tid; i < 400u; i += 32u) {
        const int ly = int(i / 20u) - 4;
        const int lx = int(i % 20u) - 4;
        gL[i] = int(LumaAt(x0 + lx, y0 + ly));
    }
    for (i = tid; i < 288u; i += 32u) {
        const uint comp = i / 144u;
        const uint k = i % 144u;
        const int ly = int(k / 12u) - 4;
        const int lx = int(k % 12u) - 4;
        gC[comp][k] = int(ChromaAt(comp, cx0 + lx, cy0 + ly));
    }
    GroupMemoryBarrierWithGroupSync();

    // Both quantisers are constant over the picture in this encoder, so qPav is just the picture QP.
    const int idxA = clamp(int(gQpY) + gAlphaOffsetDiv2 * 2, 0, 51);
    const int idxB = clamp(int(gQpY) + gBetaOffsetDiv2 * 2, 0, 51);
    const int alphaY = kAlpha[idxA];
    const int betaY = kBeta[idxB];
    const int idxAC = clamp(int(gQpC) + gAlphaOffsetDiv2 * 2, 0, 51);
    const int idxBC = clamp(int(gQpC) + gBetaOffsetDiv2 * 2, 0, 51);
    const int alphaC = kAlpha[idxAC];
    const int betaC = kBeta[idxBC];

    int s[8];
    uint e;

    // ---- vertical edges, left to right -----------------------------------------------------------
    if (tid < 16u) {
        const int ly = int(tid);
        for (e = 0; e < 4u; ++e) {
            const int lx = int(e) * 4;
            if (lx == 0 && !haveL) {
                continue;
            }
            const uint bS = DeriveBs(x0 + lx - 1, y0 + ly, x0 + lx, y0 + ly, lx == 0);
            if (bS == 0u) {
                continue;
            }
            const int tc0 = (bS < 4u) ? kTc0[idxA][bS - 1u] : 0;
            [unroll] for (i = 0; i < 8u; ++i) {
                s[i] = gL[(ly + 4) * 20 + (lx - 4 + int(i)) + 4];
            }
            FilterEdge(s, bS, alphaY, betaY, tc0, false);
            [unroll] for (i = 0; i < 8u; ++i) {
                gL[(ly + 4) * 20 + (lx - 4 + int(i)) + 4] = s[i];
            }
        }
    } else {
        const uint comp = (tid - 16u) >> 3u;
        const int ly = int((tid - 16u) & 7u);
        for (e = 0; e < 2u; ++e) {
            const int lx = int(e) * 4;
            if (lx == 0 && !haveL) {
                continue;
            }
            const uint bS = DeriveBs(x0 + lx * 2 - 1, y0 + ly * 2, x0 + lx * 2, y0 + ly * 2, lx == 0);
            if (bS == 0u) {
                continue;
            }
            const int tc0 = (bS < 4u) ? kTc0[idxAC][bS - 1u] : 0;
            [unroll] for (i = 0; i < 8u; ++i) {
                s[i] = gC[comp][(ly + 4) * 12 + (lx - 4 + int(i)) + 4];
            }
            FilterEdge(s, bS, alphaC, betaC, tc0, true);
            [unroll] for (i = 0; i < 8u; ++i) {
                gC[comp][(ly + 4) * 12 + (lx - 4 + int(i)) + 4] = s[i];
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- horizontal edges, top to bottom ---------------------------------------------------------
    if (tid < 16u) {
        const int lx = int(tid);
        for (e = 0; e < 4u; ++e) {
            const int ly = int(e) * 4;
            if (ly == 0 && !haveA) {
                continue;
            }
            const uint bS = DeriveBs(x0 + lx, y0 + ly - 1, x0 + lx, y0 + ly, ly == 0);
            if (bS == 0u) {
                continue;
            }
            const int tc0 = (bS < 4u) ? kTc0[idxA][bS - 1u] : 0;
            [unroll] for (i = 0; i < 8u; ++i) {
                s[i] = gL[(ly - 4 + int(i) + 4) * 20 + lx + 4];
            }
            FilterEdge(s, bS, alphaY, betaY, tc0, false);
            [unroll] for (i = 0; i < 8u; ++i) {
                gL[(ly - 4 + int(i) + 4) * 20 + lx + 4] = s[i];
            }
        }
    } else {
        const uint comp = (tid - 16u) >> 3u;
        const int lx = int((tid - 16u) & 7u);
        for (e = 0; e < 2u; ++e) {
            const int ly = int(e) * 4;
            if (ly == 0 && !haveA) {
                continue;
            }
            const uint bS = DeriveBs(x0 + lx * 2, y0 + ly * 2 - 1, x0 + lx * 2, y0 + ly * 2, ly == 0);
            if (bS == 0u) {
                continue;
            }
            const int tc0 = (bS < 4u) ? kTc0[idxAC][bS - 1u] : 0;
            [unroll] for (i = 0; i < 8u; ++i) {
                s[i] = gC[comp][(ly - 4 + int(i) + 4) * 12 + lx + 4];
            }
            FilterEdge(s, bS, alphaC, betaC, tc0, true);
            [unroll] for (i = 0; i < 8u; ++i) {
                gC[comp][(ly - 4 + int(i) + 4) * 12 + lx + 4] = s[i];
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- write back, whole words only ------------------------------------------------------------
    // Rows -3..15 and, in x, the five words covering -4..15. Nothing else on this wave touches
    // these samples, so writing an unmodified byte back with its own value is safe.
    const uint lumaRows = 19u;        // ly from -3 to 15
    const uint lumaWords = 5u;
    // A guarded store rather than a `continue`: a thread-strided loop that leaves its body early is
    // varying flow control to fxc, and the compiler then refuses the next thread sync it meets, which
    // is the one CSDeblockRows places between two macroblocks of the same group (error X3663). The
    // stores are the same ones.
    for (i = tid; i < lumaRows * lumaWords; i += 32u) {
        const int ly = int(i / lumaWords) - 3;
        const int wx = int(i % lumaWords) * 4 - 4;     // -4, 0, 4, 8, 12
        if ((haveA || ly >= 0) && (haveL || wx >= 0)) {
            const uint packed = PackBytes(uint(gL[(ly + 4) * 20 + wx + 4 + 0]),
                                          uint(gL[(ly + 4) * 20 + wx + 4 + 1]),
                                          uint(gL[(ly + 4) * 20 + wx + 4 + 2]),
                                          uint(gL[(ly + 4) * 20 + wx + 4 + 3]));
            rwY.Store(uint(y0 + ly) * gPadW + uint(x0 + wx), packed);
        }
    }
    const uint chromaRows = 11u;      // ly from -3 to 7
    const uint chromaWords = 3u;      // -4, 0, 4
    for (i = tid; i < chromaRows * chromaWords * 2u; i += 32u) {
        const uint comp = i / (chromaRows * chromaWords);
        const uint k = i % (chromaRows * chromaWords);
        const int ly = int(k / chromaWords) - 3;
        const int wx = int(k % chromaWords) * 4 - 4;
        if ((haveA || ly >= 0) && (haveL || wx >= 0)) {
            const uint packed = PackBytes(uint(gC[comp][(ly + 4) * 12 + wx + 4 + 0]),
                                          uint(gC[comp][(ly + 4) * 12 + wx + 4 + 1]),
                                          uint(gC[comp][(ly + 4) * 12 + wx + 4 + 2]),
                                          uint(gC[comp][(ly + 4) * 12 + wx + 4 + 3]));
            const uint addr = uint(cy0 + ly) * (gPadW >> 1u) + uint(cx0 + wx);
            if (comp == 0u) {
                rwCb.Store(addr, packed);
            } else {
                rwCr.Store(addr, packed);
            }
        }
    }
}

// The wavefront as a sequence of dispatches, one per value of t = mbx + 2 * mby: gDiagonal carries t
// and gDiagonalBase the first macroblock row on that wave. Kept because a dispatch boundary needs no
// assumption about thread group residency, which makes this the fallback of
// BC250_MFT_DEBLOCK=wavefront and the shape BC250_MFT_SERIAL_DEBLOCK drives one macroblock at a time.
[numthreads(32, 1, 1)]
void CSDeblock(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    const uint mby = gDiagonalBase + gid.x;
    if (mby >= gHeightMb || mby * 2u > gDiagonal) {
        return;
    }
    const uint mbx = gDiagonal - mby * 2u;
    if (mbx >= gWidthMb) {
        return;
    }
    DeblockMacroblock(mbx, mby, tid);
}

// The same wavefront inside one dispatch. One thread group per macroblock row, Dispatch(heightMb,1,1),
// each group walking its row from left to right and waiting for the row above to be two macroblocks
// ahead. The order is the same t = mbx + 2 * mby schedule: group mby filters macroblock mbx at step
// 2 * mby + mbx + 1, so the critical path is 2 * heightMb + widthMb - 2 macroblocks, which is exactly
// what the dispatch sequence also was. What goes away is the launch cost, and on this encoder the
// launch cost was the whole cost: a least squares fit over the 254 dispatches of a 1080p picture gives
// 9.8 us of fixed cost and a slope of zero against the thread group count, so a wave of 60
// macroblocks cost what a wave of 1 macroblock cost.
//
// rwProgress[mby + 1] is how many macroblocks of row mby are filtered and visible to other groups.
// rwProgress[0] stands for the row above row 0 and the host presets it to widthMb, so row 0 never
// waits and the wait needs no special case. The device memory barrier before the filtering is the
// acquire that pairs with the release barrier after it: without the first one a group could read a
// stale cache line of the row above, and without the second one it could publish a count for samples
// that are still in its own cache.
//
// Forward progress rests on all heightMb groups being resident at once: a group that spins while the
// group above it has not been scheduled would never finish. That is 68 groups at 1080p and 256 at the
// 4096 line bound of IsCodableFrameSize, each one wave of 32 threads holding 2.75 KB of groupshared
// memory, which every part this encoder runs on holds many times over. BC250_MFT_DEBLOCK=wavefront is
// the way out if a scheduler ever disagrees.
[numthreads(32, 1, 1)]
void CSDeblockRows(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex)
{
    const uint mby = gid.x;
    if (mby >= gHeightMb) {
        return;
    }
    for (uint mbx = 0; mbx < gWidthMb; ++mbx) {
        const uint need = min(mbx + 2u, gWidthMb);
        // The wait, and the two things about it that are not free choices.
        //
        // It is an atomic, not a Load. A plain Load of this globallycoherent buffer read a stale
        // value: with the Load form, 1080p produced 0 of 60 pictures bit exact while the same build
        // with BC250_MFT_DEBLOCK=wavefront produced 60 of 60, and the first differing sample was
        // always row 13 of a macroblock in column 0, which is the sample the next row's top edge
        // rewrites. The pattern fits a cache line that still held the previous picture's counters, so
        // the first Load answered "the row above is finished" and no group ever waited. An
        // InterlockedAdd of zero returns the same number and is performed where the counters live, and
        // with it the same cases are 60 of 60 bit exact. Reading counters with an atomic costs about
        // 0.7 ms per 1080p picture against the broken Load form, which is the price of the guarantee.
        //
        // Every lane waits, not lane 0 followed by a group sync. fxc 10.1 removes a thread sync that
        // follows a loop gated on the thread index, silently and at every optimisation level: with
        // `if (tid == 0) { spin }` then GroupMemoryBarrierWithGroupSync(), the generated assembly held
        // no sync at that point at all and thirty-one lanes read the row above without waiting. It
        // keeps a plain DeviceMemoryBarrier() there, and it keeps the release barrier below, so those
        // two are the ones this code is allowed to rely on. The assembly is worth checking after any
        // change here: fxc /Fc and look for sync_uglobal and sync_uglobal_g_t.
        //
        // [allow_uav_condition] is required: without it fxc refuses a loop whose exit condition reads
        // an unordered access view.
        uint seen = 0u;
        [allow_uav_condition]
        do {
            rwProgress.InterlockedAdd(mby * 4u, 0u, seen);
        } while (seen < need);
        // The acquire: the samples of the row above are read after the counter that announced them.
        DeviceMemoryBarrier();
        DeblockMacroblock(mbx, mby, tid);
        // The release: every lane's stores are complete and globally visible, and every lane has
        // finished reading the groupshared window, before lane 0 publishes the count.
        AllMemoryBarrierWithGroupSync();
        if (tid == 0u) {
            rwProgress.Store((mby + 1u) * 4u, mbx + 1u);
        }
    }
}
