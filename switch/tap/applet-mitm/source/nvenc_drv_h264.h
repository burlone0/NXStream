#pragma once

#ifndef NV_NVENC_5_0
#define NV_NVENC_5_0 1
#endif

#ifndef NVENC_DRV_H
#define NVENC_DRV_H

typedef struct
{
  unsigned int luma_log2_weight_denom              : 3;
  unsigned int chroma_log2_weight_denom            : 3;
  unsigned int reversed                            : 26;

  unsigned char luma_weight_l0_flag[16];
  unsigned char chroma_weight_l0_flag[16];

  unsigned char luma_weight_l1_flag[16];
  unsigned char chroma_weight_l1_flag[16];

  short        luma_weight_l0[16];
  short        luma_offset_l0[16];
  short        luma_weight_l1[16];
  short        luma_offset_l1[16];
  short        chroma_weight_l0[2][16];
  short        chroma_offset_l0[2][16];
  short        chroma_weight_l1[2][16];
  short        chroma_offset_l1[2][16];

} nvenc_pred_weight_table_s;

typedef struct
{
  unsigned short frame_width_minus1;
  unsigned short frame_height_minus1;
  unsigned short sfc_pitch;
  unsigned short sfc_pitch_chroma;
  unsigned short sfc_trans_mode;
  unsigned short reserved2;

  unsigned int   luma_top_frm_offset;
  unsigned int   luma_bot_offset;
  unsigned int   chroma_top_frm_offset;
  unsigned int   chroma_bot_offset;
  unsigned int   block_height                       : 7;
  unsigned int   tiled_16x16                        : 1;
  unsigned int   memory_mode                        : 2;
  unsigned int   nv21_enable                        : 1;
  unsigned int   input_bl_mode                      : 2;
  unsigned int   reserved                           : 19;
} nvenc_h264_surface_cfg_s;

typedef struct
{
  unsigned int   profile_idc                        : 8;
  unsigned int   level_idc                          : 8;
  unsigned int   chroma_format_idc                  : 2;
  unsigned int   pic_order_cnt_type                 : 2;
  unsigned int   log2_max_frame_num_minus4          : 4;
  unsigned int   log2_max_pic_order_cnt_lsb_minus4  : 4;
  unsigned int   frame_mbs_only                     : 1;
  unsigned int   stereo_mvc_enable                  : 1;
  unsigned int   separate_colour_plane_flag         : 1;
  unsigned int   lossless_qpprime_flag              : 1;
} nvenc_h264_sps_data_s;

typedef struct
{
  unsigned int   pic_param_set_id                   : 8;
  unsigned int   entropy_coding_mode_flag           : 1;
  unsigned int   num_ref_idx_l0_active_minus1       : 5;
  unsigned int   num_ref_idx_l1_active_minus1       : 5;
  unsigned int   weighted_bipred_idc                : 2;
           int   pic_init_qp_minus26                : 6;
           int   chroma_qp_index_offset             : 5;

           int   second_chroma_qp_index_offset      : 5;
  unsigned int   constrained_intra_pred_flag        : 1;
  unsigned int   deblocking_filter_control_present_flag : 1;
  unsigned int   transform_8x8_mode_flag            : 1;
  unsigned int   pic_order_present_flag             : 1;
  unsigned int   weighted_pred_flag                 : 1;
  unsigned int   reserved0                          : 22;
} nvenc_h264_pps_data_s;

