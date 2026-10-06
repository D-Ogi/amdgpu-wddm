// SPDX-License-Identifier: MIT
#include "h264_syntax.h"

namespace bc250h264 {

void WriteSps(BitWriter& bw, const SequenceParams& sps)
{
    // seq_parameter_set_data, clause 7.3.2.1.1, in syntax order.
    bw.U(8, 66);              // profile_idc: Baseline
    bw.Flag(true);            // constraint_set0_flag
    bw.Flag(true);            // constraint_set1_flag -> together: Constrained Baseline
    bw.Flag(false);           // constraint_set2_flag
    bw.Flag(false);           // constraint_set3_flag
    bw.Flag(false);           // constraint_set4_flag
    bw.Flag(false);           // constraint_set5_flag
    bw.U(2, 0);               // reserved_zero_2bits
    bw.U(8, sps.levelIdc);    // level_idc
    bw.UE(0);                 // seq_parameter_set_id
    bw.UE(sps.log2MaxFrameNumMinus4);
    bw.UE(2);                 // pic_order_cnt_type
    bw.UE(1);                 // max_num_ref_frames
    bw.Flag(false);           // gaps_in_frame_num_value_allowed_flag
    bw.UE(sps.widthMb - 1);   // pic_width_in_mbs_minus1
    bw.UE(sps.heightMb - 1);  // pic_height_in_map_units_minus1
    bw.Flag(true);            // frame_mbs_only_flag
    bw.Flag(true);            // direct_8x8_inference_flag (unconditional; must be 1 here)

    const bool crop = (sps.cropRight != 0) || (sps.cropBottom != 0);
    bw.Flag(crop);            // frame_cropping_flag
    if (crop) {
        bw.UE(0);                      // frame_crop_left_offset
        bw.UE(sps.cropRight / 2);      // frame_crop_right_offset, CropUnitX == SubWidthC == 2
        bw.UE(0);                      // frame_crop_top_offset
        bw.UE(sps.cropBottom / 2);     // frame_crop_bottom_offset, CropUnitY == 2 (frame_mbs_only)
    }

    // vui_parameters, clause E.1.1: only what a player and a muxer actually need.
    bw.Flag(true);            // vui_parameters_present_flag
    bw.Flag(false);           // aspect_ratio_info_present_flag
    bw.Flag(false);           // overscan_info_present_flag
    // The colour description is what the client said its samples are (the transform reads
    // MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION, MF_MT_YUV_MATRIX and
    // MF_MT_VIDEO_NOMINAL_RANGE from the input type), not a fixed claim: nothing in this encoder
    // converts between colour spaces, so writing BT.709 over BT.601 samples would make every player
    // apply the wrong matrix.
    bw.Flag(true);                           // video_signal_type_present_flag
    bw.U(3, 5);                              //   video_format: Unspecified
    bw.Flag(sps.fullRange);                  //   video_full_range_flag
    bw.Flag(true);                           //   colour_description_present_flag
    bw.U(8, sps.colourPrimaries);            //     colour_primaries
    bw.U(8, sps.transferCharacteristics);    //     transfer_characteristics
    bw.U(8, sps.matrixCoefficients);         //     matrix_coefficients
    bw.Flag(false);           // chroma_loc_info_present_flag
    bw.Flag(true);            // timing_info_present_flag
    bw.U(32, sps.fpsDen);     //   num_units_in_tick
    bw.U(32, sps.fpsNum * 2); //   time_scale: two field ticks per frame
    bw.Flag(true);            //   fixed_frame_rate_flag
    bw.Flag(false);           // nal_hrd_parameters_present_flag
    bw.Flag(false);           // vcl_hrd_parameters_present_flag
    bw.Flag(false);           // pic_struct_present_flag
    bw.Flag(false);           // bitstream_restriction_flag
}

void WritePps(BitWriter& bw, const SequenceParams& sps, const PictureParams& pps)
{
    (void)sps;
    bw.UE(0);                 // pic_parameter_set_id
    bw.UE(0);                 // seq_parameter_set_id
    bw.Flag(false);           // entropy_coding_mode_flag: CAVLC
    bw.Flag(false);           // bottom_field_pic_order_in_frame_present_flag
    bw.UE(0);                 // num_slice_groups_minus1
    bw.UE(0);                 // num_ref_idx_l0_default_active_minus1
    bw.UE(0);                 // num_ref_idx_l1_default_active_minus1
    bw.Flag(false);           // weighted_pred_flag
    bw.U(2, 0);               // weighted_bipred_idc
    bw.SE(pps.picInitQp - 26);
    bw.SE(0);                 // pic_init_qs_minus26
    bw.SE(pps.chromaQpIndexOffset);
    bw.Flag(pps.deblockingFilterControlPresent);
    bw.Flag(false);           // constrained_intra_pred_flag
    bw.Flag(false);           // redundant_pic_cnt_present_flag
}

void WriteSliceHeader(BitWriter& bw, const SequenceParams& sps, const PictureParams& pps,
                      const SliceParams& slice)
{
    bw.UE(0);                                       // first_mb_in_slice
    bw.UE(slice.pSlice ? 5u : 7u);                  // slice_type: P (all) or I (all)
    bw.UE(0);                                       // pic_parameter_set_id
    bw.U(sps.log2MaxFrameNumMinus4 + 4, slice.frameNum);
    if (slice.idr) {
        bw.UE(slice.idrPicId);
    }
    // pic_order_cnt_type == 2 and bottom_field_pic_order_in_frame_present_flag == 0: nothing here.
    // redundant_pic_cnt_present_flag == 0: no redundant_pic_cnt.
    if (slice.pSlice) {
        bw.Flag(false);                             // num_ref_idx_active_override_flag
        bw.Flag(false);                             // ref_pic_list_modification_flag_l0
    }
    // nal_ref_idc != 0 for every picture we emit, so dec_ref_pic_marking is present.
    if (slice.idr) {
        bw.Flag(false);                             // no_output_of_prior_pics_flag
        bw.Flag(false);                             // long_term_reference_flag
    } else {
        bw.Flag(false);                             // adaptive_ref_pic_marking_mode_flag
    }
    // entropy_coding_mode_flag == 0: no cabac_init_idc.
    bw.SE(slice.sliceQp - pps.picInitQp);           // slice_qp_delta
    if (pps.deblockingFilterControlPresent) {
        bw.UE(slice.disableDeblockingFilterIdc);
        if (slice.disableDeblockingFilterIdc != 1) {
            bw.SE(slice.alphaC0OffsetDiv2);
            bw.SE(slice.betaOffsetDiv2);
        }
    }
}

void BuildParameterSetNals(std::vector<uint8_t>& out, const SequenceParams& sps,
                           const PictureParams& pps)
{
    BitWriter bw;
    bw.Clear();
    WriteSps(bw, sps);
    bw.RbspTrailingBits();
    EmitNal(out, 3, kNalSps, bw.Rbsp());

    bw.Clear();
    WritePps(bw, sps, pps);
    bw.RbspTrailingBits();
    EmitNal(out, 3, kNalPps, bw.Rbsp());
}

} // namespace bc250h264
