// SPDX-License-Identifier: MIT
#include "h264_cavlc.h"
#include "h264_tables.h"

namespace bc250h264 {

namespace {

inline int32_t Median3(int32_t a, int32_t b, int32_t c)
{
    return a + b + c - (a < b ? (a < c ? a : c) : (b < c ? b : c))
                     - (a > b ? (a > c ? a : c) : (b > c ? b : c));
}

void WriteCoeffToken(BitWriter& bw, int32_t nC, uint32_t totalCoeff, uint32_t trailingOnes)
{
    const uint32_t idx = trailingOnes + 4u * totalCoeff;
    uint32_t len = 0;
    uint32_t bits = 0;
    if (nC < 0) {
        len = kCoeffTokenChromaDcLen[idx];
        bits = kCoeffTokenChromaDcBits[idx];
    } else {
        const uint32_t t = (nC < 2) ? 0u : (nC < 4) ? 1u : (nC < 8) ? 2u : 3u;
        len = kCoeffTokenLen[t][idx];
        bits = kCoeffTokenBits[t][idx];
    }
    bw.U(len, bits);
}

// clause 9.2.2 inverse: emits level_prefix and level_suffix for one non-trailing-one level and
// advances suffixLength exactly as the decoder does.
void WriteLevel(BitWriter& bw, int32_t level, uint32_t* suffixLength, bool firstAfterTrailingOnes)
{
    int32_t levelCode = (level > 0) ? (2 * level - 2) : (-2 * level - 1);
    if (firstAfterTrailingOnes) {
        levelCode -= 2;
    }
    const uint32_t sl = *suffixLength;
    if (sl == 0) {
        if (levelCode < 14) {
            bw.U(static_cast<uint32_t>(levelCode) + 1u, 1);          // level_prefix == levelCode
        } else if (levelCode < 30) {
            bw.U(15, 1);                                             // level_prefix == 14
            bw.U(4, static_cast<uint32_t>(levelCode - 14));
        } else {
            bw.U(16, 1);                                             // level_prefix == 15
            bw.U(12, static_cast<uint32_t>(levelCode - 30));
        }
    } else {
        const uint32_t prefix = static_cast<uint32_t>(levelCode) >> sl;
        if (prefix < 15) {
            bw.U(prefix + 1u, 1);
            bw.U(sl, static_cast<uint32_t>(levelCode) & ((1u << sl) - 1u));
        } else {
            bw.U(16, 1);
            bw.U(12, static_cast<uint32_t>(levelCode) - (15u << sl));
        }
    }
    if (*suffixLength == 0) {
        *suffixLength = 1;
    }
    const int32_t mag = level < 0 ? -level : level;
    if (*suffixLength < 6 && mag > (3 << (*suffixLength - 1))) {
        (*suffixLength)++;
    }
}

} // namespace

void SliceWriter::Begin(BitWriter* bw, uint32_t widthMb, uint32_t heightMb, bool pSlice)
{
    InitDerivedTables();
    m_bw = bw;
    m_widthMb = widthMb;
    m_heightMb = heightMb;
    m_pSlice = pSlice;
    m_skipRun = 0;
    m_skippedTotal = 0;
    m_nnzY.assign(static_cast<size_t>(widthMb) * 4u * heightMb * 4u, 0);
    for (int c = 0; c < 2; ++c) {
        m_nnzC[c].assign(static_cast<size_t>(widthMb) * 2u * heightMb * 2u, 0);
    }
    m_mv.assign(static_cast<size_t>(widthMb) * heightMb, MvEntry{ 0, 0, 0, 0 });
}

const SliceWriter::MvEntry* SliceWriter::MbAt(int32_t mbx, int32_t mby) const
{
    if (mbx < 0 || mby < 0 || mbx >= static_cast<int32_t>(m_widthMb) ||
        mby >= static_cast<int32_t>(m_heightMb)) {
        return nullptr;
    }
    return &m_mv[static_cast<size_t>(mby) * m_widthMb + mbx];
}

int32_t SliceWriter::NcLuma(uint32_t gx, uint32_t gy) const
{
    const uint32_t stride = m_widthMb * 4u;
    const bool haveA = gx > 0;
    const bool haveB = gy > 0;
    const int32_t nA = haveA ? m_nnzY[static_cast<size_t>(gy) * stride + (gx - 1)] : 0;
    const int32_t nB = haveB ? m_nnzY[static_cast<size_t>(gy - 1) * stride + gx] : 0;
    if (haveA && haveB) {
        return (nA + nB + 1) >> 1;
    }
    if (haveA) {
        return nA;
    }
    if (haveB) {
        return nB;
    }
    return 0;
}

int32_t SliceWriter::NcChroma(uint32_t comp, uint32_t gx, uint32_t gy) const
{
    const uint32_t stride = m_widthMb * 2u;
    const bool haveA = gx > 0;
    const bool haveB = gy > 0;
    const int32_t nA = haveA ? m_nnzC[comp][static_cast<size_t>(gy) * stride + (gx - 1)] : 0;
    const int32_t nB = haveB ? m_nnzC[comp][static_cast<size_t>(gy - 1) * stride + gx] : 0;
    if (haveA && haveB) {
        return (nA + nB + 1) >> 1;
    }
    if (haveA) {
        return nA;
    }
    if (haveB) {
        return nB;
    }
    return 0;
}

// clause 8.4.1.3 for a 16x16 partition with refIdxL0 == 0. All macroblocks in a P picture of this
// encoder are inter with refIdxL0 == 0, so "available and inter" is the only distinction that matters.
void SliceWriter::MvPred(uint32_t mbx, uint32_t mby, int32_t* px, int32_t* py) const
{
    const MvEntry* a = MbAt(static_cast<int32_t>(mbx) - 1, static_cast<int32_t>(mby));
    const MvEntry* b = MbAt(static_cast<int32_t>(mbx), static_cast<int32_t>(mby) - 1);
    const MvEntry* c = MbAt(static_cast<int32_t>(mbx) + 1, static_cast<int32_t>(mby) - 1);
    if (c == nullptr) {
        c = MbAt(static_cast<int32_t>(mbx) - 1, static_cast<int32_t>(mby) - 1);   // mbAddrD
    }
    // "If both mbAddrB and mbAddrC are not available and mbAddrA is available, B and C take A."
    if (b == nullptr && c == nullptr && a != nullptr) {
        b = a;
        c = a;
    }
    const int32_t ax = a ? a->x : 0, ay = a ? a->y : 0;
    const int32_t bx = b ? b->x : 0, by = b ? b->y : 0;
    const int32_t cx = c ? c->x : 0, cy = c ? c->y : 0;
    const int32_t refA = (a && a->inter) ? 0 : -1;
    const int32_t refB = (b && b->inter) ? 0 : -1;
    const int32_t refC = (c && c->inter) ? 0 : -1;
    const int32_t matches = (refA == 0 ? 1 : 0) + (refB == 0 ? 1 : 0) + (refC == 0 ? 1 : 0);
    if (matches == 1) {
        if (refA == 0) { *px = ax; *py = ay; }
        else if (refB == 0) { *px = bx; *py = by; }
        else { *px = cx; *py = cy; }
        return;
    }
    *px = Median3(ax, bx, cx);
    *py = Median3(ay, by, cy);
}

// clause 8.4.1.1: the P_Skip motion vector.
void SliceWriter::SkipMvPred(uint32_t mbx, uint32_t mby, int32_t* px, int32_t* py) const
{
    const MvEntry* a = MbAt(static_cast<int32_t>(mbx) - 1, static_cast<int32_t>(mby));
    const MvEntry* b = MbAt(static_cast<int32_t>(mbx), static_cast<int32_t>(mby) - 1);
    if (a == nullptr || b == nullptr ||
        (a->inter && a->x == 0 && a->y == 0) ||
        (b->inter && b->x == 0 && b->y == 0)) {
        *px = 0;
        *py = 0;
        return;
    }
    MvPred(mbx, mby, px, py);
}

void SliceWriter::WriteResidualBlock(const int32_t* scan, uint32_t maxNumCoeff, int32_t nC,
                                     uint32_t* outTotalCoeff)
{
    uint32_t pos[16];
    uint32_t totalCoeff = 0;
    for (uint32_t i = 0; i < maxNumCoeff; ++i) {
        if (scan[i] != 0) {
            pos[totalCoeff++] = i;
        }
    }
    *outTotalCoeff = totalCoeff;

    uint32_t trailingOnes = 0;
    while (trailingOnes < 3 && trailingOnes < totalCoeff) {
        const int32_t v = scan[pos[totalCoeff - 1 - trailingOnes]];
        if (v != 1 && v != -1) {
            break;
        }
        ++trailingOnes;
    }

    WriteCoeffToken(*m_bw, nC, totalCoeff, trailingOnes);
    if (totalCoeff == 0) {
        return;
    }

    uint32_t suffixLength = (totalCoeff > 10 && trailingOnes < 3) ? 1u : 0u;
    for (uint32_t j = 0; j < totalCoeff; ++j) {
        const int32_t level = scan[pos[totalCoeff - 1 - j]];
        if (j < trailingOnes) {
            m_bw->U(1, level < 0 ? 1u : 0u);                 // trailing_ones_sign_flag
        } else {
            WriteLevel(*m_bw, level, &suffixLength, j == trailingOnes && trailingOnes < 3);
        }
    }

    const uint32_t highest = pos[totalCoeff - 1];
    const uint32_t totalZeros = highest + 1u - totalCoeff;
    if (totalCoeff < maxNumCoeff) {
        if (maxNumCoeff == 4) {
            m_bw->U(kTotalZerosChromaDcLen[totalCoeff - 1][totalZeros],
                    kTotalZerosChromaDcBits[totalCoeff - 1][totalZeros]);
        } else {
            m_bw->U(kTotalZerosLen[totalCoeff - 1][totalZeros],
                    kTotalZerosBits[totalCoeff - 1][totalZeros]);
        }
    }

    uint32_t zerosLeft = totalZeros;
    for (uint32_t j = 0; j + 1 < totalCoeff; ++j) {
        const uint32_t hi = pos[totalCoeff - 1 - j];
        const uint32_t lo = pos[totalCoeff - 2 - j];
        const uint32_t run = hi - lo - 1u;
        if (zerosLeft > 0) {
            const uint32_t row = (zerosLeft > 7 ? 7u : zerosLeft) - 1u;
            m_bw->U(kRunBeforeLen[row][run], kRunBeforeBits[row][run]);
            zerosLeft -= run;
        }
    }
}

void SliceWriter::WriteMb(uint32_t mbx, uint32_t mby, const MbInfo& mb, const uint32_t* mbLevels)
{
    const uint32_t lumaStride = m_widthMb * 4u;
    const uint32_t chromaStride = m_widthMb * 2u;
    const bool intra = MbIsIntra(mb);

    // ---- P_Skip -----------------------------------------------------------------------------------
    if (m_pSlice && !intra && mb.cbpLuma == 0 && mb.cbpChroma == 0) {
        int32_t spx = 0, spy = 0;
        SkipMvPred(mbx, mby, &spx, &spy);
        if (spx == mb.mvx && spy == mb.mvy) {
            ++m_skipRun;
            ++m_skippedTotal;
            m_mv[static_cast<size_t>(mby) * m_widthMb + mbx] =
                MvEntry{ static_cast<int16_t>(spx), static_cast<int16_t>(spy), 1, 0 };
            for (uint32_t by = 0; by < 4; ++by) {
                for (uint32_t bx = 0; bx < 4; ++bx) {
                    m_nnzY[static_cast<size_t>(mby * 4 + by) * lumaStride + mbx * 4 + bx] = 0;
                }
            }
            for (uint32_t c = 0; c < 2; ++c) {
                for (uint32_t by = 0; by < 2; ++by) {
                    for (uint32_t bx = 0; bx < 2; ++bx) {
                        m_nnzC[c][static_cast<size_t>(mby * 2 + by) * chromaStride + mbx * 2 + bx] = 0;
                    }
                }
            }
            return;
        }
    }

    if (m_pSlice) {
        m_bw->UE(m_skipRun);
        m_skipRun = 0;
    }

    // ---- mb_type ----------------------------------------------------------------------------------
    const uint32_t cbpLuma = mb.cbpLuma;
    const uint32_t cbpChroma = mb.cbpChroma;
    if (intra) {
        // I_16x16_<predMode>_<cbpChroma>_<cbpLuma != 0>, clause 7.4.5 Table 7-11. In a P slice the
        // intra types are offset by 5 (the number of P types), Table 7-13.
        const uint32_t base = 1u + MbPredMode(mb) + 4u * cbpChroma + 12u * (cbpLuma != 0 ? 1u : 0u);
        m_bw->UE(m_pSlice ? base + 5u : base);
        m_bw->UE(MbChromaMode(mb));                        // intra_chroma_pred_mode
    } else {
        m_bw->UE(0);                                       // P_L0_16x16
        int32_t pmx = 0, pmy = 0;
        MvPred(mbx, mby, &pmx, &pmy);
        m_bw->SE(mb.mvx - pmx);                            // mvd_l0[0][0][0]
        m_bw->SE(mb.mvy - pmy);                            // mvd_l0[0][0][1]
        m_bw->UE(CbpToCodeNum(cbpLuma + 16u * cbpChroma, false));
    }

    // ---- mb_qp_delta ------------------------------------------------------------------------------
    // Rate control is per picture, so the quantiser never changes inside a slice.
    if (intra || cbpLuma != 0 || cbpChroma != 0) {
        m_bw->SE(0);
    }

    // ---- residual ---------------------------------------------------------------------------------
    // Clear this macroblock's non-zero counts first: blocks that the coded block pattern drops are
    // never written below, and their neighbours must see a count of zero, not last picture's value.
    for (uint32_t by = 0; by < 4; ++by) {
        for (uint32_t bx = 0; bx < 4; ++bx) {
            m_nnzY[static_cast<size_t>(mby * 4 + by) * lumaStride + mbx * 4 + bx] = 0;
        }
    }
    for (uint32_t c = 0; c < 2; ++c) {
        for (uint32_t by = 0; by < 2; ++by) {
            for (uint32_t bx = 0; bx < 2; ++bx) {
                m_nnzC[c][static_cast<size_t>(mby * 2 + by) * chromaStride + mbx * 2 + bx] = 0;
            }
        }
    }

    int32_t scan[16];
    uint32_t tc = 0;

    if (intra) {
        for (uint32_t i = 0; i < 16; ++i) {
            scan[i] = LevelAt(mbLevels, kLevelsOffLumaDc, i);
        }
        WriteResidualBlock(scan, 16, NcLuma(mbx * 4u, mby * 4u), &tc);
    }

    if (cbpLuma != 0) {
        for (uint32_t blk = 0; blk < 16; ++blk) {
            const uint32_t bx = kBlk4x4X[blk];
            const uint32_t by = kBlk4x4Y[blk];
            if ((cbpLuma & (1u << (blk >> 2))) == 0) {
                continue;
            }
            const uint32_t raster = by * 4u + bx;
            const uint32_t wordBase = kLevelsOffLumaAc + raster * 8u;
            const uint32_t first = intra ? 1u : 0u;
            const uint32_t count = intra ? 15u : 16u;
            for (uint32_t i = 0; i < count; ++i) {
                scan[i] = LevelAt(mbLevels, wordBase, first + i);
            }
            WriteResidualBlock(scan, count, NcLuma(mbx * 4u + bx, mby * 4u + by), &tc);
            (void)raster;
            m_nnzY[static_cast<size_t>(mby * 4 + by) * lumaStride + mbx * 4 + bx] =
                static_cast<uint8_t>(tc);
        }
    }

    if (cbpChroma != 0) {
        for (uint32_t c = 0; c < 2; ++c) {
            for (uint32_t i = 0; i < 4; ++i) {
                scan[i] = LevelAt(mbLevels, kLevelsOffChromaDc + c * 2u, i);
            }
            WriteResidualBlock(scan, 4, -1, &tc);          // nC == -1 for 4:2:0 chroma DC
        }
    }
    if (cbpChroma == 2) {
        for (uint32_t c = 0; c < 2; ++c) {
            for (uint32_t blk = 0; blk < 4; ++blk) {
                const uint32_t bx = blk & 1u;
                const uint32_t by = blk >> 1;
                const uint32_t wordBase = kLevelsOffChromaAc + (c * 4u + blk) * 8u;
                for (uint32_t i = 0; i < 15; ++i) {
                    scan[i] = LevelAt(mbLevels, wordBase, 1u + i);
                }
                WriteResidualBlock(scan, 15, NcChroma(c, mbx * 2u + bx, mby * 2u + by), &tc);
                m_nnzC[c][static_cast<size_t>(mby * 2 + by) * chromaStride + mbx * 2 + bx] =
                    static_cast<uint8_t>(tc);
            }
        }
    }

    m_mv[static_cast<size_t>(mby) * m_widthMb + mbx] =
        MvEntry{ static_cast<int16_t>(intra ? 0 : mb.mvx), static_cast<int16_t>(intra ? 0 : mb.mvy),
                 static_cast<uint8_t>(intra ? 0 : 1), 0 };
}

void SliceWriter::End()
{
    if (m_pSlice && m_skipRun > 0) {
        m_bw->UE(m_skipRun);
        m_skipRun = 0;
    }
}

} // namespace bc250h264