typedef struct
{
  unsigned int   self_temporal_stamp_l0             : 3;
  unsigned int   self_temporal_stamp_l1             : 3;
  unsigned int   self_temporal_explicit             : 1;
  unsigned int   self_temporal_search               : 1;
  unsigned int   self_temporal_refine               : 1;
  unsigned int   self_temporal_enable               : 1;

  unsigned int   coloc_stamp_l0                     : 3;
  unsigned int   coloc_stamp_l1                     : 3;
  unsigned int   coloc_explicit                     : 1;
  unsigned int   coloc_search                       : 1;
  unsigned int   coloc_refine                       : 1;
  unsigned int   coloc_enable                       : 1;

  unsigned int   self_spatial_stamp_l0              : 3;
  unsigned int   self_spatial_stamp_l1              : 3;
  unsigned int   self_spatial_explicit              : 1;
  unsigned int   self_spatial_search                : 1;
  unsigned int   self_spatial_refine                : 1;
  unsigned int   self_spatial_enable                : 1;
  unsigned int   reserved0                          : 2;

  unsigned int   external_stamp_l0_refidx0_stamp    : 3;
  unsigned int   external_stamp_l0_refidx1_stamp    : 3;
  unsigned int   external_stamp_l0_refidx2_stamp    : 3;
  unsigned int   external_stamp_l0_refidx3_stamp    : 3;
  unsigned int   external_stamp_l0_refidx4_stamp    : 3;
  unsigned int   external_stamp_l0_refidx5_stamp    : 3;
  unsigned int   external_stamp_l0_refidx6_stamp    : 3;
  unsigned int   external_stamp_l0_refidx7_stamp    : 3;
  unsigned int   external_stamp_l1_refidx0_stamp    : 3;
  unsigned int   external_stamp_l1_refidx1_stamp    : 3;
  unsigned int   reserved1                          : 2;

  unsigned int   external_stamp_l1_refidx2_stamp    : 3;
  unsigned int   external_stamp_l1_refidx3_stamp    : 3;
  unsigned int   external_stamp_l1_refidx4_stamp    : 3;
  unsigned int   external_stamp_l1_refidx5_stamp    : 3;
  unsigned int   external_stamp_l1_refidx6_stamp    : 3;
  unsigned int   external_stamp_l1_refidx7_stamp    : 3;

  unsigned int   external_explicit                  : 1;
  unsigned int   external_search                    : 1;
  unsigned int   external_refine                    : 1;
  unsigned int   external_enable                    : 1;

  unsigned int   const_mv_stamp_l0                  : 3;
  unsigned int   const_mv_stamp_l1                  : 3;
  unsigned int   const_mv_explicit                  : 1;
  unsigned int   const_mv_search                    : 1;
  unsigned int   const_mv_refine                    : 1;
  unsigned int   const_mv_enable                    : 1;

  unsigned int   stamp_refidx1_stamp    : 3;
  unsigned int   stamp_refidx2_stamp    : 3;
  unsigned int   stamp_refidx3_stamp    : 3;
  unsigned int   stamp_refidx4_stamp    : 3;
  unsigned int   stamp_refidx5_stamp    : 3;
  unsigned int   stamp_refidx6_stamp    : 3;
  unsigned int   stamp_refidx7_stamp    : 3;
  unsigned int   reserved2              : 11;

} nvenc_h264_me_hint_cfg_s;

typedef struct
{
  unsigned int   mvx_frac                           : 2;
           int   mvx_int                            : 12;
  unsigned int   reserved_x                         : 2;
  unsigned int   mvy_frac                           : 2;
           int   mvy_int                            : 10;
  unsigned int   reserved_y                         : 4;
} nvenc_h264_me_const_vec_s;

typedef struct
{
  unsigned int   bitmask[2]                         ;
  unsigned int   hor_adjust                         : 1;
  unsigned int   ver_adjust                         : 1;
  unsigned int   reserved                           : 30;
} nvenc_h264_me_stamp_s;

typedef struct
{
  unsigned int   mv_limit_enable                    : 1;
  unsigned int   left_mvx_frac                      : 2;
           int   left_mvx_int                       : 12;
  unsigned int   reserved1                          : 1;
  unsigned int   top_mvy_frac                       : 2;
           int   top_mvy_int                        : 10;
  unsigned int   reserved2                          : 4;

  unsigned int   right_mvx_frac                     : 2;
           int   right_mvx_int                      : 12;
  unsigned int   reserved3                          : 2;
  unsigned int   bottom_mvy_frac                    : 2;
           int   bottom_mvy_int                     : 10;
  unsigned int   reserved4                          : 4;
} nvenc_h264_me_limit_mv_s;

typedef struct
{
  unsigned char teb_K                               ;
  unsigned char teb_N                               ;
  unsigned char teb_P                               ;
  unsigned char teb_S                               : 4;
  unsigned char teb_mode                            : 4;
} nvenc_me_tebif_s;

