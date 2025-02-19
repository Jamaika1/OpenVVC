#include <string.h>

#include "ovutils.h"
#include "cabac_internal.h"
#include "vcl.h"
#include "dec_structures.h"
#include "ctudec.h"
#include "rcn.h"
#include "dbf_utils.h"
#include "drv_utils.h"
#include "drv.h"

struct TBInfo {
   uint16_t last_pos;
   uint64_t sig_sb_map;
};

struct TUInfo {
   uint8_t is_sbt;
   uint8_t cbf_mask;
   uint16_t pos_offset;
   uint8_t tr_skip_mask;
   uint8_t cu_mts_flag;
   uint8_t cu_mts_idx;
   uint8_t lfnst_flag;
   uint8_t lfnst_idx;
   struct TBInfo tb_info[3];
};

struct ISPTUInfo {
   uint8_t cbf_mask;
   uint8_t tr_skip_mask;
   uint8_t cu_mts_flag;
   uint8_t cu_mts_idx;
   uint8_t lfnst_flag;
   uint8_t lfnst_idx;
   struct TBInfo tb_info[4];
};

/* FIXME refactor dequant*/
static void
derive_dequant_ctx(OVCTUDec *const ctudec, const VVCQPCTX *const qp_ctx,
                  int cu_qp_delta)
{
    /*FIXME avoid negative values especiallly in chroma_map derivation*/
    int base_qp = (qp_ctx->current_qp + cu_qp_delta + 64) & 63;
    ctudec->dequant_luma.qp = ((base_qp + 12) & 63);

    /*FIXME update transform skip ctx to VTM-10.0 */
    ctudec->dequant_luma_skip.qp = OVMAX(ctudec->dequant_luma.qp, qp_ctx->min_qp_prime_ts);
    ctudec->dequant_cb.qp = qp_ctx->chroma_qp_map_cb[(base_qp + qp_ctx->cb_offset + 64) & 63] + 12;
    ctudec->dequant_cr.qp = qp_ctx->chroma_qp_map_cr[(base_qp + qp_ctx->cr_offset + 64) & 63] + 12;
    ctudec->dequant_joint_cb_cr.qp = qp_ctx->chroma_qp_map_jcbcr[(base_qp + 64) & 63] + qp_ctx->jcbcr_offset + 12;
    ctudec->qp_ctx.current_qp = base_qp;
}

static uint8_t
ovcabac_read_ae_root_cbf(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[QT_ROOT_CBF_CTX_OFFSET]);
}

static uint8_t
ovcabac_read_ae_tu_cbf_luma_isp(OVCABACCtx *const cabac_ctx,
                                uint8_t prev_cbf)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    int offset = 2 + prev_cbf;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[QT_CBF_CTX_OFFSET + offset] );
}

static uint8_t
ovcabac_read_ae_tu_cbf_luma(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[QT_CBF_CTX_OFFSET] );
}

static uint8_t
ovcabac_read_ae_tu_cbf_cb(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[QT_CBF_CB_CTX_OFFSET] );
}

static uint8_t
ovcabac_read_ae_tu_cbf_cr(OVCABACCtx *const cabac_ctx,
                          uint8_t tu_cbf_cb)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[QT_CBF_CR_CTX_OFFSET + tu_cbf_cb] );
}

static uint8_t
ovcabac_read_ae_joint_cb_cr_flag(OVCABACCtx *const cabac_ctx,
                                 uint8_t cbf_mask_minus1)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[JOINT_CB_CR_FLAG_CTX_OFFSET + cbf_mask_minus1] );
}

static int
vvc_exp_golomb(OVCABACCtx *const cabac_ctx)
{
    int symbol = 0;
    int bit = 1;
    int add_val = 0;
    int count = 0;
    /*FIXME determine max_num_bits to be read in bypass */
     while (bit && count <= 32){
        bit = ovcabac_bypass_read(cabac_ctx);
        symbol += bit << count++;
     }
     if (--count){
         add_val = 0;
         while (count){
             add_val <<= 1;
             add_val |= ovcabac_bypass_read(cabac_ctx);
             count--;
         }
     }
     return symbol + add_val;
}

static int
ovcabac_read_ae_cu_delta_qp(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
   int delta_qp = ovcabac_ae_read(cabac_ctx, &cabac_state[DELTA_QP_CTX_OFFSET]);
   if (delta_qp)
       while (delta_qp < 5 && ovcabac_ae_read(cabac_ctx, &cabac_state[DELTA_QP_CTX_OFFSET + 1]))
       delta_qp ++;

   if (delta_qp >= 5){
       delta_qp += vvc_exp_golomb(cabac_ctx);
   }

   if (delta_qp){
       delta_qp = ovcabac_bypass_read(cabac_ctx) ? -delta_qp : delta_qp;
   }

   return delta_qp;
}

static uint8_t
ovcabac_read_ae_cu_mts_flag(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx,&cabac_state[MTS_IDX_CTX_OFFSET]);
}

static uint8_t
ovcabac_read_ae_cu_mts_idx(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    uint8_t symbol = ovcabac_ae_read(cabac_ctx, &cabac_state[MTS_IDX_CTX_OFFSET + 1]);
    if(symbol && ovcabac_ae_read(cabac_ctx, &cabac_state[MTS_IDX_CTX_OFFSET + 2])){
        symbol++;
        if( ovcabac_ae_read(cabac_ctx, &cabac_state[MTS_IDX_CTX_OFFSET + 3])){
            symbol++;
        }
    }
    return symbol;
}

static uint8_t
ovcabac_read_ae_transform_skip_luma_flag(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[TRANSFORM_SKIP_FLAG_CTX_OFFSET]);
}

static uint8_t
ovcabac_read_ae_transform_skip_flag_c(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[TRANSFORM_SKIP_FLAG_CTX_OFFSET + 1]);
}

static inline int
decode_last_sig_prefix(OVCABACCtx *const cabac_ctx,
                       unsigned int log2_tb_d, unsigned offset_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    /* FIXME this tab could be adapted by adding some unused ctx to last sig ctx */
    static const int prefix_ctx[8]  = { 0, 0, 0, 3, 6, 10, 15, 21 };
    int pos = 0;
    int ctx_offset, ctx_shift;
    int max_symbol = OVMIN(log2_tb_d, 5) << 1;

    ctx_offset = prefix_ctx[log2_tb_d];
    ctx_shift  = (log2_tb_d + 1) >> 2 ;

    while(--max_symbol > 0 && ovcabac_ae_read(cabac_ctx, &cabac_state[offset_ctx + ctx_offset + (pos >> ctx_shift)])){
        ++pos;
    }
    return pos;
}

static inline int
decode_last_sig_prefix_sbt_mts(OVCABACCtx *const cabac_ctx,
                               unsigned int log2_tb_d, unsigned int log2_tb_red,
                               unsigned offset_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    /* FIXME this tab could be adapted by adding some unused ctx to last sig ctx */
    static const int prefix_ctx[8]  = { 0, 0, 0, 3, 6, 10, 15, 21 };
    int pos = 0;
    int ctx_offset, ctx_shift;
    int max_symbol = OVMIN(log2_tb_red, 5) << 1;

    ctx_offset = prefix_ctx[log2_tb_d];
    ctx_shift  = (log2_tb_red + 1) >> 2 ;

    while(--max_symbol > 0 && ovcabac_ae_read(cabac_ctx, &cabac_state[offset_ctx + ctx_offset + (pos >> ctx_shift)])){
        ++pos;
    }
    return pos;
}

static inline int
decode_last_sig_prefix_c(OVCABACCtx *const cabac_ctx,
                         unsigned int log2_tb_h, unsigned offset_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    int pos = 0;
    int ctx_shift;
    int max_symbol = log2_tb_h << 1;

    ctx_shift = (1 << log2_tb_h) >> 3 ;
    ctx_shift = ov_clip(ctx_shift,0,2);

    while(--max_symbol > 0 && ovcabac_ae_read(cabac_ctx, &cabac_state[offset_ctx + (pos >> ctx_shift)])){
        ++pos;
    }

    return pos;
}

static inline int
decode_last_sig_suffix(OVCABACCtx *const cabac_ctx, int prefix)
{
    int num_bins = (prefix - 2) >> 1;
    int val = 0;
    while (num_bins > 0){
        val = val << 1 | ovcabac_bypass_read(cabac_ctx) ;
        --num_bins;
    }
    val = (1 << ((prefix >> 1) - 1)) * (2 + (prefix & 1)) + val;
    return val;
}

static uint16_t
ovcabac_read_ae_last_sig_pos(OVCABACCtx *const cabac_ctx,
                             uint8_t log2_tb_w, uint8_t log2_tb_h)
{
    uint8_t last_x;
    uint8_t last_y;

    last_x = decode_last_sig_prefix(cabac_ctx, log2_tb_w, LAST_X_CTX_OFFSET);

    last_y = decode_last_sig_prefix(cabac_ctx, log2_tb_h, LAST_Y_CTX_OFFSET);

    if (last_x > 3) {
        last_x = decode_last_sig_suffix(cabac_ctx, last_x);
    }

    if (last_y > 3) {
        last_y = decode_last_sig_suffix(cabac_ctx, last_y);
    }

    return ((uint16_t) last_y << 8) | (last_x & 0xFF);
}

static uint16_t
ovcabac_read_ae_last_sig_pos_red(OVCABACCtx *const cabac_ctx,
                                 uint8_t log2_tb_w, uint8_t log2_tb_h)
{
    uint8_t last_x;
    uint8_t last_y;
    uint8_t log2_red_w = log2_tb_w == 5 ? 4 : log2_tb_w;
    uint8_t log2_red_h = log2_tb_h == 5 ? 4 : log2_tb_h;

    last_x = decode_last_sig_prefix_sbt_mts(cabac_ctx, log2_tb_w, log2_red_w, LAST_X_CTX_OFFSET);

    last_y = decode_last_sig_prefix_sbt_mts(cabac_ctx, log2_tb_h, log2_red_h, LAST_Y_CTX_OFFSET);

    if (last_x > 3) {
        last_x = decode_last_sig_suffix(cabac_ctx, last_x);
    }

    if (last_y > 3) {
        last_y = decode_last_sig_suffix(cabac_ctx, last_y);
    }

    return ((uint16_t) last_y << 8) | (last_x & 0xFF);
}

static uint16_t
ovcabac_read_ae_last_sig_pos_c(OVCABACCtx *const cabac_ctx,
                               uint8_t log2_tb_w, uint8_t log2_tb_h)
{
    uint8_t last_x;
    uint8_t last_y;

    last_x = decode_last_sig_prefix_c(cabac_ctx, log2_tb_w, LAST_X_C_CTX_OFFSET);
    last_y = decode_last_sig_prefix_c(cabac_ctx, log2_tb_h, LAST_Y_C_CTX_OFFSET);

    if (last_x > 3){
        last_x = decode_last_sig_suffix(cabac_ctx, last_x);
    }

    if(last_y > 3){
        last_y = decode_last_sig_suffix(cabac_ctx, last_y);
    }

    return ((uint16_t) last_y << 8) | (last_x & 0xFF);
}

static uint8_t
ovcabac_read_ae_lfnst_flag(OVCABACCtx *const cabac_ctx, uint8_t is_dual_tree)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    uint8_t lfnst_flag = ovcabac_ae_read(cabac_ctx, &cabac_state[LFNST_IDX_CTX_OFFSET + is_dual_tree]);
    return lfnst_flag;
}

static uint8_t
ovcabac_read_ae_lfnst_idx(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[LFNST_IDX_CTX_OFFSET + 2]);
}

static uint8_t
ovcabac_read_ae_sbt_flag(OVCABACCtx *const cabac_ctx, uint8_t log2_tb_w, uint8_t log2_tb_h)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    uint8_t ctx_offset = log2_tb_w + log2_tb_h <= 8;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[SBT_FLAG_CTX_OFFSET + ctx_offset]);
}

static uint8_t
ovcabac_read_ae_sbt_quad_flag(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[SBT_QUAD_FLAG_CTX_OFFSET]);
}

static uint8_t
ovcabac_read_ae_sbt_hor_flag(OVCABACCtx *const cabac_ctx, uint8_t log2_tb_w, uint8_t log2_tb_h)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    uint8_t ctx_offset = (log2_tb_w == log2_tb_h) ? 0 :  (log2_tb_w < log2_tb_h) ? 1 : 2;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[SBT_HOR_FLAG_CTX_OFFSET + ctx_offset]);
}

static uint8_t
ovcabac_read_ae_sbt_pos_flag(OVCABACCtx *const cabac_ctx)
{
    uint64_t *const cabac_state = cabac_ctx->ctx_table;
    return ovcabac_ae_read(cabac_ctx, &cabac_state[SBT_POS_FLAG_CTX_OFFSET]);
}


