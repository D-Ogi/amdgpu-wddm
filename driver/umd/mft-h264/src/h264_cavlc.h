// CAVLC residual coding and the macroblock layer (ITU-T Rec. H.264 clauses 7.3.5 and 9.2).
//
// This is the serial half of the encoder and the reason the split is where it is: every symbol here
// depends on the symbol before it and on the non-zero counts of the neighbouring blocks, so there is
// nothing to vectorise. See ../../BOOTSTRAP.md section 2.2 for the measured share of frame time this
// costs on the target silicon.

#pragma once
#include <stdint.h>
#include <vector>
#include "bitwriter.h"
#include "mb_layout.h"

namespace bc250h264 {

// Writes one picture's worth of macroblock data into a BitWriter, keeping the CAVLC neighbour context
// and, for P pictures, the motion vector predictors and mb_skip_run.
class SliceWriter {
public:
    void Begin(BitWriter* bw, uint32_t widthMb, uint32_t heightMb, bool pSlice);

    // Appends one macroblock in raster order. Macroblocks must be presented in raster order and all
    // of them exactly once. For a P picture the writer decides by itself whether the macroblock can
    // be coded as P_Skip: that needs the skip motion vector predictor, which only the writer knows.
    void WriteMb(uint32_t mbx, uint32_t mby, const MbInfo& mb, const uint32_t* mbLevels);

    // Flushes a trailing run of skipped macroblocks. Must be called once after the last WriteMb.
    void End();

    uint32_t SkippedMbs() const { return m_skippedTotal; }

private:
    struct MvEntry { int16_t x; int16_t y; uint8_t inter; uint8_t pad; };

    void WriteResidualBlock(const int32_t* scan, uint32_t maxNumCoeff, int32_t nC,
                            uint32_t* outTotalCoeff);
    int32_t NcLuma(uint32_t gx, uint32_t gy) const;
    int32_t NcChroma(uint32_t comp, uint32_t gx, uint32_t gy) const;
    void SkipMvPred(uint32_t mbx, uint32_t mby, int32_t* px, int32_t* py) const;
    void MvPred(uint32_t mbx, uint32_t mby, int32_t* px, int32_t* py) const;
    const MvEntry* MbAt(int32_t mbx, int32_t mby) const;

    BitWriter* m_bw = nullptr;
    uint32_t m_widthMb = 0;
    uint32_t m_heightMb = 0;
    bool m_pSlice = false;
    uint32_t m_skipRun = 0;
    uint32_t m_skippedTotal = 0;
    std::vector<uint8_t> m_nnzY;        // (widthMb*4) x (heightMb*4)
    std::vector<uint8_t> m_nnzC[2];     // (widthMb*2) x (heightMb*2)
    std::vector<MvEntry> m_mv;          // widthMb x heightMb
};

} // namespace bc250h264