typedef struct
{
  unsigned int   me_predictor_mode                  : 1;
  unsigned int   refinement_mode                    : 1;
  unsigned int   lambda_mode                        : 1;
  unsigned int   const_lambda                       : 11;
  unsigned int   refine_on_search_enable            : 1;
  unsigned int   me_only_mode                       : 1;
  unsigned int   fps_mvcost                         : 1;
  unsigned int   sps_mvcost                         : 1;
  unsigned int   sps_cost_func                      : 1;
  unsigned int   me_icc_mode_mad                    : 1;

  unsigned int   sps_filter                         : 3;
  unsigned int   mc_filter                          : 3;
  unsigned int   vc1_fastuv_mc                      : 1;
  unsigned int   vc1_mc_rnd                         : 1;
  unsigned int   mbc_ctrl_arbitor                   : 2;
  unsigned int   mv_only_enable                     : 1;
  unsigned int   average_mvhint_enable              : 1;

  nvenc_h264_me_limit_mv_s      limit_mv;
  nvenc_h264_me_hint_cfg_s      predsrc;
  nvenc_h264_me_const_vec_s     l0_hint;
  nvenc_h264_me_const_vec_s     l1_hint;
  nvenc_h264_me_const_vec_s     l0_pred;
  nvenc_h264_me_const_vec_s     l1_pred;
  nvenc_h264_me_stamp_s         shape0;
  nvenc_h264_me_stamp_s         shape1;
  nvenc_h264_me_stamp_s         shape2;
  nvenc_h264_me_stamp_s         shape3;
  nvenc_h264_me_stamp_s         shape4;
  nvenc_h264_me_stamp_s         shape5;
  nvenc_h264_me_stamp_s         shape6;
  nvenc_h264_me_stamp_s         shape7;
  nvenc_me_tebif_s              teb_para;
  unsigned int mbc_mb_size                        : 9;
  unsigned int rmvp_source_half_scaled            : 1;
  unsigned int partDecisionMadeByFPP              : 1;
  unsigned int penalty_factor_FPP                 : 6;
  unsigned int spatial_hint_pattern               : 4;
  unsigned int temporal_hint_pattern              : 6;
  unsigned int Cu16partDecisionMadeByFPP          : 1;
  unsigned int sps_mvp_from_fbm                   : 1;
  unsigned int fbm_select_best_cu16_parttype_num    : 3;
  unsigned int fbm_op_winner_num_p_frame            : 3;
  unsigned int fbm_op_winner_num_b_frame_l0         : 3;
  unsigned int fbm_op_winner_num_b_frame_l1         : 3;
  unsigned int fbm_select_best_cu32_parttype_num    : 3;
  unsigned int sps_evaluate_merge_cand_num          : 3;
  unsigned int fps_quad_thresh_hold                 : 10;
  unsigned int external_hint_order                  : 1;
  unsigned int coloc_hint_order                     : 1;
  unsigned int reserved1                            : 5;
  short        ct_threshold                         ;
  unsigned short hint_type0                        : 3;
  unsigned short hint_type1                        : 3;
  unsigned short hint_type2                        : 3;
  unsigned short hint_type3                        : 3;
  unsigned short hint_type4                        : 3;
  unsigned short pyramidal_hint_order              : 1;
  unsigned char  left_hint_delay_N                 : 4;
  unsigned char  reserved3                         : 4;

  unsigned char  cu8partDecisionMadeByFPP          : 1;
  unsigned char  fbm_select_best_cu8_parttype_num  : 3;
  unsigned char  fbm_op_winner_num_p_frame_cu8     : 3;
  unsigned char  reserved4                         : 1;

  unsigned char  cu64partDecisionMadeByFPP         : 1;
  unsigned char  fbm_select_best_cu64_parttype_num : 3;
  unsigned char  reserved5                         : 4;

  unsigned char  reserved[33];
} nvenc_h264_me_control_s;

