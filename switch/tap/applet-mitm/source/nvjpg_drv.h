#pragma once

#ifndef NV_NVJPG_SUPPORT_ENC
#define NV_NVJPG_SUPPORT_ENC 1
#endif

#ifndef NVJPG_DRV_H
#define NVJPG_DRV_H

#ifdef NV_NVJPG_SUPPORT_ENC
typedef struct
{
    unsigned int    chroma_format       :  2;
    unsigned int    mcu_width           : 12;
    unsigned int    mcu_height          : 12;
    unsigned int    stuff_disable       :  1;
    unsigned int    rsvd                :  5;
}ENC_TOP_CTL_s;

typedef struct
{
    unsigned int    restart_interval    :  16;
    unsigned int    rsvd                :  16;
}ENC_TOP_RST_INTERVAL_s;

typedef struct
{
    unsigned short  quantLumaFactor[64];
    unsigned short  quantChromaFactor[64];
}ENC_FDCT_QT_WDATA_s;

typedef struct
{
    unsigned int    prequant_enable     :   1;
    unsigned int    prequant_index      :   8;
    unsigned int    prequant_threshold  :  21;
    unsigned int    rsvd                :   2;
}ENC_FDCT_RATECTRL_s;

#define DCVALUEITEM 12
#define ACVALUEITEM 176
typedef struct
{
    unsigned short length;
    unsigned short value;
}huffman_symbol_s;

typedef struct
{
    huffman_symbol_s hfDcLuma[DCVALUEITEM];
    huffman_symbol_s hfAcLuma[ACVALUEITEM];
    huffman_symbol_s hfDcChroma[DCVALUEITEM];
    huffman_symbol_s hfAcChroma[ACVALUEITEM];
}ENC_VLE_HT_WDATA_s;

typedef struct
{
    unsigned int    rate_control        :   2;
    unsigned int    luma_target_bits    :  11;
    unsigned int    chroma_target_bits  :  11;
    unsigned int    rsvd                :   8;
}ENC_VLE_RATECTRL_s;
#endif

typedef struct
{
    unsigned int input_type             :   3;
    unsigned int chroma_mode            :   3;
    unsigned int width_in_8x8           :  12;
    unsigned int height_in_8x8          :  12;
    unsigned int power2_scale           :   2;
}PDMA_PIC_INFO_s;

typedef struct
{
    unsigned int rsvd0                  :  15;
    unsigned int tiling_mode            :   2;
    unsigned int blklinear_gob          :   3;
    unsigned int rsvd1                  :   1;
    unsigned int width_pad              :   4;
    unsigned int height_pad             :   4;
    unsigned int memory_mode            :   3;
}PDMA_PIC_INFO1_s;
    
typedef struct
{
    unsigned int luma_8x1_height        :   4;
    unsigned int chroma_8x1_height      :   4;
    unsigned int luma_block_num         :   3;
    unsigned int chroma_block_num       :   3;
    unsigned int rsvd                   :  18;
}PDMA_DEC_PACKET_INFO_s;

typedef struct 
{
    unsigned int mcu_width      : 12;
    unsigned int rsvd0          :  4;
    unsigned int mcu_height     : 12;
    unsigned int rsvd1          :  4;
}DEC_FRAME_SIZE_s;

typedef struct 
{
    unsigned int component              :  3;
    unsigned int restart_interval       : 16;
    unsigned int stream_chroma_mode     :  3;
    unsigned int motion_jpeg_type       :  1;
    unsigned int output_type            :  3;
    unsigned int power2_downscale       :  2;
    unsigned int chroma_subsample       :  2;
    unsigned int rsvd                   :  2;
}DEC_FRAME_PARAM_s;

typedef struct
{
    unsigned int ac_idx                 :   2;
    unsigned int dc_idx                 :   2;
    unsigned int quant                  :   2;
    unsigned int rsvd0                  :   2;
    unsigned int hblock                 :   4;
    unsigned int vblock                 :   4;
    unsigned int rsvd1                  :  16;
}DEC_BLOCK_CTL_s;

typedef struct 
{
    unsigned int ac                     :   1;
    unsigned int comp                   :   2;
    unsigned int off                    :   4;
    unsigned int idx                    :   8;
    unsigned int val                    :  17;
}DEC_HUFFMAN_MINCODE_s;

typedef struct 
{
    unsigned int ac                     :   1;
    unsigned int comp                   :   2;
    unsigned int off                    :   8;
    unsigned int val                    :   8;
    unsigned int rsvd                   :  13;
}DEC_HUFFMAN_SYMBOL_s;

typedef struct
{
    unsigned int luma_blocks_x          :   2;
    unsigned int luma_blocks_y          :   2;
    unsigned int chroma_blocks_x        :   2;
    unsigned int chroma_blocks_y        :   2;
    unsigned int luma_block_width       :   4;
    unsigned int luma_block_height      :   4;
    unsigned int chroma_block_width     :   4;
    unsigned int chroma_block_height    :   4;
    unsigned int alpha_val              :   8;
}DEC_OUTPUT_INFO_s;

typedef struct 
{
#ifdef NV_NVJPG_SUPPORT_ENC
    ENC_TOP_CTL_s           enc_top_ctl;
    ENC_TOP_RST_INTERVAL_s  enc_restart_interval;
    ENC_FDCT_RATECTRL_s     enc_fdct_rc;
    ENC_VLE_RATECTRL_s      enc_vle_rc;
    unsigned int            enc_align256[4];
    ENC_FDCT_QT_WDATA_s     enc_quant_tab;
    ENC_VLE_HT_WDATA_s      enc_huff_tab;
#endif
    PDMA_PIC_INFO_s         pdma_pic_info;
    PDMA_PIC_INFO1_s        pdma_pic_info1;
    unsigned int            luma_stride;
    unsigned int            chroma_stride;
    PDMA_DEC_PACKET_INFO_s  pdma_dec_packet_info;
    unsigned int            pdma_align256[3];

    unsigned int            bitstream_offset;
    unsigned int            bitstream_size;
    unsigned int            sdma_align256[6];

    DEC_FRAME_SIZE_s        dec_frame_size;
    DEC_FRAME_PARAM_s       dec_frame_param;
    DEC_BLOCK_CTL_s         dec_block_ctl[4];
    unsigned int            dec_align256_0[2];
    DEC_HUFFMAN_MINCODE_s   dec_huff_mincode[6][16];
    DEC_HUFFMAN_SYMBOL_s    dec_huff_symbol[3][174];
    unsigned int            dec_align256_1[6];
    DEC_OUTPUT_INFO_s       dec_output_info;
    unsigned int            dec_yuv2rgb_param[6];
    unsigned int            dec_align256_2;
    unsigned char           dec_quant[64][4];

}nvjpg_drv_pic_param_s;

typedef struct
{
    unsigned int bytes_offset;
    unsigned int mcu_x;
    unsigned int mcu_y;
    unsigned int cycle_count;
    unsigned int error_status;
    unsigned char reserved[12];
}nvjpg_dec_status;

typedef struct
{
    unsigned int bitstream_size;
    unsigned int mcu_x;
    unsigned int mcu_y;
    unsigned int cycle_count;
    unsigned int error_status;
    unsigned char reserved[12];  
}nvjpg_enc_status;

#endif