static uint8_t
decode_cbf_st(const OVCTUDec *const ctu_dec, uint8_t rqt_root_cbf, uint8_t tr_depth, uint8_t cu_flags)
{
    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
    uint8_t tu_cbf_cb = ovcabac_read_ae_tu_cbf_cb(cabac_ctx);
    uint8_t tu_cbf_cr = ovcabac_read_ae_tu_cbf_cr(cabac_ctx, tu_cbf_cb);
    uint8_t cbf_mask = (tu_cbf_cb << 1) | tu_cbf_cr;
    uint8_t tu_cbf_luma = rqt_root_cbf;

    if (!rqt_root_cbf || (cbf_mask && rqt_root_cbf) || (tr_depth == 1 && rqt_root_cbf)){
        tu_cbf_luma = ovcabac_read_ae_tu_cbf_luma(cabac_ctx);
    }

    /* FIXME intra if inter we only check for cbf_mask == 3*/
    if (ctu_dec->jcbcr_enabled && (((cu_flags & 0x2) && cbf_mask) || cbf_mask == 3)) {
        uint8_t joint_cb_cr = ovcabac_read_ae_joint_cb_cr_flag(cabac_ctx, (cbf_mask & 0x3) - 1);
        cbf_mask |= joint_cb_cr << 3;
    }

    return cbf_mask | (tu_cbf_luma << 4);
}

static uint8_t
decode_cbf_c(const OVCTUDec *const ctu_dec)
{
    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
    uint8_t tu_cbf_cb = ovcabac_read_ae_tu_cbf_cb(cabac_ctx);
    uint8_t tu_cbf_cr = ovcabac_read_ae_tu_cbf_cr(cabac_ctx, tu_cbf_cb);
    uint8_t cbf_mask = (tu_cbf_cb << 1) | tu_cbf_cr;

    if (ctu_dec->jcbcr_enabled && cbf_mask) {
        uint8_t joint_cb_cr = ovcabac_read_ae_joint_cb_cr_flag(cabac_ctx,
                (cbf_mask & 0x3) - 1);
        cbf_mask |= joint_cb_cr << 3;
    }

    return cbf_mask;
}

static void
rcn_res_c(OVCTUDec *const ctu_dec, const struct TUInfo *tu_info,
          uint8_t x0, uint8_t y0,
          uint8_t log2_tb_w, uint8_t log2_tb_h, uint8_t cbf_mask, uint8_t lfnst_flag)
{
    const struct RCNFunctions *const rcn_func = &ctu_dec->rcn_ctx.rcn_funcs;

    if (cbf_mask & 0x2) {
        uint16_t *const dst_cb = &ctu_dec->rcn_ctx.ctu_buff.cb[(x0) + (y0 * RCN_CTB_STRIDE)];
        int16_t scale  =  ctu_dec->lmcs_info.scale_c_flag ? ctu_dec->lmcs_info.lmcs_chroma_scale : 1<< 11;
        int16_t *const coeffs_cb = ctu_dec->residual_cb + tu_info->pos_offset;

        if (!(tu_info->tr_skip_mask & 0x2)) {
            const struct TBInfo *const tb_info_cb = &tu_info->tb_info[0];
            rcn_residual_c(ctu_dec, ctu_dec->transform_buff, coeffs_cb,
                           x0, y0, log2_tb_w, log2_tb_h,
                           tb_info_cb->last_pos, lfnst_flag, tu_info->lfnst_idx);
            if (log2_tb_w + log2_tb_h > 2) {
                rcn_func->ict.ict[log2_tb_w][0](ctu_dec->transform_buff, dst_cb, log2_tb_w, log2_tb_h, scale);
            } else {
                rcn_func->ict.add[log2_tb_w](ctu_dec->transform_buff, dst_cb, log2_tb_w, log2_tb_h, scale);
            }
        } else {
            if (log2_tb_w + log2_tb_h > 2) {
                rcn_func->ict.ict[log2_tb_w][0](coeffs_cb, dst_cb, log2_tb_w, log2_tb_h, scale);
            } else {
                rcn_func->ict.add[log2_tb_w](coeffs_cb, dst_cb, log2_tb_w, log2_tb_h, scale);
            }
        }


        fill_bs_map(&ctu_dec->dbf_info.bs1_map_cb, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1);
    }

    if (cbf_mask & 0x1) {
        uint16_t *const dst_cr = &ctu_dec->rcn_ctx.ctu_buff.cr[(x0) + (y0 * RCN_CTB_STRIDE)];
        int16_t scale  =  ctu_dec->lmcs_info.scale_c_flag ? ctu_dec->lmcs_info.lmcs_chroma_scale : 1<< 11;
        int16_t *const coeffs_cr = ctu_dec->residual_cr + tu_info->pos_offset;

        if (!(tu_info->tr_skip_mask & 0x1)) {
            const struct TBInfo *const tb_info_cr = &tu_info->tb_info[1];
            rcn_residual_c(ctu_dec, ctu_dec->transform_buff, coeffs_cr,
                           x0, y0, log2_tb_w, log2_tb_h,
                           tb_info_cr->last_pos, lfnst_flag, tu_info->lfnst_idx);
            if (log2_tb_w + log2_tb_h > 2) {
                rcn_func->ict.ict[log2_tb_w][0](ctu_dec->transform_buff, dst_cr, log2_tb_w, log2_tb_h, scale);
            } else {
                rcn_func->ict.add[log2_tb_w](ctu_dec->transform_buff, dst_cr, log2_tb_w, log2_tb_h, scale);
            }
        } else {
            if (log2_tb_w + log2_tb_h > 2) {
                rcn_func->ict.ict[log2_tb_w][0](coeffs_cr, dst_cr, log2_tb_w, log2_tb_h, scale);
            } else {
                rcn_func->ict.add[log2_tb_w](coeffs_cr, dst_cr, log2_tb_w, log2_tb_h, scale);
            }
        }

        fill_bs_map(&ctu_dec->dbf_info.bs1_map_cr, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1);
    }
}

static void
rcn_jcbcr(OVCTUDec *const ctu_dec, const struct TUInfo *const tu_info,
          uint8_t x0, uint8_t y0, uint8_t log2_tb_w, uint8_t log2_tb_h,
          uint8_t cbf_mask, uint8_t lfnst_flag)
{
    const struct RCNFunctions *const rcn_func = &ctu_dec->rcn_ctx.rcn_funcs;
    uint16_t *const dst_cb = &ctu_dec->rcn_ctx.ctu_buff.cb[x0 + (y0 * RCN_CTB_STRIDE)];
    uint16_t *const dst_cr = &ctu_dec->rcn_ctx.ctu_buff.cr[x0 + (y0 * RCN_CTB_STRIDE)];
    if (!(tu_info->tr_skip_mask & 0x1)) {
        const struct TBInfo *const tb_info = &tu_info->tb_info[0];
        int16_t *const coeffs_jcbcr = ctu_dec->residual_cb + tu_info->pos_offset;
        rcn_residual_c(ctu_dec, ctu_dec->transform_buff, coeffs_jcbcr,
                       x0, y0, log2_tb_w, log2_tb_h,
                       tb_info->last_pos, lfnst_flag, tu_info->lfnst_idx);
    } else {
        int16_t *const coeffs_jcbcr = ctu_dec->residual_cb + tu_info->pos_offset;
        memcpy(ctu_dec->transform_buff, coeffs_jcbcr, sizeof(int16_t) << (log2_tb_w + log2_tb_h));
    }

    fill_bs_map(&ctu_dec->dbf_info.bs1_map_cb, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1);
    fill_bs_map(&ctu_dec->dbf_info.bs1_map_cr, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1);
    fill_ctb_bound_c(&ctu_dec->dbf_info, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1);
    if ((cbf_mask & 0x3) == 0x3) {
        uint8_t    qp = ctu_dec->dequant_joint_cb_cr.qp - 12;
        dbf_fill_qp_map(&ctu_dec->dbf_info.qp_map_cb, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1, qp);
        dbf_fill_qp_map(&ctu_dec->dbf_info.qp_map_cr, x0 << 1, y0 << 1, log2_tb_w + 1, log2_tb_h + 1, qp);
    }
    /* FIXME set jcbcr dbf qp */

    /* FIXME better organisation based on cbf_mask */
    if (cbf_mask == 3) {
        int16_t scale  =  ctu_dec->lmcs_info.scale_c_flag ? ctu_dec->lmcs_info.lmcs_chroma_scale : 1<< 11;
        if (log2_tb_w + log2_tb_h == 2) scale = 1<<11;
        rcn_func->ict.ict[log2_tb_w][0](ctu_dec->transform_buff, dst_cb, log2_tb_w, log2_tb_h, scale);
        rcn_func->ict.ict[log2_tb_w][1](ctu_dec->transform_buff, dst_cr, log2_tb_w, log2_tb_h, scale);
    } else if (cbf_mask == 2) {
        int16_t scale  =  ctu_dec->lmcs_info.scale_c_flag ? ctu_dec->lmcs_info.lmcs_chroma_scale : 1<< 11;
        if (log2_tb_w + log2_tb_h == 2) scale = 1<<11;
        rcn_func->ict.ict[log2_tb_w][0](ctu_dec->transform_buff, dst_cb, log2_tb_w, log2_tb_h, scale);
        rcn_func->ict.ict[log2_tb_w][2](ctu_dec->transform_buff, dst_cr, log2_tb_w, log2_tb_h, scale);
    } else {
        int16_t scale  =  ctu_dec->lmcs_info.scale_c_flag ? ctu_dec->lmcs_info.lmcs_chroma_scale : 1<< 11;
        if (log2_tb_w + log2_tb_h == 2) scale = 1<<11;
        rcn_func->ict.ict[log2_tb_w][0](ctu_dec->transform_buff, dst_cr, log2_tb_w, log2_tb_h, scale);
        rcn_func->ict.ict[log2_tb_w][2](ctu_dec->transform_buff, dst_cb, log2_tb_w, log2_tb_h, scale);
    }

}

static void
recon_isp_subtree_v(OVCTUDec *const ctudec,
                    unsigned int x0, unsigned int y0,
                    unsigned int log2_cb_w, unsigned int log2_cb_h,
                    uint8_t intra_mode,
                    const struct ISPTUInfo *const tu_info)
{
    #if 0
    struct OVDrvCtx *const pred_ctx = &ctudec->drv_ctx;
    #endif
    const struct TRFunctions *TRFunc = &ctudec->rcn_ctx.rcn_funcs.tr;
    const struct RCNFunctions *const rcn_func = &ctudec->rcn_ctx.rcn_funcs;
    uint8_t cbf_flags = tu_info->cbf_mask;
    uint8_t lfnst_flag = tu_info->lfnst_flag;

    int offset_x;
    int log2_pb_w = log2_cb_w - 2;
    int nb_pb;
    int pb_w;

    if (log2_cb_h < 4 && (log2_pb_w <= (4 - log2_cb_h))) {
        log2_pb_w = 4 - log2_cb_h;
    }
    nb_pb = (1 << log2_cb_w) >> log2_pb_w;
    pb_w =  1 << log2_pb_w;
    offset_x = 0;

    uint8_t type_h = ctudec->mts_enabled && log2_pb_w <= 4 && log2_pb_w > 1 ? DST_VII : DCT_II;
    uint8_t type_v = ctudec->mts_enabled && log2_cb_h <= 4 ? DST_VII : DCT_II;
    int i;

    for (i = 0; i < nb_pb; ++i) {
        uint8_t cbf = (cbf_flags >> (nb_pb - i - 1)) & 0x1;

        /* On vertical ISP the intra prediction can only be performed with a min width of 4
           thus requiring to perform prediction on 2 PBs for size 2xX and all PBs in the
           case of 1xX. Even when transforms are smaller
         */
         /*FIXME separate small cases */
        if (!(offset_x & 0x3)) {
            vvc_intra_pred_isp(ctudec, &ctudec->rcn_ctx.ctu_buff.y[0],
                               RCN_CTB_STRIDE, intra_mode, x0, y0,
                               log2_pb_w >= 2 ? log2_pb_w : 2, log2_cb_h,
                               log2_cb_w, log2_cb_h, offset_x, 0);
        }

        /* FIXME determine if DBF is applied on ISP subblocks */
        #if 1
        if (!(offset_x & 0x3) ) {
            dbf_fill_qp_map(&ctudec->dbf_info.qp_map_y, x0, y0,  log2_pb_w >= 2 ? log2_pb_w : 2, log2_cb_h, ctudec->qp_ctx.current_qp);
            fill_ctb_bound(&ctudec->dbf_info, x0 & ~0x3, y0, log2_pb_w >= 2 ? log2_pb_w : 2, log2_cb_h);
            fill_bs_map(&ctudec->dbf_info.bs2_map, x0 & ~0x3, y0, log2_pb_w >= 2 ? log2_pb_w : 2, log2_cb_h);
        }
        //fill_ctb_bound(&ctudec->dbf_info, x0 & ~0x3, y0, log2_pb_w, log2_cb_h);
        #endif

        if (cbf) {
            int16_t *coeffs_y = ctudec->residual_y + i * (1 << (log2_pb_w + log2_cb_h));

            //fill_bs_map(&ctudec->dbf_info.bs1_map, x0, y0, log2_pb_w, log2_cb_h);

            if (log2_pb_w) {
                int shift_v = 6 + 1;
                int shift_h = (6 + 15 - 1) - 10;
                DECLARE_ALIGNED(32, int16_t, tmp)[64*64];
                int16_t *src = coeffs_y;
                int16_t *dst = ctudec->transform_buff;
                int cb_h = 1 << log2_cb_h;

                memset(tmp, 0, sizeof(int16_t) << (log2_pb_w + log2_cb_h));
                if (lfnst_flag) {
                    uint8_t lfnst_idx = tu_info->lfnst_idx;
                    int16_t lfnst_sb[16];
                    memcpy(lfnst_sb     , &coeffs_y[0], sizeof(int16_t) * 4);
                    memcpy(lfnst_sb +  4, &coeffs_y[1 << log2_pb_w], sizeof(int16_t) * 4);
                    memcpy(lfnst_sb +  8, &coeffs_y[2 << log2_pb_w], sizeof(int16_t) * 4);
                    memcpy(lfnst_sb + 12, &coeffs_y[3 << log2_pb_w], sizeof(int16_t) * 4);
                    process_lfnst_luma_isp(ctudec, coeffs_y, lfnst_sb, log2_pb_w, log2_cb_h,log2_cb_w, log2_cb_h, x0 -offset_x, y0,
                                       lfnst_idx);
                    /* lfnst forces IDCT II usage */
                    type_v = type_h = DCT_II;
                }

                TRFunc->func[type_v][OVMIN(log2_cb_h,6)](src, tmp, pb_w, pb_w, cb_h, shift_v);
                TRFunc->func[type_h][OVMIN(log2_pb_w,6)](tmp, dst, cb_h, cb_h, pb_w, shift_h);
            } else {
                int shift_h = (6 + 15 - 1) - 10;
                int cb_h = 1 << log2_cb_h;
                DECLARE_ALIGNED(32, int16_t, tmp)[64];

                memset(tmp, 0, sizeof(int16_t) << (log2_pb_w + log2_cb_h));

                TRFunc->func[type_v][OVMIN(log2_cb_h,6)](coeffs_y, tmp, pb_w, pb_w, cb_h, shift_h + 1);

                memcpy(ctudec->transform_buff, tmp, sizeof(uint16_t) * (1 << log2_cb_h));
            }

            uint16_t *dst  = &ctudec->rcn_ctx.ctu_buff.y[x0 + y0 * RCN_CTB_STRIDE];
            int16_t *src  = ctudec->transform_buff;

            rcn_func->ict.add[log2_pb_w](src, dst, log2_pb_w, log2_cb_h, 0);
        }
        x0 += pb_w;
        offset_x += pb_w;
    }
}