typedef struct
{
  unsigned short intra_refresh_cnt;
  unsigned short intra_refresh_offs;

  unsigned int   intra_luma4x4_mode_enable          : 9;
  unsigned int   intra_luma8x8_mode_enable          : 9;
  unsigned int   intra_luma16x16_mode_enable        : 4;
  unsigned int   intra_chroma_mode_enable           : 4;
  unsigned int   inter_penalty_factor_for_ip1       : 6;

  unsigned int   l0_part_16x16_enable               : 1;
  unsigned int   l0_part_16x8_enable                : 1;
  unsigned int   l0_part_8x16_enable                : 1;
  unsigned int   l0_part_8x8_enable                 : 1;
  unsigned int   l0_part_8x4_enable                 : 1;
  unsigned int   l0_part_4x8_enable                 : 1;
  unsigned int   l0_part_4x4_enable                 : 1;
  unsigned int   l0_part_reserved                   : 1;
  unsigned int   l1_part_16x16_enable               : 1;
  unsigned int   l1_part_16x8_enable                : 1;
  unsigned int   l1_part_8x16_enable                : 1;
  unsigned int   l1_part_8x8_enable                 : 1;
  unsigned int   l1_part_reserved                   : 4;
  unsigned int   bi_part_16x16_enable               : 1;
  unsigned int   bi_part_16x8_enable                : 1;
  unsigned int   bi_part_8x16_enable                : 1;
  unsigned int   bi_part_8x8_enable                 : 1;
  unsigned int   bi_part_reserved                   : 4;
  unsigned int   bdirect_mode                       : 2;
  unsigned int   bskip_enable                       : 1;
  unsigned int   pskip_enable                       : 1;
  unsigned int   special_reserved                   : 4;

           short bias_inter_16x16;
           short bias_inter_16x8;

           short bias_inter_8x16;
           short bias_inter_8x8;

           short bias_pskip;
           short bias_bdir;

           short bias_intra_over_inter;
           short bias_intra_16x16;

           short bias_intra_8x8;
           short bias_intra_4x4;

           short bias_intra_most_prob;
  unsigned short mv_cost_bias;

  unsigned short intra_nxn_bias_multiplier;
  unsigned short intra_most_prob_bias_multiplier;

           short pskip_bias_multiplier;
           short bdirect_bias_multiplier;

  unsigned short pskip_esc_threshold;
  unsigned short bdirect_esc_threshold;

  unsigned short early_intra_disable_mpeb_threshold;
  unsigned short tempmv_wt_spread_threshold;
  unsigned int   tempmv_wt_distort_threshold        : 16;
  unsigned int   mv_cost_predictor_control          : 1;
  unsigned int   mv_cost_enable                     : 1;
  unsigned int   intra_most_prob_force_on           : 1;
  unsigned int   early_intra_mode_control           : 2;
  unsigned int   early_intra_mode_type_16x16dc      : 1;
  unsigned int   early_intra_mode_type_16x16h       : 1;
  unsigned int   early_intra_mode_type_16x16v       : 1;
  unsigned int   early_ip_is_final                  : 1;
  unsigned int   chroma_eval_mode                   : 1;
  unsigned int   ip_search_mode                     : 3;
  unsigned int   multiply_bias_with_lambda          : 1;
  unsigned int   force_ipcm                         : 1;
  unsigned int   early_termination_ip1              : 1;
           short bias_favor_intra_16x16_early;
  unsigned short priority_ipred_type_ip1            : 3;
  unsigned short intra_ssd_cnt_4x4                  : 4;
  unsigned short intra_ssd_cnt_8x8                  : 4;
  unsigned short intra_ssd_cnt_16x16                : 4;
  unsigned short skip_evaluate_enable               : 1;
  unsigned int   rdo_level                          : 3;
  unsigned int   tu_search_num                      : 3;
  unsigned int   luma_residual_zero_eval            : 2;
  unsigned int   num_1div8_lambda_ssd               : 7;
  unsigned int   ofs_mvx_range                      : 2;
  unsigned int   reserved4                          : 15;
  short          bias_tu_4x4;
  short          bias_tu_8x8;
  unsigned int   ssim_rdo                           : 2;
  unsigned int   calc_ssim                          : 1;
  unsigned int   reserved5                          : 29;
  unsigned int   ssd_over_ssim_factor;
  unsigned int   reserved6[13];
} nvenc_h264_md_control_s;

typedef struct
{
  unsigned short qpp_run_vector_4x4;
  unsigned short qpp_run_vector_8x8[3];
  unsigned char  qpp_luma8x8_cost;
  unsigned char  qpp_luma16x16_cost;
  unsigned char  qpp_chroma_cost;
  unsigned char  qpp_mode                           : 2;
  unsigned char  reserved1                          : 4;
  unsigned char  quant_intra_sat_flag               : 1;
  unsigned char  quant_inter_sat_flag               : 1;
  unsigned int   quant_intra_sat_limit              : 16;
  unsigned int   quant_inter_sat_limit              : 16;
  unsigned short dz_4x4_YI[16];
  unsigned short dz_4x4_YP[16];
  unsigned short dz_4x4_CI;
  unsigned short dz_4x4_CP;
  unsigned short dz_8x8_YI[16];
  unsigned short dz_8x8_YP[16];
  unsigned   int reserved2[11];
} nvenc_h264_quant_control_s;

