// SPDX-License-Identifier: MIT
// The GPU/CPU hand-off layout: what the compute shaders write and the CPU entropy coder reads.
//
// This is the whole interface between the two halves of the encoder. Mirrored, word for word, in
// shaders/h264_common.hlsli; the two must be changed together and the sizes are asserted below.
//
// Per macroblock the shaders produce
//   - MbInfo  (16 uint32): the mode decision, the motion vector and the non-zero counts the CAVLC
//               context needs, plus a distortion measure for rate control;
//   - levels  (204 uint32): the quantised transform coefficient levels, two int16 per word, in
//               zig-zag scan order within each 4x4 block.
// Nothing else crosses. In particular the reconstruction never leaves the GPU during encoding.

#pragma once
#include <stdint.h>

namespace bc250h264 {

// --- levels buffer, in uint32 words per macroblock -------------------------------------------------
enum : uint32_t {
    kLevelsWordsPerMb = 204,
    kLevelsOffLumaDc = 0,       // 8 words: 16 coefficients of the Intra_16x16 luma DC block
    kLevelsOffLumaAc = 8,       // 128 words: 16 blocks x 8 words, coefficient 0 unused for I_16x16
    kLevelsOffChromaDc = 136,   // 4 words: 2 components x 4 coefficients
    kLevelsOffChromaAc = 140,   // 64 words: 2 components x 4 blocks x 8 words, coefficient 0 unused
};

// --- MbInfo ----------------------------------------------------------------------------------------
enum : uint32_t {
    kMbInfoWords = 16,
    kMbFlagIntra16x16 = 1u << 0,       // the macroblock is I_16x16 (always set in an I slice)
    kMbFlagPredModeShift = 1,          // 2 bits: Intra_16x16 prediction mode 0..3
    kMbFlagChromaModeShift = 3,        // 2 bits: intra chroma prediction mode 0..3
};

struct MbInfo {
    uint32_t flags;
    uint32_t cbpLuma;        // 0..15; for I_16x16 only 0 or 15 occur
    uint32_t cbpChroma;      // 0 none, 1 DC only, 2 DC and AC
    int32_t mvx;             // quarter luma sample units
    int32_t mvy;
    uint32_t nnzLuma[4];     // 16 luma 4x4 blocks, 8 bits each, block index = raster 4x4 in the MB
    uint32_t nnzChroma[2];   // 8 chroma 4x4 blocks, 8 bits each: component * 4 + blockInComponent
    uint32_t nnzDc;          // bits 0..7 luma DC, 8..15 Cb DC, 16..23 Cr DC
    uint32_t cost;           // SAD (inter) or intra prediction SAD, for rate control
    uint32_t reserved[3];
};
static_assert(sizeof(MbInfo) == kMbInfoWords * 4, "MbInfo must match the shader layout");

inline uint32_t MbNnzLuma(const MbInfo& mb, uint32_t rasterBlk)
{
    return (mb.nnzLuma[rasterBlk >> 2] >> ((rasterBlk & 3u) * 8u)) & 0xFFu;
}
inline uint32_t MbNnzChroma(const MbInfo& mb, uint32_t comp, uint32_t blkInComp)
{
    const uint32_t idx = comp * 4u + blkInComp;
    return (mb.nnzChroma[idx >> 2] >> ((idx & 3u) * 8u)) & 0xFFu;
}
inline uint32_t MbNnzDc(const MbInfo& mb, uint32_t which)   // 0 luma, 1 Cb, 2 Cr
{
    return (mb.nnzDc >> (which * 8u)) & 0xFFu;
}
inline uint32_t MbPredMode(const MbInfo& mb) { return (mb.flags >> kMbFlagPredModeShift) & 3u; }
inline uint32_t MbChromaMode(const MbInfo& mb) { return (mb.flags >> kMbFlagChromaModeShift) & 3u; }
inline bool MbIsIntra(const MbInfo& mb) { return (mb.flags & kMbFlagIntra16x16) != 0; }

// Unpacks one int16 level out of the packed levels buffer.
inline int32_t LevelAt(const uint32_t* mbLevels, uint32_t wordBase, uint32_t coeff)
{
    const uint32_t w = mbLevels[wordBase + (coeff >> 1)];
    const uint16_t raw = static_cast<uint16_t>((coeff & 1u) ? (w >> 16) : (w & 0xFFFFu));
    return static_cast<int32_t>(static_cast<int16_t>(raw));
}

} // namespace bc250h264