static void
recon_isp_subtree_h(OVCTUDec *const ctudec,
                    unsigned int x0, unsigned int y0,
                    unsigned int log2_cb_w, unsigned int log2_cb_h,
                    uint8_t intra_mode,
                    const struct ISPTUInfo *const tu_info)
{
    const struct TRFunctions *TRFunc = &ctudec->rcn_ctx.rcn_funcs.tr;
    const struct RCNFunctions *const rcn_func = &ctudec->rcn_ctx.rcn_funcs;

    int log2_pb_h = log2_cb_h - 2;
    int nb_pb;
    uint8_t cbf_flags = tu_info->cbf_mask;
    uint8_t lfnst_flag = tu_info->lfnst_flag;
    int pb_h, offset_y;

    // width < 16 imposes restrictions on split numbers
    if (log2_cb_w < 4 && (log2_pb_h <= (4 - log2_cb_w))) {
        log2_pb_h = 4 - log2_cb_w;
    }

    nb_pb = (1 << log2_cb_h) >> log2_pb_h;
    pb_h =  1 << log2_pb_h;
    offset_y = 0;

    uint8_t type_h = ctudec->mts_enabled && log2_cb_w <= 4 ? DST_VII : DCT_II;
    uint8_t type_v = ctudec->mts_enabled && log2_pb_h <= 4 && log2_pb_h > 1 ? DST_VII : DCT_II;
    int i;

    for (i = 0; i < nb_pb; ++i) {

        uint8_t cbf = (cbf_flags >> (nb_pb - i - 1)) & 0x1;

        /* FIXME determine if DBF is applied on ISP subblocks */
        #if 1
        if (!(offset_y & 0x3) ) {
            dbf_fill_qp_map(&ctudec->dbf_info.qp_map_y, x0, y0, log2_cb_w, log2_pb_h >=2 ? log2_pb_h : 2, ctudec->qp_ctx.current_qp);
            fill_ctb_bound(&ctudec->dbf_info, x0, y0, log2_cb_w, log2_pb_h >=2 ? log2_pb_h : 2);
            fill_bs_map(&ctudec->dbf_info.bs2_map, x0, y0, log2_cb_w, log2_pb_h >=2 ? log2_pb_h : 2);
        }
        //fill_ctb_bound(&ctudec->dbf_info, x0, y0, log2_cb_w, log2_pb_h);
        #endif

        vvc_intra_pred_isp(ctudec, &ctudec->rcn_ctx.ctu_buff.y[0],
                           RCN_CTB_STRIDE, intra_mode, x0, y0,
                           log2_cb_w, log2_pb_h, log2_cb_w, log2_cb_h, 0, offset_y);
        if (cbf) {
            int16_t *coeffs_y = ctudec->residual_y + i * (1 << (log2_cb_w + log2_pb_h));

            fill_bs_map(&ctudec->dbf_info.bs1_map, x0, y0, log2_cb_w, log2_pb_h);

            if (log2_pb_h) {
                DECLARE_ALIGNED(32, int16_t, tmp)[64*64];
                int shift_v = 6 + 1;
                int shift_h = (6 + 15 - 1) - 10;
                int16_t *src = coeffs_y;
                int16_t *dst = ctudec->transform_buff;
                int cb_w = 1 << log2_cb_w;

                memset(tmp, 0, sizeof(int16_t) << (log2_cb_w + log2_pb_h));

                if (lfnst_flag) {
                    uint8_t lfnst_idx = tu_info->lfnst_idx;
                    /*FIXME avoid distinguishing isp case in lfnst */
                    int16_t lfnst_sb[16];
                    memcpy(lfnst_sb     , &coeffs_y[0], sizeof(int16_t) * 4);
                    memcpy(lfnst_sb +  4, &coeffs_y[1 << log2_cb_w], sizeof(int16_t) * 4);
                    memcpy(lfnst_sb +  8, &coeffs_y[2 << log2_cb_w], sizeof(int16_t) * 4);
                    memcpy(lfnst_sb + 12, &coeffs_y[3 << log2_cb_w], sizeof(int16_t) * 4);
                    process_lfnst_luma_isp(ctudec, coeffs_y, lfnst_sb, log2_cb_w, log2_pb_h,log2_cb_w, log2_cb_h, x0, y0-offset_y,
                                       lfnst_idx);

                    /* lfnst forces IDCT II usage */
                    type_v = type_h = DCT_II;
                }

                TRFunc->func[type_v][OVMIN(log2_pb_h,6)](src, tmp, cb_w, cb_w, pb_h, shift_v);
                TRFunc->func[type_h][OVMIN(log2_cb_w,6)](tmp, dst, pb_h, pb_h, cb_w, shift_h);
            } else {
                int shift_h = (6 + 15 - 1) - 10;
                int cb_w = 1 << log2_cb_w;
                DECLARE_ALIGNED(32, int16_t, tmp)[64];

                memset(tmp, 0, sizeof(int16_t) << (log2_cb_w + log2_pb_h));

                TRFunc->func[type_h][OVMIN(log2_cb_w,6)](coeffs_y, tmp, pb_h, pb_h, cb_w, shift_h + 1);

                memcpy(ctudec->transform_buff, tmp, sizeof(uint16_t) * (1 << log2_cb_w));
            }

            uint16_t *dst  = &ctudec->rcn_ctx.ctu_buff.y[x0 + y0 * RCN_CTB_STRIDE];
            int16_t *src  = ctudec->transform_buff;

            rcn_func->ict.add[log2_cb_w](src, dst, log2_cb_w, log2_pb_h, 0);
        }
        y0 += pb_h;
        offset_y += pb_h;
    }
}

static uint8_t
jcbcr_lfnst_check(const struct TUInfo *tu_info, uint8_t log2_tb_w, uint8_t log2_tb_h)
{
    const struct TBInfo *const tb_info = &tu_info->tb_info[0];
    if (!tu_info->tr_skip_mask && tb_info->sig_sb_map == 0x1 && log2_tb_h > 1 && log2_tb_w > 1) {
        int max_lfnst_pos = (log2_tb_h == log2_tb_w) && (log2_tb_w <= 3) ? 7 : 15;

        uint64_t scan_map = 0xFDA6EB73C8419520;
        int last_y = tb_info->last_pos >> 8;
        int last_x = tb_info->last_pos & 0xFF;
        int nb_coeffs = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;

        uint8_t can_lfnst = nb_coeffs <= max_lfnst_pos;

        can_lfnst &= !!nb_coeffs;

        return can_lfnst;
    }
    return 0;
}

static uint8_t
chroma_lfnst_check(const struct TUInfo *tu_info, uint8_t cbf_mask, uint8_t log2_tb_w, uint8_t log2_tb_h)
{
    uint8_t can_lfnst = 0;
    const struct TBInfo *const tb_info_cb = &tu_info->tb_info[0];
    const struct TBInfo *const tb_info_cr = &tu_info->tb_info[1];

    if (log2_tb_w > 1 && log2_tb_h > 1 && tb_info_cb->sig_sb_map == 0x1 && tb_info_cr->sig_sb_map == 0x1 && !tu_info->tr_skip_mask) {
        uint8_t need_cb_chk = cbf_mask & 0x2;
        uint8_t need_cr_chk = cbf_mask & 0x1;

        int max_lfnst_pos = (log2_tb_h == log2_tb_w) && (log2_tb_w <= 3) ? 7 : 15;

        uint64_t scan_map = 0xfda6eb73c8419520;

        can_lfnst = !!cbf_mask;

        /*fixme better can_lfnst derivation */
        if (need_cb_chk && need_cr_chk) {
            int last_y_cb = tb_info_cb->last_pos >> 8;
            int last_x_cb = tb_info_cb->last_pos & 0xff;
            int last_y_cr = tb_info_cr->last_pos >> 8;
            int last_x_cr = tb_info_cr->last_pos & 0xff;
            int nb_coeffs_cb = (scan_map >> ((last_x_cb + (last_y_cb << 2)) << 2)) & 0xf;
            int nb_coeffs_cr = (scan_map >> ((last_x_cr + (last_y_cr << 2)) << 2)) & 0xf;
            can_lfnst &= nb_coeffs_cr <= max_lfnst_pos && nb_coeffs_cb <= max_lfnst_pos;
            can_lfnst &= !!(nb_coeffs_cr | nb_coeffs_cb);
        } else if (need_cb_chk) {
            int last_y_cb = tb_info_cb->last_pos >> 8;
            int last_x_cb = tb_info_cb->last_pos & 0xff;
            int nb_coeffs_cb = (scan_map >> ((last_x_cb + (last_y_cb << 2)) << 2)) & 0xf;
            can_lfnst &= nb_coeffs_cb <= max_lfnst_pos;
            can_lfnst &= !!nb_coeffs_cb;
        } else {
            int last_y_cr = tb_info_cr->last_pos >> 8;
            int last_x_cr = tb_info_cr->last_pos & 0xff;
            int nb_coeffs_cr = (scan_map >> ((last_x_cr + (last_y_cr << 2)) << 2)) & 0xf;
            can_lfnst &= nb_coeffs_cr <= max_lfnst_pos;
            can_lfnst &= !!nb_coeffs_cr;
        }
    }

    return can_lfnst;
}

