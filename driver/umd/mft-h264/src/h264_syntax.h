// Sequence, picture and slice header syntax for H.264 Constrained Baseline (ITU-T Rec. H.264 7.3.2).
//
// Profile choices, all recorded here so the bitstream stays legible:
//   profile_idc 66 with constraint_set0_flag and constraint_set1_flag set -> Constrained Baseline.
//   entropy_coding_mode_flag 0  (CAVLC; CABAC is future work, see ../../BOOTSTRAP.md 2.2)
//   frame_mbs_only_flag 1       (progressive only)
//   pic_order_cnt_type 2        (decoding order == output order; legal because every picture we emit
//                                is a reference picture, so the clause 8.2.1.3 restriction holds)
//   max_num_ref_frames 1        (one reference, P pictures only)
//   num_slice_groups_minus1 0, no FMO, no ASO, no redundant pictures
//   direct_8x8_inference / weighted prediction: not applicable to Baseline P
//
// Non-MB-aligned sizes are coded by padding to a macroblock multiple and cropping in the SPS.

#pragma once
#include <stdint.h>
#include <vector>
#include "bitwriter.h"

namespace bc250h264 {

enum NalUnitType : uint32_t {
    kNalSliceNonIdr = 1,
    kNalSliceIdr = 5,
    kNalSps = 7,
    kNalPps = 8,
    kNalAud = 9,
};

struct SequenceParams {
    uint32_t widthMb = 0;        // picture width in macroblocks
    uint32_t heightMb = 0;       // picture height in macroblocks
    uint32_t cropRight = 0;      // in luma samples, must be even (CropUnitX == 2 for 4:2:0)
    uint32_t cropBottom = 0;     // in luma samples, must be even (CropUnitY == 2 when frame_mbs_only)
    uint32_t levelIdc = 31;
    uint32_t log2MaxFrameNumMinus4 = 4;   // MaxFrameNum == 256
    uint32_t fpsNum = 30;        // frames per second numerator
    uint32_t fpsDen = 1;
    uint32_t maxBitRate = 0;     // 0 omits the HRD; we never write an HRD
    // vui_parameters colour description, clause E.2.1 code points. 2 is "unspecified".
    uint32_t colourPrimaries = 1;
    uint32_t transferCharacteristics = 1;
    uint32_t matrixCoefficients = 1;
    bool fullRange = false;      // video_full_range_flag
};

struct PictureParams {
    int32_t picInitQp = 26;
    int32_t chromaQpIndexOffset = 0;
    bool deblockingFilterControlPresent = true;
};

struct SliceParams {
    bool idr = false;
    bool pSlice = false;         // false: I slice
    uint32_t frameNum = 0;
    uint32_t idrPicId = 0;
    int32_t sliceQp = 26;
    uint32_t disableDeblockingFilterIdc = 1;   // 1 == filter off for this slice
    int32_t alphaC0OffsetDiv2 = 0;
    int32_t betaOffsetDiv2 = 0;
};

// Writes seq_parameter_set_rbsp into bw (without rbsp_trailing_bits; the caller adds them).
void WriteSps(BitWriter& bw, const SequenceParams& sps);
// Writes pic_parameter_set_rbsp.
void WritePps(BitWriter& bw, const SequenceParams& sps, const PictureParams& pps);
// Writes slice_header. The macroblock data follows in the same BitWriter.
void WriteSliceHeader(BitWriter& bw, const SequenceParams& sps, const PictureParams& pps,
                      const SliceParams& slice);

// Convenience: the two parameter set NAL units as an Annex B byte sequence. This is what goes into
// MF_MT_MPEG_SEQUENCE_HEADER on the output media type.
void BuildParameterSetNals(std::vector<uint8_t>& out, const SequenceParams& sps,
                           const PictureParams& pps);

} // namespace bc250h264