typedef struct
{
    unsigned int reorder_l0_cmd_count                 : 4;
    unsigned int reorder_l1_cmd_count                 : 4;
    unsigned int mmco_cmd_count                       : 4;
    unsigned int no_output_of_prior_pic_flag          : 1;
    unsigned int long_term_ref_flag                   : 1;
    unsigned int reserved                             :18;
} nvenc_h264_refpic_cmd_s;

typedef struct
{
    unsigned int   reordering_of_pic_nums_idc         : 3;
    unsigned int   abs_diff_pic_num_minus1            :17;
    unsigned int   long_term_pic_num                  : 4;
    unsigned int   abs_diff_view_idx_minus1           : 4;
    unsigned int   reserved                           : 4;
} nvenc_h264_ref_pic_reorder_s;

typedef struct
{
    unsigned int   mmco_cmd_id                        : 3;

    unsigned int   difference_of_pic_nums_minus1      :17;
    unsigned int   long_term_pic_num                  : 3;
    unsigned int   long_term_frame_idx                : 3;
    unsigned int   max_long_term_frame_idx_plus1      : 4;
    unsigned int   reserved                           : 2;
} nvenc_h264_mmco_s;

typedef struct
{
  unsigned int   num_mb                            : 19;
  unsigned int   qp_avr                            :  8;
  unsigned int   reserved                          :  5;
  unsigned int   slice_tgt_rate;

           char  slice_alpha_c0_offset_div2         : 4;
           char  slice_beta_offset_div2             : 4;
  unsigned char  qp_slice_min;
  unsigned short qp_slice_max                       : 8;
  unsigned short force_intra                        : 1;
  unsigned short disable_deblocking_filter_idc      : 2;
  unsigned short cabac_init_idc                     : 2;
  unsigned short disable_slice_header               : 1;
  unsigned short reserved0                          : 2;

  unsigned int   num_ref_idx_active_override_flag   : 1;
  unsigned int   num_ref_idx_l0_active_minus1       : 5;
  unsigned int   num_ref_idx_l1_active_minus1       : 5;
  unsigned int   reserved1                          : 9;
  unsigned int   me_control_idx                     : 4;
  unsigned int   md_control_idx                     : 4;
  unsigned int   q_control_idx                      : 4;

  unsigned int   limit_slice_top_boundary           : 1;
  unsigned int   limit_slice_bot_boundary           : 1;
  unsigned int   limit_slice_left_boundary          : 1;
  unsigned int   limit_slice_right_boundary         : 1;
  unsigned int   ROI_enable                         : 1;
  unsigned int   ROI_me_control_idx                 : 4;
  unsigned int   ROI_md_control_idx                 : 4;
  unsigned int   ROI_q_control_idx                  : 4;
  unsigned int   ROI_qp_delta                       : 8;
  unsigned int   wp_control_idx                     : 4;
  unsigned int   reserved2                          : 3;

  unsigned int   ROI_top_mbx                        : 8;
  unsigned int   ROI_top_mby                        : 8;
  unsigned int   ROI_bot_mbx                        : 8;
  unsigned int   ROI_bot_mby                        : 8;

  nvenc_h264_refpic_cmd_s        ref_pic_cmd_cfg;

  nvenc_h264_ref_pic_reorder_s   ref_pic_list_reorder_cmd[2][8];

  nvenc_h264_mmco_s              mmco_cmd[8] ;

  unsigned int   reserved3;

} nvenc_h264_slice_control_s;

#define NVENC_QRES_FLAG_ENABLE          0x1
#define NVENC_QRES_FLAG_DECIMATED       0x10
#define NVENC_QRES_FLAG_PASS2REFS       0x20

#define NVENC_LOOKAHEAD_INFO1(Wp2Wi)        ((unsigned char)(Wp2Wi))
#define NVENC_LOOKAHEAD_INFO2(nextI,depth)  (((unsigned short)(nextI)) | (((unsigned short)(depth))<<8))

#define NVENC_RC_HRD_VCL                0x1
#define NVENC_RC_HRD_NAL                0x2
#define NVENC_RC_HRD_STRICTGOPTARGET    0x4
#define NVENC_RC_FIXED_PBRATIO          0x8U