static uint8_t
lfnst_check_st(const struct TUInfo *const tu_info, uint8_t log2_tb_w, uint8_t log2_tb_h,
               uint8_t cbf_mask, uint8_t cu_flags)
{
    uint8_t cbf_flag_l  = cbf_mask & 0x10;
    uint8_t jcbcr_flag  = cbf_mask & 0x8;
    uint8_t cbf_mask_cb = cbf_mask & 0x2;
    uint8_t cbf_mask_cr = cbf_mask & 0x1;
    uint8_t non_only_dc = 0;
    /* FIXME chroma size ? */
    const uint8_t max_lfnst_pos   = (log2_tb_h == log2_tb_w) && (log2_tb_w <= 3) ? 7 : 15;
    const uint8_t max_lfnst_pos_c = (log2_tb_h == log2_tb_w) && (log2_tb_w <= 4) ? 7 : 15;

    /*FIXME MIP check on ly for luma TB*/
    uint8_t is_mip = !!(cu_flags & flg_mip_flag);
    uint8_t mip_lfnst = !is_mip || (log2_tb_h >= 4 && log2_tb_w >= 4);

    uint8_t can_lfnst = mip_lfnst;

    can_lfnst &= !(tu_info->tr_skip_mask);

    /* FIXME Note that sig_sb_map check is sufficient if max is 15 */
    if (can_lfnst) {
        static const uint64_t scan_map = 0xFDA6EB73C8419520;
        if (cbf_flag_l) {
            const struct TBInfo *tb_info = &tu_info->tb_info[2];
            int last_y = tb_info->last_pos >> 8;
            int last_x = tb_info->last_pos & 0xFF;
            int nb_coeffs = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;

            can_lfnst &= tb_info->sig_sb_map == 0x1;
            can_lfnst &= nb_coeffs <= max_lfnst_pos;
            non_only_dc |= nb_coeffs;
        }

        if (jcbcr_flag && log2_tb_h > 2 && log2_tb_w > 2) {
            const struct TBInfo *const tb_info_cbcr = &tu_info->tb_info[0];
            int last_y_cbcr = tb_info_cbcr->last_pos >> 8;
            int last_x_cbcr = tb_info_cbcr->last_pos & 0xff;
            int nb_coeffs_cbcr = (scan_map >> ((last_x_cbcr + (last_y_cbcr << 2)) << 2)) & 0xf;
            can_lfnst &= tb_info_cbcr->sig_sb_map == 0x1;
            can_lfnst &= nb_coeffs_cbcr <= max_lfnst_pos_c;
            non_only_dc |= nb_coeffs_cbcr;
        } else {
            if (cbf_mask_cb && log2_tb_h > 2 && log2_tb_w > 2) {
                const struct TBInfo *const tb_info_cb = &tu_info->tb_info[0];
                int last_y_cb = tb_info_cb->last_pos >> 8;
                int last_x_cb = tb_info_cb->last_pos & 0xff;
                int nb_coeffs_cb = (scan_map >> ((last_x_cb + (last_y_cb << 2)) << 2)) & 0xf;
                can_lfnst &= tb_info_cb->sig_sb_map == 0x1;
                can_lfnst &= nb_coeffs_cb <= max_lfnst_pos_c;
                non_only_dc |= nb_coeffs_cb;
            }

            if (cbf_mask_cr && log2_tb_h > 2 && log2_tb_w > 2) {
                const struct TBInfo *const tb_info_cr = &tu_info->tb_info[1];
                int last_y_cr = tb_info_cr->last_pos >> 8;
                int last_x_cr = tb_info_cr->last_pos & 0xff;
                int nb_coeffs_cr = (scan_map >> ((last_x_cr + (last_y_cr << 2)) << 2)) & 0xf;
                can_lfnst &= tb_info_cr->sig_sb_map == 0x1;
                can_lfnst &= nb_coeffs_cr <= max_lfnst_pos_c;
                non_only_dc |= nb_coeffs_cr;
            }
        }

        can_lfnst &= !!non_only_dc;

    }

    return can_lfnst;
}

static int
residual_coding_l(OVCTUDec *const ctu_dec,
                  unsigned int x0, unsigned int y0,
                  unsigned int log2_tb_w, unsigned int log2_tb_h,
                  uint8_t cu_flags, struct TUInfo *const tu_info)
{
    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
    uint8_t tr_skip_flag = 0;
    int16_t *const coeffs_y = ctu_dec->residual_y + tu_info->pos_offset;

    struct TBInfo *tb_info = &tu_info->tb_info[2];

    if (ctu_dec->transform_skip_enabled && log2_tb_w <= ctu_dec->max_log2_transform_skip_size
        && log2_tb_h <= ctu_dec->max_log2_transform_skip_size && !tu_info->is_sbt && !(cu_flags &flg_isp_flag)) {
        tr_skip_flag = ovcabac_read_ae_transform_skip_luma_flag(cabac_ctx);
        tu_info->tr_skip_mask |= tr_skip_flag << 4;
    }


    if (!tr_skip_flag) {
        uint16_t last_pos;
        if (tu_info->is_sbt && tu_info->cu_mts_flag && log2_tb_w <= 5 && log2_tb_h <= 5) {
            last_pos = ovcabac_read_ae_last_sig_pos_red(cabac_ctx, log2_tb_w, log2_tb_h);
            uint8_t log2_red_w = log2_tb_w == 5 ? 4 : log2_tb_w;
            uint8_t log2_red_h = log2_tb_h == 5 ? 4 : log2_tb_h;

            /* FIXME recheck this */
            ctu_dec->tmp_red  =  log2_tb_w - log2_red_w;
            ctu_dec->tmp_red |= (log2_tb_h - log2_red_h) << 1;

            memset(coeffs_y, 0, sizeof(int16_t) << (log2_tb_h + log2_tb_w));
        } else {
            last_pos = ovcabac_read_ae_last_sig_pos(cabac_ctx, log2_tb_w, log2_tb_h);
        }

        uint64_t sig_sb_map = ctu_dec->residual_coding(ctu_dec, coeffs_y, log2_tb_w, log2_tb_h,
                                                       last_pos);

        ctu_dec->tmp_red  = 0;
        tb_info->sig_sb_map = sig_sb_map;
        tb_info->last_pos   = last_pos;


    } else {
        ctu_dec->dequant_skip = &ctu_dec->dequant_luma_skip;
        residual_coding_ts(ctu_dec, ctu_dec->residual_y + tu_info->pos_offset, log2_tb_w, log2_tb_h);
    }


    return 0;
}

static int
residual_coding_c(OVCTUDec *const ctu_dec,
                  unsigned int x0, unsigned int y0,
                  unsigned int log2_tb_w, unsigned int log2_tb_h,
                  uint8_t cbf_mask, struct TUInfo *tu_info)
{

    int16_t *const coeffs_cb = ctu_dec->residual_cb + tu_info->pos_offset;
    int16_t *const coeffs_cr = ctu_dec->residual_cr + tu_info->pos_offset;
    uint16_t last_pos_cb = (1 << (log2_tb_h + log2_tb_w)) - 1;
    uint16_t last_pos_cr = (1 << (log2_tb_h + log2_tb_w)) - 1;
    uint32_t sig_sb_map_cb = 0x1;
    uint32_t sig_sb_map_cr = 0x1;
    uint8_t transform_skip_flag = 0;
    struct TBInfo *const tb_info_cb = &tu_info->tb_info[0];
    struct TBInfo *const tb_info_cr = &tu_info->tb_info[1];


    /* FIXME move dequant to reconstruction this require modification in coeff
       reading */

    if (cbf_mask & 0x2) {
        OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;

        /* TODO use max_tr_skip_s = 0 to avoid using enabled test */
        if (ctu_dec->transform_skip_enabled && log2_tb_w <= ctu_dec->max_log2_transform_skip_size
            && log2_tb_h <= ctu_dec->max_log2_transform_skip_size && !tu_info->is_sbt) {
            OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
            transform_skip_flag |= ovcabac_read_ae_transform_skip_flag_c(cabac_ctx) << 1;
        }

        if (!(transform_skip_flag & 0x2)) {
            ctu_dec->dequant_chroma = &ctu_dec->dequant_cb;

            last_pos_cb = ovcabac_read_ae_last_sig_pos_c(cabac_ctx, log2_tb_w, log2_tb_h);

            sig_sb_map_cb = ctu_dec->residual_coding_chroma(ctu_dec, coeffs_cb,
                                                            log2_tb_w, log2_tb_h,
                                                            last_pos_cb);

        }  else {
            /* FIXME Chroma TS */
            ctu_dec->dequant_skip = &ctu_dec->dequant_cb_skip;
            residual_coding_ts(ctu_dec, coeffs_cb, log2_tb_w, log2_tb_h);
        }
    }

    if (cbf_mask & 0x1) {
        OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;

        /* TODO use max_tr_skip_s = 0 to avoid using enabled test */
        if (ctu_dec->transform_skip_enabled && log2_tb_w <= ctu_dec->max_log2_transform_skip_size
            && log2_tb_h <= ctu_dec->max_log2_transform_skip_size && !tu_info->is_sbt) {
            OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
            transform_skip_flag |= ovcabac_read_ae_transform_skip_flag_c(cabac_ctx);
        }

        if (!(transform_skip_flag & 0x1)) {
            ctu_dec->dequant_chroma = &ctu_dec->dequant_cr;

            last_pos_cr = ovcabac_read_ae_last_sig_pos_c(cabac_ctx, log2_tb_w, log2_tb_h);

            sig_sb_map_cr = ctu_dec->residual_coding_chroma(ctu_dec, coeffs_cr,
                                                            log2_tb_w, log2_tb_h,
                                                            last_pos_cr);
        } else {
            ctu_dec->dequant_skip = &ctu_dec->dequant_cr_skip;
            residual_coding_ts(ctu_dec, coeffs_cr, log2_tb_w, log2_tb_h);
        }
    }

    tu_info->tr_skip_mask  |= transform_skip_flag;
    tb_info_cb->last_pos   = last_pos_cb;
    tb_info_cr->last_pos   = last_pos_cr;
    tb_info_cb->sig_sb_map = sig_sb_map_cb;
    tb_info_cr->sig_sb_map = sig_sb_map_cr;

    return 0;
}

static int
residual_coding_jcbcr(OVCTUDec *const ctu_dec,
                      unsigned int x0, unsigned int y0,
                      unsigned int log2_tb_w, unsigned int log2_tb_h,
                      uint8_t cbf_mask, struct TUInfo *tu_info)
{
    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;

    uint16_t last_pos;
    int16_t *const coeffs_jcbcr = ctu_dec->residual_cb + tu_info->pos_offset;
    uint32_t sig_sb_map = 0x1;
    uint8_t transform_skip_flag = 0;
    struct TBInfo *const tb_info = &tu_info->tb_info[0];

    if (ctu_dec->transform_skip_enabled && log2_tb_w <= ctu_dec->max_log2_transform_skip_size
        && log2_tb_h <= ctu_dec->max_log2_transform_skip_size && !tu_info->is_sbt) {
        OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
        transform_skip_flag = ovcabac_read_ae_transform_skip_flag_c(cabac_ctx);
        tu_info->tr_skip_mask |= transform_skip_flag;
    }

    /* FIXME this is hackish joint cb cr involves a different delta qp from
       previous ones */
    if (cbf_mask == 3) {
        ctu_dec->dequant_chroma = &ctu_dec->dequant_joint_cb_cr;
        ctu_dec->dequant_skip = &ctu_dec->dequant_jcbcr_skip;
    } else if (cbf_mask == 1) {
        ctu_dec->dequant_chroma = &ctu_dec->dequant_cr;
        ctu_dec->dequant_skip   = &ctu_dec->dequant_cr_skip;
    } else {
        ctu_dec->dequant_chroma = &ctu_dec->dequant_cb;
        ctu_dec->dequant_skip   = &ctu_dec->dequant_cb_skip;
    }

    if (!transform_skip_flag) {
        last_pos = ovcabac_read_ae_last_sig_pos_c(cabac_ctx, log2_tb_w, log2_tb_h);

        sig_sb_map = ctu_dec->residual_coding_chroma(ctu_dec, coeffs_jcbcr, log2_tb_w, log2_tb_h,
                                                     last_pos);
    } else {
        residual_coding_ts(ctu_dec, ctu_dec->residual_cb + tu_info->pos_offset, log2_tb_w, log2_tb_h);
    }

    tb_info->sig_sb_map = sig_sb_map;
    tb_info->last_pos   = last_pos;

    return 0;
}

static void
rcn_tu_st(OVCTUDec *const ctu_dec,
          uint8_t x0, uint8_t y0,
          uint8_t log2_tb_w, uint8_t log2_tb_h,
          uint8_t cu_flags, uint8_t cbf_mask,
          const struct TUInfo *const tu_info)
{
    uint8_t cbf_flag_l = cbf_mask & 0x10;
    uint8_t jcbcr_flag = cbf_mask & 0x8;
    uint8_t cbf_mask_c = cbf_mask & 0x3;

    if (cbf_flag_l) {
        const struct TBInfo *const tb_info = &tu_info->tb_info[2];

        if (!(tu_info->tr_skip_mask & 0x10)) {
            int lim_sb_s = ((((tb_info->last_pos >> 8)) >> 2) + (((tb_info->last_pos & 0xFF))>> 2) + 1) << 2;
            int16_t *const coeffs_y = ctu_dec->residual_y + tu_info->pos_offset;
            uint8_t is_mip = !!(cu_flags & flg_mip_flag);
            rcn_residual(ctu_dec, ctu_dec->transform_buff, coeffs_y, x0, y0, log2_tb_w, log2_tb_h,
                         lim_sb_s, tu_info->cu_mts_flag, tu_info->cu_mts_idx,
                         !tb_info->last_pos, tu_info->lfnst_flag, is_mip, tu_info->lfnst_idx, tu_info->is_sbt);

        } else {
            int16_t *const coeffs_y = ctu_dec->residual_y + tu_info->pos_offset;
            memcpy(ctu_dec->transform_buff, coeffs_y, sizeof(int16_t) << (log2_tb_w + log2_tb_h));
        }

        /* FIXME use transform add optimization */
        vvc_add_residual(ctu_dec->transform_buff, &ctu_dec->rcn_ctx.ctu_buff.y[x0 + y0 * RCN_CTB_STRIDE], log2_tb_w, log2_tb_h, 0);
        /* FIXME Avoid reprocessing CCLM from here by recontructing at the end of transform tree */
        if (ctu_dec->intra_mode_c >= 67 && ctu_dec->intra_mode_c < 70) {
            vvc_intra_pred_chroma(&ctu_dec->rcn_ctx, &ctu_dec->rcn_ctx.ctu_buff, ctu_dec->intra_mode_c, x0 >> 1, y0 >> 1, log2_tb_w - 1, log2_tb_h - 1);
        }
        fill_bs_map(&ctu_dec->dbf_info.bs1_map, x0, y0, log2_tb_w, log2_tb_h);
        fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    }

    if (jcbcr_flag) {

        rcn_jcbcr(ctu_dec, tu_info, x0 >> 1, y0 >> 1, log2_tb_w - 1, log2_tb_h - 1, cbf_mask_c, 0);

    } else if (cbf_mask_c) {

        rcn_res_c(ctu_dec, tu_info, x0 >> 1, y0 >> 1, log2_tb_w - 1, log2_tb_h - 1, cbf_mask_c, 0);

    }

    derive_dequant_ctx(ctu_dec, &ctu_dec->qp_ctx, 0);
    struct DBFInfo *dbf_info = &ctu_dec->dbf_info;
    uint8_t qp_l  = ctu_dec->qp_ctx.current_qp;
    uint8_t qp_jc = ctu_dec->dequant_joint_cb_cr.qp - 12;
    uint8_t qp_cb = (cbf_mask & 0x3) == 0x3 && jcbcr_flag ? qp_jc : ctu_dec->dequant_cb.qp - 12;
    uint8_t qp_cr = (cbf_mask & 0x3) == 0x3 && jcbcr_flag ? qp_jc : ctu_dec->dequant_cr.qp - 12;

    dbf_fill_qp_map(&dbf_info->qp_map_y, x0, y0, log2_tb_w, log2_tb_h, qp_l);
    dbf_fill_qp_map(&dbf_info->qp_map_cb, x0, y0, log2_tb_w, log2_tb_h, qp_cb);
    dbf_fill_qp_map(&dbf_info->qp_map_cr, x0, y0, log2_tb_w, log2_tb_h, qp_cr);
}

