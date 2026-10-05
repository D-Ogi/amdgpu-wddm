// H.264 variable-length code tables and normative constant tables (ITU-T Rec. H.264, clauses 8 and 9).
//
// Every table here is a table of the standard. None is copied from another implementation; see
// ../PROVENANCE.md. The build runs structural checks over all of them (prefix-freeness, Kraft sum,
// per-entry round trip) in tests/mfthost.cpp --selftest, because a single transposed digit in a VLC
// table produces a bitstream that no decoder can parse and the failure is otherwise hard to localise.

#pragma once
#include <stdint.h>

namespace bc250h264 {

// ---------------------------------------------------------------------------------------------------
// clause 9.2.1, Table 9-5: coeff_token.
// Index: table 0..3 selected by nC (0: 0<=nC<2, 1: 2<=nC<4, 2: 4<=nC<8, 3: nC>=8), then
// trailingOnes + 4*totalCoeff. A length of 0 marks a combination that cannot occur.
// ---------------------------------------------------------------------------------------------------
extern const uint8_t kCoeffTokenLen[4][4 * 17];
extern const uint8_t kCoeffTokenBits[4][4 * 17];

// Chroma DC, ChromaArrayType == 1 (4:2:0), nC == -1. Table 9-5 last column group.
// Index: trailingOnes + 4*totalCoeff, totalCoeff 0..4.
extern const uint8_t kCoeffTokenChromaDcLen[4 * 5];
extern const uint8_t kCoeffTokenChromaDcBits[4 * 5];

// ---------------------------------------------------------------------------------------------------
// clause 9.2.3, Tables 9-7 and 9-8: total_zeros for 4x4 blocks (maxNumCoeff 15 or 16).
// Index: [tzVlcIndex-1][total_zeros], tzVlcIndex == totalCoeff, 1..15.
// ---------------------------------------------------------------------------------------------------
extern const uint8_t kTotalZerosLen[15][16];
extern const uint8_t kTotalZerosBits[15][16];

// Table 9-9 (a): total_zeros for chroma DC 2x2 (maxNumCoeff 4). Index: [totalCoeff-1][total_zeros].
extern const uint8_t kTotalZerosChromaDcLen[3][4];
extern const uint8_t kTotalZerosChromaDcBits[3][4];

// ---------------------------------------------------------------------------------------------------
// clause 9.2.4, Table 9-10: run_before. Index: [min(zerosLeft,7)-1][run_before].
// ---------------------------------------------------------------------------------------------------
extern const uint8_t kRunBeforeLen[7][16];
extern const uint8_t kRunBeforeBits[7][16];

// ---------------------------------------------------------------------------------------------------
// clause 9.1.2, Table 9-4 (a): coded_block_pattern mapping for ChromaArrayType 1 or 2.
// Forward direction (codeNum -> CodedBlockPattern); the encoder uses the inverse built at runtime.
// ---------------------------------------------------------------------------------------------------
extern const uint8_t kCbpIntraFromCodeNum[48];
extern const uint8_t kCbpInterFromCodeNum[48];

// ---------------------------------------------------------------------------------------------------
// clause 8.6.1, Table 8-15: QPc from qPi (only the qPi >= 30 part varies).
// Index: qPi - 30, for qPi in 30..51.
// ---------------------------------------------------------------------------------------------------
extern const uint8_t kChromaQpFromQpi30[22];

// clause 8.5.9, Table 8-14 normAdjust4x4: [qp%6][class], class 0 = (i,j) both even,
// 1 = both odd, 2 = otherwise. LevelScale4x4 with a flat scaling list is 16 * normAdjust4x4.
extern const uint8_t kNormAdjust4x4[6][3];

// Forward quantisation multipliers, 2^15 / normAdjust rounded, the usual encoder-side companion of
// kNormAdjust4x4. Not normative: the standard fixes dequantisation only. Same index scheme.
extern const uint16_t kQuantCoef4x4[6][3];

// clause 8.5.6: the 4x4 zig-zag scan. kZigZag4x4[scanPos] = raster index inside the 4x4 block.
extern const uint8_t kZigZag4x4[16];

// clause 6.4.3: inverse 4x4 luma block scan. kBlk4x4X/Y[blkIdx] = position in 4x4 units inside the MB.
extern const uint8_t kBlk4x4X[16];
extern const uint8_t kBlk4x4Y[16];

// Runtime-built inverse of Table 9-4. Call once (idempotent, not thread safe before first use).
void InitDerivedTables();
// codeNum for a CodedBlockPattern value, intra (Intra_16x16 uses the Intra_4x4 column) or inter.
uint32_t CbpToCodeNum(uint32_t cbp, bool intra);

} // namespace bc250h264