typedef struct
{
  unsigned char  hrd_type;
  unsigned char  QP[3];
  unsigned char  minQP[3];
  unsigned char  maxQP[3];
  unsigned char  maxQPD;
  unsigned char  baseQPD;
           int   rhopbi[3];

           int   framerate;
  unsigned int   buffersize;

           int   nal_cpb_size;
           int   nal_bitrate;
           int   vcl_cpb_size;
           int   vcl_bitrate;

  unsigned int   gop_length;
           int   Np;
           int   Bmin;
           int   Ravg;
           int   R;

  unsigned char  ab_alpha;
  unsigned char  ab_beta;
  unsigned short prev_act;

  unsigned char  aqMode                             : 3;
  unsigned char  dump_aq_stats                      : 1;
  unsigned char  is_emphasis_level                  : 1;
  unsigned char  reserve0                           : 3;
  unsigned char  single_frame_VBV;
  unsigned char  two_pass_rc;
  unsigned char  rc_class;

  unsigned char  first_pass_source_half_scaled;
  unsigned char  iSizeRatioX;
  unsigned char  iSizeRatioY;
  unsigned char  ext_scene_change_flag;
  unsigned int   ext_intra_satd;
  unsigned char  ext_picture_rc_hints;
  unsigned char  session_max_qp;
  unsigned char  reserve[2];
} nvenc_h264_rc_s;

typedef struct
{
  unsigned int   var_min;
  unsigned int   var_max;
  unsigned int   var_avg;
  unsigned char  reserved[4];
} nvenc_aq_stat_s;

typedef struct
{
           char  l0[8];
           char  l1[8];
           char  temp_dist_l0[8];
           char  temp_dist_l1[8];
           short dist_scale_factor[8][8];
  unsigned int   diff_pic_order_cnt_zero[2];
  unsigned int   max_slice_size;
  unsigned int   max_byte_count_before_resid_zero;
           int   delta_pic_order_cnt_bottom;
  unsigned short frame_num;
  unsigned short pic_order_cnt_lsb;
  unsigned short idr_pic_id;
  unsigned short colour_plane_id;
  unsigned int   longTermFlag;
  unsigned char  num_forced_slices_minus1;
  unsigned char  num_me_controls_minus1;
  unsigned char  num_md_controls_minus1;
  unsigned char  num_q_controls_minus1;
  unsigned int   slice_control_offset;
  unsigned int   me_control_offset;
  unsigned int   md_control_offset;
  unsigned int   q_control_offset;
  unsigned int   hist_buf_size;
  unsigned int   bitstream_buf_size;
  unsigned int   bitstream_start_pos;
  unsigned int   pic_struct                         : 2;
  unsigned int   pic_type                           : 2;
  unsigned int   ref_pic_flag                       : 1;
  unsigned int   slice_mode                         : 1;
  unsigned int   ipcm_rewind_enable                 : 1;
  unsigned int   cabac_zero_word_enable             : 1;

  unsigned int   base_view                          : 1;
  unsigned int   priority_id                        : 6;
  unsigned int   view_id                            : 10;
  unsigned int   temporal_id                        : 3;
  unsigned int   anchor_pic_flag                    : 1;
  unsigned int   inter_view_flag                    : 1;
  unsigned int   opm_enable                         : 1;
  unsigned int   temporal_hint8x8_enable            : 1;
  char           cur_interview_ref_pic;
  char           prev_interview_ref_pic;
  unsigned char  codec                              : 3;
  unsigned char  e4byteStartCode                    : 1;
  unsigned char  qpfifo                             : 1;
  unsigned int   me_candidates_output_struct        : 1;
  unsigned int   me_candidates_output_enable        : 1;
  unsigned char  medma_output_cost_mode             : 1;
  unsigned char  intraRefreshCount;
  unsigned int   mpec_threshold                     : 30;
  unsigned int   me_lambda_fifo_enable              : 1;
  unsigned int   me_candidates_rdo_mode             : 1;
  unsigned int   slice_stat_offset;
  unsigned int   mpec_stat_offset;
  unsigned int   wp_control_offset;

  unsigned char  num_wp_controls_minus1;
  unsigned char  b_as_ref                           : 1;
  unsigned char  ofs_mode                           : 2;
  unsigned char  new_subframe                       : 1;
  unsigned char  bit_depth_minus_8                  : 4;
  unsigned char  stripID;
  unsigned char  strips_in_frame;
  unsigned short slice_encoding_row_start;
  unsigned short slice_encoding_row_num;

  unsigned char  segment_enable                     : 1;
  unsigned char  background_seg                     : 5;
  unsigned char  input_sub_sampling                 : 1;
  unsigned char  output_sub_sampling                : 1;
  unsigned char  segment_P2                            ;
  unsigned char  segment_thr                           ;
  unsigned char  accurate_skip_mv                   : 1;
  unsigned char  intra_sel_4x4_enable               : 1;
  unsigned int   enable_source_image_padding        : 1;
  unsigned int   luma_padding_mode                  : 2;
  unsigned int   chroma_padding_mode                : 2;
  unsigned int   stats_fifo_enable                  : 1;
  unsigned int   chroma_skip_threshold_4x4         : 16;
  unsigned int   intra_sel_4x4_threshold           : 16;
  unsigned int   aq_stat_offset;
  unsigned int   act_stat_offset;
  unsigned int   stats_fifo_offset;
} nvenc_h264_pic_control_s;