static void
rcn_tu_l(OVCTUDec *const ctu_dec,
         uint8_t x0, uint8_t y0,
         uint8_t log2_tb_w, uint8_t log2_tb_h,
         uint8_t cu_flags, uint8_t cbf_mask,
         const struct TUInfo *const tu_info)
{
    const struct TBInfo *const tb_info = &tu_info->tb_info[2];
    if (cbf_mask) {
        if (!(tu_info->tr_skip_mask & 0x10)) {
            int lim_sb_s = ((((tb_info->last_pos >> 8)) >> 2) + (((tb_info->last_pos & 0xFF))>> 2) + 1) << 2;
            int16_t *const coeffs_y = ctu_dec->residual_y + tu_info->pos_offset;
            uint8_t is_mip = !!(cu_flags & flg_mip_flag);
            rcn_residual(ctu_dec, ctu_dec->transform_buff, coeffs_y, x0, y0, log2_tb_w, log2_tb_h,
                         lim_sb_s, tu_info->cu_mts_flag, tu_info->cu_mts_idx,
                         !tb_info->last_pos, tu_info->lfnst_flag, is_mip, tu_info->lfnst_idx, tu_info->is_sbt);

        } else {
            int16_t *const coeffs_y = ctu_dec->residual_y + tu_info->pos_offset;
            memcpy(ctu_dec->transform_buff, coeffs_y, sizeof(int16_t) << (log2_tb_w + log2_tb_h));
        }

        fill_bs_map(&ctu_dec->dbf_info.bs1_map, x0, y0, log2_tb_w, log2_tb_h);
        fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);

        /* FIXME use transform add optimization */
        vvc_add_residual(ctu_dec->transform_buff, &ctu_dec->rcn_ctx.ctu_buff.y[x0 + y0 * RCN_CTB_STRIDE], log2_tb_w, log2_tb_h, 0);
    }
}

static void
rcn_tu_c(OVCTUDec *const ctu_dec, uint8_t x0, uint8_t y0,
         uint8_t log2_tb_w, uint8_t log2_tb_h,
         uint8_t cu_flags, uint8_t cbf_mask,
         const struct TUInfo *const tu_info)
{
    uint8_t jcbcr_flag = cbf_mask & 0x8;
    uint8_t cbf_mask_c = cbf_mask & 0x3;

    if (jcbcr_flag) {

        rcn_jcbcr(ctu_dec, tu_info, x0, y0, log2_tb_w, log2_tb_h, cbf_mask_c, tu_info->lfnst_flag);

    } else if (cbf_mask_c) {

        rcn_res_c(ctu_dec, tu_info, x0, y0, log2_tb_w, log2_tb_h, cbf_mask_c, tu_info->lfnst_flag);

    }
}


int
transform_unit_st(OVCTUDec *const ctu_dec,
                  unsigned int x0, unsigned int y0,
                  unsigned int log2_tb_w, unsigned int log2_tb_h,
                  uint8_t rqt_root_cbf, uint8_t cu_flags, uint8_t tr_depth,
                  struct TUInfo *const tu_info)
{
    uint8_t cbf_mask = decode_cbf_st(ctu_dec, rqt_root_cbf, tr_depth, cu_flags);

    uint8_t cbf_flag_l = cbf_mask & 0x10;
    uint8_t jcbcr_flag = cbf_mask & 0x8;
    uint8_t cbf_mask_c = cbf_mask & 0x3;

    if (cbf_mask) {
        /* FIXME delta_qp is only read on first significant TU in CU */
        if (ctu_dec->delta_qp_enabled && cbf_mask) {
            OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
            int cu_qp_delta = ovcabac_read_ae_cu_delta_qp(cabac_ctx);
            derive_dequant_ctx(ctu_dec, &ctu_dec->qp_ctx, cu_qp_delta);
        }

        if (cbf_flag_l) {

            residual_coding_l(ctu_dec, x0, y0, log2_tb_w, log2_tb_h, cu_flags, tu_info);

            if (ctu_dec->tmp_ciip) {
                fill_bs_map(&ctu_dec->dbf_info.bs2_map, x0, y0, log2_tb_w, log2_tb_h);
            }

        }

        if (jcbcr_flag) {

            residual_coding_jcbcr(ctu_dec, x0 >> 1, y0 >> 1, log2_tb_w - 1,
                                  log2_tb_h - 1, cbf_mask_c, tu_info);

            if (ctu_dec->tmp_ciip) {
                fill_bs_map(&ctu_dec->dbf_info.bs2_map_c, x0, y0, log2_tb_w, log2_tb_h);
            }

        } else if (cbf_mask_c) {

            residual_coding_c(ctu_dec, x0 >> 1, y0 >> 1, log2_tb_w - 1,
                              log2_tb_h - 1, cbf_mask_c, tu_info);

            if (ctu_dec->tmp_ciip) {
                fill_bs_map(&ctu_dec->dbf_info.bs2_map_c, x0, y0, log2_tb_w, log2_tb_h);
            }
        }

    }
    return cbf_mask;
}

int
transform_unit_l(OVCTUDec *const ctu_dec,
                 unsigned int x0, unsigned int y0,
                 unsigned int log2_tb_w, unsigned int log2_tb_h,
                 uint8_t rqt_root_cbf, uint8_t cu_flags, uint8_t tr_depth,
                 struct TUInfo *const tu_info)
{
    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
    uint8_t cbf_mask = ovcabac_read_ae_tu_cbf_luma(cabac_ctx);

    if (cbf_mask) {
        if (ctu_dec->delta_qp_enabled && cbf_mask) {
            int cu_qp_delta = ovcabac_read_ae_cu_delta_qp(cabac_ctx);
            derive_dequant_ctx(ctu_dec, &ctu_dec->qp_ctx, cu_qp_delta);
        }

        residual_coding_l(ctu_dec, x0, y0, log2_tb_w, log2_tb_h, cu_flags, tu_info);

    }

    return cbf_mask;
}

int
transform_unit_c(OVCTUDec *const ctu_dec,
                 unsigned int x0, unsigned int y0,
                 unsigned int log2_tb_w, unsigned int log2_tb_h,
                 uint8_t rqt_root_cbf, uint8_t cu_flags, uint8_t tr_depth,
                 struct TUInfo *const tu_info)
{
    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
    uint8_t cbf_mask = decode_cbf_c(ctu_dec);
    uint8_t jcbcr_flag = cbf_mask & 0x8;
    uint8_t cbf_mask_c = cbf_mask & 0x3;

    if (cbf_mask) {
        /* FIXME Read only on first TU */
        if (ctu_dec->delta_qp_enabled && cbf_mask) {
            int cu_qp_delta = ovcabac_read_ae_cu_delta_qp(cabac_ctx);
            derive_dequant_ctx(ctu_dec, &ctu_dec->qp_ctx, cu_qp_delta);
        }

        if (jcbcr_flag) {

            residual_coding_jcbcr(ctu_dec, x0, y0, log2_tb_w, log2_tb_h,
                                  cbf_mask_c, tu_info);
        } else if (cbf_mask_c) {

            residual_coding_c(ctu_dec, x0, y0, log2_tb_w, log2_tb_h,
                              cbf_mask_c, tu_info);
        }
    }

    return cbf_mask;
}

static void
lfnst_mts(const OVCTUDec *const ctu_dec, uint8_t log2_tb_w, uint8_t log2_tb_h,
          uint8_t cu_flags, struct TUInfo *const tu_info)
{
    uint8_t cbf_mask = tu_info->cbf_mask;
    if (ctu_dec->transform_unit == &transform_unit_st && cbf_mask) {
        if (ctu_dec->enable_lfnst && cu_flags & 0x2) {
            uint8_t can_lfnst = lfnst_check_st(tu_info, log2_tb_w, log2_tb_h,
                                               cbf_mask, cu_flags);
            if (can_lfnst) {
                OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
                uint8_t is_dual = ctu_dec->transform_unit != &transform_unit_st;
                uint8_t lfnst_flag = ovcabac_read_ae_lfnst_flag(cabac_ctx, is_dual);
                tu_info->lfnst_flag = lfnst_flag;
                if (lfnst_flag) {
                    uint8_t lfnst_idx = ovcabac_read_ae_lfnst_idx(cabac_ctx);
                    tu_info->lfnst_idx = lfnst_idx;
                }
            }
        }

        if (ctu_dec->mts_enabled && (cu_flags & 0x2)) {
            const struct TBInfo *tb_info = &tu_info->tb_info[2];
            if (!tu_info->lfnst_flag && !!tb_info->last_pos && (log2_tb_w < 6) && (log2_tb_h < 6)
                && !(tb_info->sig_sb_map & (~0x000000000F0F0F0F))) {
                OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
                uint8_t cu_mts_flag = ovcabac_read_ae_cu_mts_flag(cabac_ctx);
                tu_info->cu_mts_flag = cu_mts_flag;
                if (cu_mts_flag) {
                    uint8_t cu_mts_idx = ovcabac_read_ae_cu_mts_idx(cabac_ctx);
                    tu_info->cu_mts_idx = cu_mts_idx;
                }
            }
        }
    } else if (ctu_dec->transform_unit == &transform_unit_l && cbf_mask) {
        if (!(tu_info->tr_skip_mask & 0x10)) {
            const struct TBInfo *tb_info = &tu_info->tb_info[2];
            /* FIXME use sb_sig_map instead of last pos */
            if (ctu_dec->enable_lfnst && cu_flags & 0x2 && tb_info->sig_sb_map == 0x1) {
                int max_lfnst_pos = (log2_tb_h == log2_tb_w) && (log2_tb_w <= 3) ? 7 : 15;
                int last_y = tb_info->last_pos >> 8;
                int last_x = tb_info->last_pos & 0xFF;
                uint64_t scan_map = 0xFDA6EB73C8419520;
                int nb_coeffs = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;
                uint8_t is_mip = !!(cu_flags & flg_mip_flag);
                uint8_t allow_mip_lfnst = !is_mip || (log2_tb_h >= 4 && log2_tb_w >= 4);

                if (allow_mip_lfnst && nb_coeffs <= max_lfnst_pos && !!tb_info->last_pos) {
                    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
                    uint8_t is_dual = ctu_dec->transform_unit != &transform_unit_st;
                    uint8_t lfnst_flag = ovcabac_read_ae_lfnst_flag(cabac_ctx, is_dual);
                    tu_info->lfnst_flag = lfnst_flag;
                    if (lfnst_flag) {
                        uint8_t lfnst_idx = ovcabac_read_ae_lfnst_idx(cabac_ctx);
                        tu_info->lfnst_idx = lfnst_idx;
                    }
                }
            }

            if (!tu_info->lfnst_flag && !!tb_info->last_pos && (cu_flags & 0x2)
                && ctu_dec->mts_enabled && (log2_tb_w < 6) && (log2_tb_h < 6)
                && !(tb_info->sig_sb_map & (~0x000000000F0F0F0F))) {
                OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;

                uint8_t cu_mts_flag = ovcabac_read_ae_cu_mts_flag(cabac_ctx);
                tu_info->cu_mts_flag = cu_mts_flag;
                if (cu_mts_flag) {
                    uint8_t cu_mts_idx = ovcabac_read_ae_cu_mts_idx(cabac_ctx);
                    tu_info->cu_mts_idx = cu_mts_idx;
                }
            }
        }
    } else if (ctu_dec->transform_unit == &transform_unit_c && cbf_mask) {
        if (ctu_dec->enable_lfnst) {
            uint8_t jcbcr_flag = cbf_mask & 0x8;
            uint8_t cbf_mask_c = cbf_mask & 0x3;
            uint8_t can_lfnst = jcbcr_flag ? jcbcr_lfnst_check(tu_info, log2_tb_w, log2_tb_h)
                : chroma_lfnst_check(tu_info, cbf_mask_c, log2_tb_w, log2_tb_h);

            if (can_lfnst) {
                OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
                uint8_t is_dual = ctu_dec->transform_unit != &transform_unit_st;
                uint8_t lfnst_flag = ovcabac_read_ae_lfnst_flag(cabac_ctx, is_dual);
                tu_info->lfnst_flag = lfnst_flag;
                if (lfnst_flag) {
                    uint8_t lfnst_idx = ovcabac_read_ae_lfnst_idx(cabac_ctx);
                    tu_info->lfnst_idx = lfnst_idx;
                }
            }
        }
    }
}

static void
rcn_res_wrap(OVCTUDec *const ctu_dec, uint8_t x0, uint8_t y0,
             uint8_t log2_tb_w, uint8_t log2_tb_h, uint8_t cu_flags,
             const struct TUInfo *const tu_info)
{
    uint8_t cbf_mask = tu_info->cbf_mask;
    if (ctu_dec->transform_unit == &transform_unit_st && cbf_mask) {
        rcn_tu_st(ctu_dec, x0, y0, log2_tb_w, log2_tb_h, cu_flags, cbf_mask, tu_info);
    } else if (ctu_dec->transform_unit == &transform_unit_l && cbf_mask) {
        rcn_tu_l(ctu_dec, x0, y0, log2_tb_w, log2_tb_h, cu_flags, cbf_mask, tu_info);
    } else if (ctu_dec->transform_unit == &transform_unit_c && cbf_mask) {
        rcn_tu_c(ctu_dec, x0, y0, log2_tb_w, log2_tb_h, cu_flags, cbf_mask, tu_info);
    }
}

static void
rcn_transform_tree(OVCTUDec *const ctu_dec, uint8_t x0, uint8_t y0,
                   uint8_t log2_tb_w, uint8_t log2_tb_h, uint8_t log2_max_tb_s,
                   uint8_t cu_flags, const struct TUInfo *const tu_info)
{
    uint8_t split_v = log2_tb_w > log2_max_tb_s;
    uint8_t split_h = log2_tb_h > log2_max_tb_s;

    if (split_v || split_h) {
        unsigned int tb_w1 = ((1 << log2_tb_w) >> split_v);
        unsigned int tb_h1 = ((1 << log2_tb_h) >> split_h);

        unsigned int log2_tb_w1 = log2_tb_w - split_v;
        unsigned int log2_tb_h1 = log2_tb_h - split_h;

        rcn_transform_tree(ctu_dec, x0, y0,
                           log2_tb_w1, log2_tb_h1,
                           log2_max_tb_s, cu_flags, &tu_info[0]);
        if (split_v) {
            rcn_transform_tree(ctu_dec, x0 + tb_w1, y0,
                               log2_tb_w1, log2_tb_h1,
                               log2_max_tb_s, cu_flags, &tu_info[1]);
        }

        if (split_h) {
            rcn_transform_tree(ctu_dec, x0, y0 + tb_h1,
                               log2_tb_w1, log2_tb_h1,
                               log2_max_tb_s, cu_flags, &tu_info[2]);
        }

        if (split_h && split_v) {
            rcn_transform_tree(ctu_dec, x0 + tb_w1, y0 + tb_h1,
                               log2_tb_w1, log2_tb_h1,
                               log2_max_tb_s, cu_flags, &tu_info[3]);
        }

    } else {
        rcn_res_wrap(ctu_dec, x0, y0, log2_tb_w, log2_tb_h, cu_flags, tu_info);
    }
}

static int
transform_tree(OVCTUDec *const ctu_dec,
               const OVPartInfo *const part_ctx,
               unsigned int x0, unsigned int y0,
               unsigned int log2_tb_w, unsigned int log2_tb_h,
               unsigned int log2_max_tb_s, uint8_t rqt_root_cbf,
               uint8_t cu_flags, uint8_t tr_depth, struct TUInfo *tu_info)
{
    uint8_t split_v = log2_tb_w > log2_max_tb_s;
    uint8_t split_h = log2_tb_h > log2_max_tb_s;
    uint8_t cbf_mask = 0;

    if (split_v || split_h) {
        unsigned int tb_w1 = ((1 << log2_tb_w) >> split_v);
        unsigned int tb_h1 = ((1 << log2_tb_h) >> split_h);

        unsigned int log2_tb_w1 = log2_tb_w - split_v;
        unsigned int log2_tb_h1 = log2_tb_h - split_h;
        int residual_offset = 0;

        transform_tree(ctu_dec, part_ctx, x0, y0,
                       log2_tb_w1, log2_tb_h1,
                       log2_max_tb_s, rqt_root_cbf, cu_flags, tr_depth + 1, &tu_info[0]);

        residual_offset += 1 << (log2_tb_w1 + log2_tb_h1);

        if (split_v) {
            tu_info[1].pos_offset = residual_offset;
            transform_tree(ctu_dec, part_ctx, x0 + tb_w1, y0,
                           log2_tb_w1, log2_tb_h1,
                           log2_max_tb_s, rqt_root_cbf, cu_flags, tr_depth + 1, &tu_info[1]);

            residual_offset += 1 << (log2_tb_w1 + log2_tb_h1);
        }

        if (split_h) {
            tu_info[2].pos_offset = residual_offset;
            transform_tree(ctu_dec, part_ctx, x0, y0 + tb_h1,
                           log2_tb_w1, log2_tb_h1,
                           log2_max_tb_s, rqt_root_cbf, cu_flags, tr_depth + 1, &tu_info[2]);

            residual_offset += 1 << (log2_tb_w1 + log2_tb_h1);
        }

        if (split_h && split_v) {
            tu_info[3].pos_offset = residual_offset;
            transform_tree(ctu_dec, part_ctx, x0 + tb_w1, y0 + tb_h1,
                           log2_tb_w1, log2_tb_h1,
                           log2_max_tb_s, rqt_root_cbf, cu_flags, tr_depth + 1, &tu_info[3]);
        }


    } else {

        cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w, log2_tb_h, rqt_root_cbf, cu_flags, tr_depth, tu_info);
        tu_info->cbf_mask = cbf_mask;

    }

    return 0;
}

static int
sbt_half_ver(OVCTUDec *const ctu_dec,
             const OVPartInfo *const part_ctx,
             unsigned int x0, unsigned int y0,
             unsigned int log2_tb_w, unsigned int log2_tb_h,
             uint8_t sbt_pos, uint8_t cu_flags, struct TUInfo *tu_info)
{
    fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    if (!sbt_pos) {
        tu_info[0].is_sbt = 1;
        if (log2_tb_w - 1 <= 5 && log2_tb_h <= 5) {
            tu_info->cu_mts_flag = 1;
            tu_info->cu_mts_idx = 0x1;
        }
        uint8_t cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w - 1, log2_tb_h,
                                                   1, cu_flags, 0, tu_info);

        rcn_tu_st(ctu_dec, x0, y0, log2_tb_w - 1, log2_tb_h, cu_flags, cbf_mask, tu_info);
        fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w - 1, log2_tb_h);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y0, log2_tb_w - 1, log2_tb_h);

    } else {
        tu_info[0].is_sbt = 1;
        if (log2_tb_w - 1 <= 5 && log2_tb_h <= 5) {
            tu_info->cu_mts_flag = 1;
            tu_info->cu_mts_idx = 0x0;
        }
        uint8_t x1 = x0 + (1 << (log2_tb_w - 1));
        uint8_t cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w - 1, log2_tb_h,
                                                   1, cu_flags, 0, tu_info);
        rcn_tu_st(ctu_dec, x1, y0, log2_tb_w - 1, log2_tb_h, cu_flags, cbf_mask, tu_info);
        fill_ctb_bound(&ctu_dec->dbf_info, x1, y0, log2_tb_w - 1, log2_tb_h);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x1, y0, log2_tb_w - 1, log2_tb_h);
    }

    return 0;
}

static int
sbt_half_hor(OVCTUDec *const ctu_dec,
             const OVPartInfo *const part_ctx,
             unsigned int x0, unsigned int y0,
             unsigned int log2_tb_w, unsigned int log2_tb_h,
             uint8_t sbt_pos, uint8_t cu_flags, struct TUInfo *tu_info)
{
    fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    if (!sbt_pos) {
        tu_info[0].is_sbt = 1;
        if (log2_tb_w <= 5 && log2_tb_h - 1 <= 5) {
            tu_info->cu_mts_flag = 1;
            tu_info->cu_mts_idx = 0x2;
        }

        uint8_t cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w, log2_tb_h - 1,
                                                   1, cu_flags, 0, tu_info);

        rcn_tu_st(ctu_dec, x0, y0, log2_tb_w, log2_tb_h - 1, cu_flags, cbf_mask, tu_info);
        fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h - 1);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h - 1);

    } else {
        tu_info[0].is_sbt = 1;
        uint8_t y1 = y0 + (1 << (log2_tb_h - 1));
        uint8_t y3 = y0 + (1 << (log2_tb_h - 1));
        if (log2_tb_w <= 5 && log2_tb_h - 1 <= 5) {
            tu_info->cu_mts_flag = 1;
            tu_info->cu_mts_idx = 0x0;
        }
        uint8_t cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w, log2_tb_h - 1,
                                                   1, cu_flags, 0, tu_info);


        rcn_tu_st(ctu_dec, x0, y1, log2_tb_w, log2_tb_h - 1, cu_flags, cbf_mask, tu_info);
        fill_ctb_bound(&ctu_dec->dbf_info, x0, y3, log2_tb_w, log2_tb_h - 1);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y3, log2_tb_w, log2_tb_h - 1);
    }
    return 0;
}

static int
sbt_quad_ver(OVCTUDec *const ctu_dec,
             const OVPartInfo *const part_ctx,
             unsigned int x0, unsigned int y0,
             unsigned int log2_tb_w, unsigned int log2_tb_h,
             uint8_t sbt_pos, uint8_t cu_flags, struct TUInfo *tu_info)
{
    fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    if (!sbt_pos) {
        tu_info[0].is_sbt = 1;

        if (log2_tb_w - 2 <= 5 && log2_tb_h <= 5) {
            tu_info->cu_mts_flag = 1;
            tu_info->cu_mts_idx = 0x1;
        }

        uint8_t cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w - 2, log2_tb_h,
                                                   1, cu_flags, 0, tu_info);


        rcn_tu_st(ctu_dec, x0, y0, log2_tb_w - 2, log2_tb_h, cu_flags, cbf_mask, tu_info);
        fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w - 2, log2_tb_h);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y0, log2_tb_w - 2, log2_tb_h);

    } else {
        tu_info[0].is_sbt = 1;
        uint8_t x3 = x0 + (3 << (log2_tb_w - 2));

        if (log2_tb_w - 2 <= 5 && log2_tb_h <= 5) {
            tu_info->cu_mts_flag = 1;
            tu_info->cu_mts_idx = 0x0;
        }

        uint8_t cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w - 2, log2_tb_h,
                                                   1, cu_flags, 0, tu_info);

        rcn_tu_st(ctu_dec, x3, y0, log2_tb_w - 2, log2_tb_h, cu_flags, cbf_mask, tu_info);
        fill_ctb_bound(&ctu_dec->dbf_info, x3, y0, log2_tb_w - 2, log2_tb_h);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x3, y0, log2_tb_w - 2, log2_tb_h);
    }
    return 0;
}

static int
sbt_quad_hor(OVCTUDec *const ctu_dec,
             const OVPartInfo *const part_ctx,
             unsigned int x0, unsigned int y0,
             unsigned int log2_tb_w, unsigned int log2_tb_h,
             uint8_t sbt_pos, uint8_t cu_flags, struct TUInfo *tu_info)
{
    fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h);
    if (!sbt_pos) {
        tu_info[0].is_sbt = 1;

        if (log2_tb_w <= 5 && log2_tb_h - 2 <= 5) {
            tu_info->cu_mts_flag = 1;
            tu_info->cu_mts_idx = 0x2;
        }

        uint8_t cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w, log2_tb_h - 2,
                                                   1, cu_flags, 0, tu_info);

        rcn_tu_st(ctu_dec, x0, y0, log2_tb_w, log2_tb_h - 2, cu_flags, cbf_mask, tu_info);
        fill_ctb_bound(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h - 2);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y0, log2_tb_w, log2_tb_h - 2);

    } else {
        tu_info[0].is_sbt = 1;
        uint8_t y3 = y0 + (3 << (log2_tb_h - 2));

        if (log2_tb_w <= 5 && log2_tb_h - 2 <= 5) {
            tu_info->cu_mts_flag = 1;
            tu_info->cu_mts_idx = 0x0;
        }

        uint8_t cbf_mask = ctu_dec->transform_unit(ctu_dec, x0, y0, log2_tb_w, log2_tb_h - 2,
                                                   1, cu_flags, 0, tu_info);


        rcn_tu_st(ctu_dec, x0, y3, log2_tb_w, log2_tb_h - 2, cu_flags, cbf_mask, tu_info);

        fill_ctb_bound(&ctu_dec->dbf_info, x0, y3, log2_tb_w, log2_tb_h - 2);
        fill_ctb_bound_c(&ctu_dec->dbf_info, x0, y3, log2_tb_w, log2_tb_h - 2);
    }
    return 0;
}