#ifdef NV_NVENC_8_2
#define NV_NVENC_DRV_MAGIC_VALUE  0xC9B70006
#elif NV_NVENC_8_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xC8B70006
#elif NV_NVENC_7_3
#define NV_NVENC_DRV_MAGIC_VALUE  0xC7B70006
#elif NV_NVENC_7_2
#define NV_NVENC_DRV_MAGIC_VALUE  0xC4B70006
#elif NV_NVENC_7_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xC5B70006
#elif NV_NVENC_6_8
#define NV_NVENC_DRV_MAGIC_VALUE  0xB6B70006
#elif NV_NVENC_6_6
#define NV_NVENC_DRV_MAGIC_VALUE  0xB4B70006
#elif NV_NVENC_6_4
#define NV_NVENC_DRV_MAGIC_VALUE  0xC3B70006
#elif NV_NVENC_6_2
#define NV_NVENC_DRV_MAGIC_VALUE  0xC2B70006
#elif NV_NVENC_6_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xc1b70006
#elif NV_NVENC_5_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xd0b70006
#elif NV_NVENC_1_0
#define NV_NVENC_DRV_MAGIC_VALUE  0xc0b70006
#elif NV_MSENC_2_0
#define NV_MSENC_DRV_MAGIC_VALUE  0xa0b70006
#else
#define NV_MSENC_DRV_MAGIC_VALUE  0x90b70006
#endif

#define HIST_BLOCK_SIZE  192
const static int AV1_HIST_PIXEL_FRAME_SIZE  =  (8 * 8192);
const static int AV1_HIST_MV_SB_SIZE        =  (256 * 4);

typedef struct
{
  unsigned int                      magic;
  nvenc_h264_surface_cfg_s          refpic_cfg;
  nvenc_h264_surface_cfg_s          input_cfg;
  nvenc_h264_surface_cfg_s          outputpic_cfg;
  nvenc_h264_sps_data_s             sps_data;
  nvenc_h264_pps_data_s             pps_data;
  nvenc_h264_rc_s                   rate_control;
  nvenc_h264_pic_control_s          pic_control;
  nvenc_h264_surface_cfg_s          half_scaled_outputpic_cfg;
  unsigned int                      gpTimer_timeout_val;

} nvenc_h264_drv_pic_setup_s;

typedef struct
{
  unsigned int   picture_index;
  unsigned int   error_status                       : 2;
  unsigned int   ucode_error_status                 : 30;
  unsigned int   total_bit_count;
  unsigned int   type1_bit_count;
  unsigned short pic_type;
  unsigned short num_slices;
  unsigned short ave_activity;
  unsigned short avgQP;
  unsigned int   cycle_count;
           int   hrdFullness;
  unsigned int   bitstream_start_pos;
  unsigned int   last_valid_byte_offset;
  unsigned short intra_mb_count;
  unsigned short inter_mb_count;
  unsigned int   cumulative_intra_cost;
  unsigned int   cumulative_inter_cost;
  unsigned int   total_intra_cost;
  unsigned int   total_inter_cost;
  unsigned int   complexity;
           short average_mvx;
           short average_mvy;
  unsigned short actual_min_qp_used;
  unsigned short actual_max_qp_used;
  unsigned int  hw_perf_me_sps;
  unsigned int  hw_perf_me_fps;
  unsigned int  hw_perf_mpec_active_cnt;
  unsigned int  hw_perf_mpeb_active_cnt;
  unsigned long long int total_dist_ssd;
  unsigned int  total_dist_ssim;
  unsigned int  best_satd_l0;
  unsigned int  best_satd_l1;
  unsigned int reserved[5];

} nvenc_pic_stat_s;

#endif