static int
sbt_tree(OVCTUDec *const ctu_dec,
         const OVPartInfo *const part_ctx,
         unsigned int x0, unsigned int y0,
         unsigned int log2_tb_w, unsigned int log2_tb_h,
         uint8_t sbt_mode,
         uint8_t sbt_pos, uint8_t cu_flags, struct TUInfo *tu_info)
{
    /* FIXME remove this if zeroing is done by transform */
    switch (sbt_mode) {
    case 0x1:
         /*sbt_ver*/
         sbt_half_hor(ctu_dec, part_ctx, x0, y0, log2_tb_w, log2_tb_h,
                      sbt_pos, cu_flags, tu_info);
         break;
    case 0x2:
         /*sbt_hor*/
         sbt_half_ver(ctu_dec, part_ctx, x0, y0, log2_tb_w, log2_tb_h,
                      sbt_pos, cu_flags, tu_info);
         break;
    case 0x4:
         /*sbt_ver/quad*/
         sbt_quad_hor(ctu_dec, part_ctx, x0, y0, log2_tb_w, log2_tb_h,
                      sbt_pos, cu_flags, tu_info);
         break;
    case 0x8:
         /*sbt_hor/quad*/
         sbt_quad_ver(ctu_dec, part_ctx, x0, y0, log2_tb_w, log2_tb_h,
                      sbt_pos, cu_flags, tu_info);
         break;
    }

    return 0;
}

static uint8_t
isp_subtree_v(OVCTUDec *const ctu_dec,
              unsigned int x0, unsigned int y0,
              unsigned int log2_cb_w, unsigned int log2_cb_h,
              uint8_t intra_mode)
{
    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;

    uint8_t cbf_flags = 0;
    int cbf = 0;
    int log2_pb_w = log2_cb_w - 2;
    int nb_pb;
    int i;
    struct ISPTUInfo tu_info = {0};
    struct TUInfo tu_info_c = {0};
    uint8_t cbf_mask_c = 0;

    /* height < 16 imposes restrictions on split dimension */
    if (log2_cb_h < 4 && (log2_pb_w <= (4 - log2_cb_h))) {
        log2_pb_w = 4 - log2_cb_h;
    }

    memset(ctu_dec->residual_y, 0, sizeof(int16_t) << (log2_cb_w + log2_cb_h));

    nb_pb = (1 << log2_cb_w) >> log2_pb_w;

    for (i = 0; i < nb_pb-1; ++i) {
        struct TBInfo *const tb_info = &tu_info.tb_info[i];
        cbf = ovcabac_read_ae_tu_cbf_luma_isp(cabac_ctx, cbf);
        cbf_flags <<= 1;
        cbf_flags |= cbf;

        if (cbf) {
            uint16_t last_pos = ovcabac_read_ae_last_sig_pos(cabac_ctx, log2_pb_w, log2_cb_h);
            int16_t *coeffs_y = ctu_dec->residual_y + i * (1 << (log2_pb_w + log2_cb_h));
            uint64_t scan_map = 0xFDA6EB73C8419520;
            int last_y = last_pos >> 8;
            int last_x = last_pos & 0xFF;
            tb_info->last_pos = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;

            if (log2_pb_w < 2) {
                tb_info->sig_sb_map = 2;
                ctu_dec->residual_coding_isp_v(ctu_dec, coeffs_y, log2_pb_w, log2_cb_h, last_pos);
            }else {
                tb_info->sig_sb_map = ctu_dec->residual_coding(ctu_dec, coeffs_y, log2_pb_w, log2_cb_h, last_pos);
            }
        }
    }

    if (ctu_dec->coding_tree != &dual_tree && ctu_dec->transform_unit == &transform_unit_st) {
        uint8_t tu_cbf_cb = ovcabac_read_ae_tu_cbf_cb(cabac_ctx);
        uint8_t tu_cbf_cr = ovcabac_read_ae_tu_cbf_cr(cabac_ctx, tu_cbf_cb);
        cbf_mask_c = (tu_cbf_cb << 1) | tu_cbf_cr;
    }

    cbf = !cbf_flags ? 1 : ovcabac_read_ae_tu_cbf_luma_isp(cabac_ctx, cbf);
    cbf_flags <<= 1;
    cbf_flags |= cbf;

    if (ctu_dec->jcbcr_enabled && cbf_mask_c) {
        uint8_t joint_cb_cr = ovcabac_read_ae_joint_cb_cr_flag(cabac_ctx,
                (cbf_mask_c & 0x3) - 1);
        cbf_mask_c |= joint_cb_cr << 3;
    }

    if (cbf) {
        struct TBInfo *const tb_info = &tu_info.tb_info[i];
        uint16_t last_pos = ovcabac_read_ae_last_sig_pos(cabac_ctx, log2_pb_w, log2_cb_h);
        int16_t *coeffs_y = ctu_dec->residual_y + i * (1 << (log2_pb_w + log2_cb_h));

        if (log2_pb_w <= 1) {
            tb_info->sig_sb_map = 2;
            ctu_dec->residual_coding_isp_v(ctu_dec, coeffs_y, log2_pb_w, log2_cb_h, last_pos);
        } else {
            uint64_t scan_map = 0xFDA6EB73C8419520;
            int last_y = last_pos >> 8;
            int last_x = last_pos & 0xFF;
            tb_info->last_pos  = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;
            tb_info->sig_sb_map = ctu_dec->residual_coding(ctu_dec, coeffs_y, log2_pb_w, log2_cb_h, last_pos);
        }
    }

    if (cbf_mask_c) {
        uint8_t jcbcr_flag = cbf_mask_c & 0x8;
        cbf_mask_c &= 0x3;
        if (jcbcr_flag) {

            residual_coding_jcbcr(ctu_dec, x0 >> 1, y0 >> 1, log2_cb_w - 1,
                                  log2_cb_h - 1, cbf_mask_c, &tu_info_c);


        } else if (cbf_mask_c) {

            residual_coding_c(ctu_dec, x0 >> 1, y0 >> 1, log2_cb_w - 1,
                              log2_cb_h - 1, cbf_mask_c, &tu_info_c);

        }
        cbf_mask_c |= jcbcr_flag;
    }

    if (ctu_dec->enable_lfnst) {
        int max_lfnst_pos = (log2_cb_h == log2_pb_w) && (log2_pb_w <= 3) ? 7 : 15;
        uint8_t can_lfnst = (tu_info.tb_info[0].sig_sb_map | tu_info.tb_info[1].sig_sb_map | tu_info.tb_info[2].sig_sb_map | tu_info.tb_info[3].sig_sb_map) == 1;

        can_lfnst &= tu_info.tb_info[0].last_pos <= max_lfnst_pos;
        can_lfnst &= tu_info.tb_info[1].last_pos <= max_lfnst_pos;
        can_lfnst &= tu_info.tb_info[2].last_pos <= max_lfnst_pos;
        can_lfnst &= tu_info.tb_info[3].last_pos <= max_lfnst_pos;

        if (cbf_mask_c) {
            uint8_t jcbcr_flag = cbf_mask_c & 0x8;
            uint64_t scan_map = 0xfda6eb73c8419520;
            int max_lfnst_pos_c = (log2_cb_h == log2_cb_w) && (log2_cb_w <= 4) ? 7 : 15;
            cbf_mask_c &= 0x3;
            if (log2_cb_w - 1 > 1 && log2_cb_h - 1 > 1) {
                if (jcbcr_flag) {
                    const struct TBInfo *const tb_info = &tu_info_c.tb_info[0];
                    int last_y = tb_info->last_pos >> 8;
                    int last_x = tb_info->last_pos & 0xFF;
                    int nb_coeffs = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;

                    can_lfnst &= tb_info->sig_sb_map == 0x1;
                    can_lfnst &= nb_coeffs <= max_lfnst_pos_c;
                } else {
                    uint8_t need_cb_chk = cbf_mask_c & 0x2;
                    uint8_t need_cr_chk = cbf_mask_c & 0x1;


                    if (need_cb_chk) {
                        const struct TBInfo *const tb_info_cb = &tu_info_c.tb_info[0];
                        int last_y_cb = tb_info_cb->last_pos >> 8;
                        int last_x_cb = tb_info_cb->last_pos & 0xff;
                        int nb_coeffs_cb = (scan_map >> ((last_x_cb + (last_y_cb << 2)) << 2)) & 0xf;
                        can_lfnst &= tb_info_cb->sig_sb_map == 0x1;
                        can_lfnst &= nb_coeffs_cb <= max_lfnst_pos_c;
                    }

                    if (need_cr_chk) {
                        const struct TBInfo *const tb_info_cr = &tu_info_c.tb_info[1];
                        int last_y_cr = tb_info_cr->last_pos >> 8;
                        int last_x_cr = tb_info_cr->last_pos & 0xff;
                        int nb_coeffs_cr = (scan_map >> ((last_x_cr + (last_y_cr << 2)) << 2)) & 0xf;
                        can_lfnst &= tb_info_cr->sig_sb_map == 0x1;
                        can_lfnst &= nb_coeffs_cr <= max_lfnst_pos_c;
                    }
                }
            }

            can_lfnst &= !tu_info_c.tr_skip_mask;
            cbf_mask_c |= jcbcr_flag;
        }

        if (can_lfnst) {
            uint8_t is_dual = ctu_dec->transform_unit != &transform_unit_st;
            uint8_t lfnst_flag = ovcabac_read_ae_lfnst_flag(cabac_ctx, is_dual);
            tu_info.lfnst_flag = lfnst_flag;
            if (lfnst_flag) {
                uint8_t lfnst_idx = ovcabac_read_ae_lfnst_idx(cabac_ctx);
                tu_info.lfnst_idx = lfnst_idx;
            }
        }
    }

    tu_info.cbf_mask = cbf_flags;

#if 1
    recon_isp_subtree_v(ctu_dec, x0, y0, log2_cb_w, log2_cb_h, intra_mode, &tu_info);
#endif

    if(ctu_dec->transform_unit == &transform_unit_st) {
        if (ctu_dec->intra_mode_c >= 67 && ctu_dec->intra_mode_c < 70) {
            vvc_intra_pred_chroma(&ctu_dec->rcn_ctx, &ctu_dec->rcn_ctx.ctu_buff, ctu_dec->intra_mode_c, x0 >> 1, y0 >> 1, log2_cb_w - 1, log2_cb_h - 1);
        }
    }

    if (cbf_mask_c) {
        uint8_t jcbcr_flag = cbf_mask_c & 0x8;
        tu_info_c.cbf_mask = cbf_mask_c;
        cbf_mask_c &= 0x3;
        if (jcbcr_flag) {

            rcn_jcbcr(ctu_dec, &tu_info_c, x0 >> 1, y0 >> 1, log2_cb_w - 1, log2_cb_h - 1, cbf_mask_c, 0);

        } else if (cbf_mask_c) {

            rcn_res_c(ctu_dec, &tu_info_c, x0 >> 1, y0 >> 1, log2_cb_w - 1, log2_cb_h - 1, cbf_mask_c, 0);

        }
    }

    return cbf_flags;
}

static uint8_t
isp_subtree_h(OVCTUDec *const ctu_dec,
              unsigned int x0, unsigned int y0,
              unsigned int log2_cb_w, unsigned int log2_cb_h,
              uint8_t intra_mode)
{
    OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
    int i;
    struct TUInfo tu_info_c = {0};

    uint8_t cbf = 0;
    uint8_t cbf_flags = 0;
    int log2_pb_h = log2_cb_h - 2;
    int nb_pb;
    int16_t *coeffs_y = ctu_dec->residual_y;
    struct ISPTUInfo tu_info = {0};
    uint8_t cbf_mask_c = 0;

    /* width < 16 imposes restrictions on split numbers */
    if (log2_cb_w < 4 && (log2_pb_h <= (4 - log2_cb_w))) {
        log2_pb_h = 4 - log2_cb_w;
    }

    int tb_s = 1 << (log2_cb_w + log2_pb_h);
    nb_pb = (1 << log2_cb_h) >> log2_pb_h;

    for (i = 0; i < nb_pb - 1; ++i) {
        struct TBInfo *const tb_info = &tu_info.tb_info[i];
        cbf = ovcabac_read_ae_tu_cbf_luma_isp(cabac_ctx, cbf);
        cbf_flags <<= 1;
        cbf_flags |= cbf;
        if (cbf) {
            uint16_t last_pos = ovcabac_read_ae_last_sig_pos(cabac_ctx, log2_cb_w, log2_pb_h);

            if (log2_pb_h <= 1) {
                tb_info->sig_sb_map = 2;
                ctu_dec->residual_coding_isp_h(ctu_dec, coeffs_y, log2_cb_w, log2_pb_h, last_pos);
            } else {
                uint64_t scan_map = 0xFDA6EB73C8419520;
                int last_y = last_pos >> 8;
                int last_x = last_pos & 0xFF;
                tb_info->last_pos   = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;
                tb_info->sig_sb_map = ctu_dec->residual_coding(ctu_dec, coeffs_y, log2_cb_w, log2_pb_h, last_pos);
            }
        }
        coeffs_y += tb_s;
    }

    if (ctu_dec->coding_tree != &dual_tree && ctu_dec->transform_unit == &transform_unit_st) {
        uint8_t tu_cbf_cb = ovcabac_read_ae_tu_cbf_cb(cabac_ctx);
        uint8_t tu_cbf_cr = ovcabac_read_ae_tu_cbf_cr(cabac_ctx, tu_cbf_cb);
        cbf_mask_c = (tu_cbf_cb << 1) | tu_cbf_cr;
    }

    cbf = !cbf_flags ? 1 : ovcabac_read_ae_tu_cbf_luma_isp(cabac_ctx, cbf);
    cbf_flags <<= 1;
    cbf_flags |= cbf;

    if (ctu_dec->jcbcr_enabled && cbf_mask_c) {
        uint8_t joint_cb_cr = ovcabac_read_ae_joint_cb_cr_flag(cabac_ctx,
                (cbf_mask_c & 0x3) - 1);
        cbf_mask_c |= joint_cb_cr << 3;
    }

    if (cbf) {
        struct TBInfo *const tb_info = &tu_info.tb_info[i];
        uint16_t last_pos = ovcabac_read_ae_last_sig_pos(cabac_ctx, log2_cb_w, log2_pb_h);

        if (log2_pb_h <= 1) {
            tb_info->sig_sb_map = 2;
            ctu_dec->residual_coding_isp_h(ctu_dec, coeffs_y, log2_cb_w, log2_pb_h, last_pos);
        } else {
            uint64_t scan_map = 0xFDA6EB73C8419520;
            int last_y = last_pos >> 8;
            int last_x = last_pos & 0xFF;
            tb_info->last_pos  = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;
            tb_info->sig_sb_map = ctu_dec->residual_coding(ctu_dec, coeffs_y, log2_cb_w, log2_pb_h, last_pos);
        }
    }

    if (cbf_mask_c) {
        uint8_t jcbcr_flag = cbf_mask_c & 0x8;
        cbf_mask_c &= 0x3;
        if (jcbcr_flag) {

            residual_coding_jcbcr(ctu_dec, x0 >> 1, y0 >> 1, log2_cb_w - 1,
                                  log2_cb_h - 1, cbf_mask_c, &tu_info_c);


        } else if (cbf_mask_c) {

            residual_coding_c(ctu_dec, x0 >> 1, y0 >> 1, log2_cb_w - 1,
                              log2_cb_h - 1, cbf_mask_c, &tu_info_c);

        }
        cbf_mask_c |= jcbcr_flag;

    }

    if (ctu_dec->enable_lfnst) {
        uint8_t can_lfnst = (tu_info.tb_info[0].sig_sb_map | tu_info.tb_info[1].sig_sb_map | tu_info.tb_info[2].sig_sb_map | tu_info.tb_info[3].sig_sb_map) == 1;
        int max_lfnst_pos = (log2_pb_h == log2_cb_w) && (log2_cb_w <= 3) ? 7 : 15;

        can_lfnst &= tu_info.tb_info[0].last_pos <= max_lfnst_pos;
        can_lfnst &= tu_info.tb_info[1].last_pos <= max_lfnst_pos;
        can_lfnst &= tu_info.tb_info[2].last_pos <= max_lfnst_pos;
        can_lfnst &= tu_info.tb_info[3].last_pos <= max_lfnst_pos;

        if (cbf_mask_c) {
            int max_lfnst_pos_c = (log2_cb_h == log2_cb_w) && (log2_cb_w <= 4) ? 7 : 15;
            uint8_t jcbcr_flag = cbf_mask_c & 0x8;
            uint64_t scan_map = 0xfda6eb73c8419520;
            cbf_mask_c &= 0x3;
            if (log2_cb_w - 1 > 1 && log2_cb_h - 1 > 1) {
                if (jcbcr_flag) {
                    const struct TBInfo *const tb_info = &tu_info_c.tb_info[0];
                    int last_y = tb_info->last_pos >> 8;
                    int last_x = tb_info->last_pos & 0xFF;
                    int nb_coeffs = (scan_map >> ((last_x + (last_y << 2)) << 2)) & 0xF;

                    can_lfnst &= tb_info->sig_sb_map == 0x1;
                    can_lfnst &= nb_coeffs <= max_lfnst_pos_c;
                } else {
                    uint8_t need_cb_chk = cbf_mask_c & 0x2;
                    uint8_t need_cr_chk = cbf_mask_c & 0x1;

                    if (need_cb_chk) {
                        const struct TBInfo *const tb_info_cb = &tu_info_c.tb_info[0];
                        int last_y_cb = tb_info_cb->last_pos >> 8;
                        int last_x_cb = tb_info_cb->last_pos & 0xff;
                        int nb_coeffs_cb = (scan_map >> ((last_x_cb + (last_y_cb << 2)) << 2)) & 0xf;
                        can_lfnst &= tb_info_cb->sig_sb_map == 0x1;
                        can_lfnst &= nb_coeffs_cb <= max_lfnst_pos_c;
                    }

                    if (need_cr_chk) {
                        const struct TBInfo *const tb_info_cr = &tu_info_c.tb_info[1];
                        int last_y_cr = tb_info_cr->last_pos >> 8;
                        int last_x_cr = tb_info_cr->last_pos & 0xff;
                        int nb_coeffs_cr = (scan_map >> ((last_x_cr + (last_y_cr << 2)) << 2)) & 0xf;
                        can_lfnst &= tb_info_cr->sig_sb_map == 0x1;
                        can_lfnst &= nb_coeffs_cr <= max_lfnst_pos_c;
                    }
                }
            }
            can_lfnst &= !tu_info_c.tr_skip_mask;
            cbf_mask_c |= jcbcr_flag;
        }

        if (can_lfnst) {
            uint8_t is_dual = ctu_dec->transform_unit != &transform_unit_st;
            uint8_t lfnst_flag = ovcabac_read_ae_lfnst_flag(cabac_ctx, is_dual);
            tu_info.lfnst_flag = lfnst_flag;
            if (lfnst_flag) {
                uint8_t lfnst_idx = ovcabac_read_ae_lfnst_idx(cabac_ctx);
                tu_info.lfnst_idx = lfnst_idx;
            }
        }
    }

    tu_info.cbf_mask = cbf_flags;

#if 1
    recon_isp_subtree_h(ctu_dec, x0, y0, log2_cb_w, log2_cb_h, intra_mode, &tu_info);
#endif
    if(ctu_dec->transform_unit == &transform_unit_st) {
        if (ctu_dec->intra_mode_c >= 67 && ctu_dec->intra_mode_c < 70) {
            vvc_intra_pred_chroma(&ctu_dec->rcn_ctx, &ctu_dec->rcn_ctx.ctu_buff, ctu_dec->intra_mode_c, x0 >> 1, y0 >> 1, log2_cb_w - 1, log2_cb_h - 1);
        }
    }

    if (cbf_mask_c) {
        uint8_t jcbcr_flag = cbf_mask_c & 0x8;
        tu_info_c.cbf_mask = cbf_mask_c;
        cbf_mask_c &= 0x3;
        if (jcbcr_flag) {

            rcn_jcbcr(ctu_dec, &tu_info_c, x0 >> 1, y0 >> 1, log2_cb_w - 1, log2_cb_h - 1, cbf_mask_c, 0);

        } else if (cbf_mask_c) {

            rcn_res_c(ctu_dec, &tu_info_c, x0 >> 1, y0 >> 1, log2_cb_w - 1, log2_cb_h - 1, cbf_mask_c, 0);

        }
    }

    return cbf_flags;
}

static uint8_t
sbt_allowed(uint8_t log2_tb_w, uint8_t log2_tb_h)
{
  uint8_t sbt_status = 0;
  uint8_t allow_half_v = log2_tb_w >= 3;
  uint8_t allow_half_h = log2_tb_h >= 3;
  uint8_t allow_quad_v = log2_tb_w >= 4;
  uint8_t allow_quad_h = log2_tb_h >= 4;

  if (log2_tb_w > 6 || log2_tb_h > 6) {
      return 0;
  }

  sbt_status |= allow_half_h;
  sbt_status |= allow_half_v << 1;
  sbt_status |= allow_quad_h << 2;
  sbt_status |= allow_quad_v << 3;

  return sbt_status;
}

static uint8_t
sbt_mode(OVCABACCtx *const cabac_ctx, uint8_t log2_tb_w, uint8_t log2_tb_h, uint8_t sbt_mask)
{
    uint8_t sbt_mode;
    uint8_t sbt_quad_flag;
    uint8_t sbt_hor_flag;
    uint8_t sbt_pos_flag;

    if ((sbt_mask & 0xC) && (sbt_mask & 0x3)) {
        sbt_quad_flag = ovcabac_read_ae_sbt_quad_flag(cabac_ctx);
    } else {
        sbt_quad_flag = 0;
    }

    if ((sbt_quad_flag && (sbt_mask & 0x4) && (sbt_mask & 0x8)) ||
        (!sbt_quad_flag && (sbt_mask & 0x1) && (sbt_mask & 0x2))) {
        sbt_hor_flag = ovcabac_read_ae_sbt_hor_flag(cabac_ctx, log2_tb_w, log2_tb_h);
    } else {
        sbt_hor_flag = (sbt_quad_flag && (sbt_mask & 0x4)) || (!sbt_quad_flag && (sbt_mask & 0x1));
    }

    sbt_pos_flag = ovcabac_read_ae_sbt_pos_flag(cabac_ctx);

    sbt_mode = ((1 << (!sbt_hor_flag)) << (sbt_quad_flag << 1));
    sbt_mode |= sbt_pos_flag << 7;

    return sbt_mode;
}

int
transform_unit_wrap(OVCTUDec *const ctu_dec,
                    const OVPartInfo *const part_ctx,
                    uint8_t x0, uint8_t y0,
                    uint8_t log2_cb_w, uint8_t log2_cb_h,
                    VVCCU cu)
{
    if (cu.cu_flags & flg_pred_mode_flag) {
        /* INTRA */
        if (!(cu.cu_flags & flg_isp_flag)) {
            /*FIXME check if part_ctx mandatory for transform_tree */
            struct TUInfo tu_info[4] = {0};
            transform_tree(ctu_dec, part_ctx, x0, y0, log2_cb_w, log2_cb_h,
                           part_ctx->log2_max_tb_s, 0, cu.cu_flags, 0, tu_info);

            lfnst_mts(ctu_dec, log2_cb_w, log2_cb_h, cu.cu_flags, tu_info);

            rcn_transform_tree(ctu_dec, x0, y0, log2_cb_w, log2_cb_h, part_ctx->log2_max_tb_s,
                               cu.cu_flags, tu_info);
        } else {
            uint8_t isp_mode = cu.cu_opaque;
            uint8_t intra_mode = cu.cu_mode_idx;
            if (isp_mode) {
                /* Disable CCLM in 64x64 ISP CU*/
                ctu_dec->tmp_disable_cclm = log2_cb_h == log2_cb_w && log2_cb_h == 6;

                if (isp_mode == 2) {
                    isp_subtree_v(ctu_dec, x0, y0, log2_cb_w, log2_cb_h, intra_mode);
                } else if (isp_mode == 1) {
                    isp_subtree_h(ctu_dec, x0, y0, log2_cb_w, log2_cb_h, intra_mode);
                }
            }
        }
    } else {
        /* INTER (should probably be default in a swicth*/
        /*FIXME move root_cbf_read into transform_tree */
        struct TUInfo tu_info[4] = {0};
        OVCABACCtx *const cabac_ctx = ctu_dec->cabac_ctx;
        uint8_t merge_flag   = !!(cu.cu_flags & flg_merge_flag);
        uint8_t rqt_root_cbf = merge_flag || ovcabac_read_ae_root_cbf(cabac_ctx);
        if (ctu_dec->tmp_ciip) {
        fill_bs_map(&ctu_dec->dbf_info.bs2_map, x0, y0, log2_cb_w, log2_cb_h);
        fill_bs_map(&ctu_dec->dbf_info.bs2_map_c, x0, y0, log2_cb_w, log2_cb_h);
        }

        if (rqt_root_cbf) {
            uint8_t sbt_flag = 0;
            uint8_t sbt_type = 0;
            if (ctu_dec->sbt_enabled && !ctu_dec->tmp_ciip) {
                uint8_t sbt_mask = sbt_allowed(log2_cb_w, log2_cb_h);
                if (sbt_mask) {
                    sbt_flag = ovcabac_read_ae_sbt_flag(cabac_ctx, log2_cb_w, log2_cb_h);
                    if (sbt_flag) {
                        sbt_type = sbt_mode(cabac_ctx, log2_cb_w, log2_cb_h, sbt_mask);
                    }
                }
            }

            if (!sbt_flag) {
                transform_tree(ctu_dec, part_ctx, x0, y0, log2_cb_w, log2_cb_h,
                               part_ctx->log2_max_tb_s, 1, cu.cu_flags, 0, tu_info);
            } else {
                /* SBT tree */
                uint8_t sbt_pos = !!(sbt_type & 0x80);
                sbt_type &= 0x7F;
                sbt_tree(ctu_dec, part_ctx, x0, y0, log2_cb_w, log2_cb_h,
                         sbt_type, !!sbt_pos, cu.cu_flags, tu_info);
            }

            lfnst_mts(ctu_dec, log2_cb_w, log2_cb_h, cu.cu_flags, tu_info);

            rcn_transform_tree(ctu_dec, x0, y0, log2_cb_w, log2_cb_h, part_ctx->log2_max_tb_s,
                               cu.cu_flags, tu_info);
        }
    }
    return 0;
}

