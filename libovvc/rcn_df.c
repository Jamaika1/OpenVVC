#include <stdint.h>
#include <stdlib.h>
#include "ovutils.h"
#include "dec_structures.h"
#include "ctudec.h"

#define DEFAULT_INTRA_TC_OFFSET 2 ///< Default intra TC offset
#define MAX_QP 64

static const uint16_t tc_lut[MAX_QP + 1 + DEFAULT_INTRA_TC_OFFSET] =
{
      0,   0,   0,   0,   0,   0,   0,   0,   
      0,   0,   0,   0,   0,   0,   0,   0,   
      0,   0,   3,   4,   4,   4,   4,   5,   
      5,   5,   5,   7,   7,   8,   9,  10,   
     10,  11,  13,  14,  15,  17,  19,  21, 
     24,  25,  29,  33,  36,  41,  45,  51, 
     57,  64,  71,  80,  89, 100, 112, 125, 
    141, 157, 177, 198, 222, 250, 280, 314, 
    352, 395
};

static const uint8_t beta_lut[MAX_QP + 1] =
{
     0,  0,  0,  0,  0,  0,  0,  0,  
     0,  0,  0,  0,  0,  0,  0,  0,  
     6,  7,  8,  9, 10, 11, 12, 13, 
    14, 15, 16, 17, 18, 20, 22, 24, 
    26, 28, 30, 32, 34, 36, 38, 40, 
    42, 44, 46, 48, 50, 52, 54, 56, 
    58, 60, 62, 64, 66, 68, 70, 72,
    74, 76, 78, 80, 82, 84, 86, 88
};


static inline uint8_t
use_strong_filter_l0(const int16_t* src, const int stride, const int beta, const int tc, int max_l_p, int max_l_q)
{
    const int16_t m0 = src[-stride * 4];
    const int16_t m3 = src[-stride    ];
    const int16_t m4 = src[ 0         ];
    const int16_t m7 = src[ stride * 3];

    int sp3 = abs(m0 - m3);
    int sq3 = abs(m7 - m4);

    /*FIXME we could use branchless to derive eiher mxmy coordinate */
    if (max_l_p == 7) {
        const int16_t mP8 = src[-stride * 8];
        const int16_t mP7 = src[-stride * 7];
        const int16_t mP6 = src[-stride * 6];
        const int16_t mP5 = src[-stride * 5];
        sp3 += abs(mP5 - mP6 - mP7 + mP8);
        sp3 += abs(m0 - mP8) + 1;
        sp3 >>= 1;
    } else if (max_l_p > 3) {
        /*FIXME max_l_p == 5 ?*/
        const int16_t mP6 = src[-stride * 6];
        sp3 += abs(m0 - mP6) + 1;
        sp3 >>= 1;
    }

    if (max_l_q == 7) {
        const int16_t m8  = src[stride * 4];
        const int16_t m9  = src[stride * 5];
        const int16_t m10 = src[stride * 6];
        const int16_t m11 = src[stride * 7];
        sq3 += abs(m8 - m9 - m10 + m11);
        sq3 += abs(m11 - m7) + 1;
        sq3 >>= 1;
    } else if (max_l_q > 3) {
        const int16_t m9  = src[stride * 5];
        sq3 += abs(m9 - m7) + 1;
        sq3 >>= 1;
    }

    return ((sp3 + sq3) < (beta * 3 >> 5)) && (abs(m3 - m4) < ((tc * 5 + 1) >> 1));
}

static inline uint8_t
use_strong_filter_l1(const int16_t* src, const int stride, const int beta, const int tc)
{
    const int16_t m0 = src[-stride * 4];
    const int16_t m3 = src[-stride    ];
    const int16_t m4 = src[ 0         ];
    const int16_t m7 = src[ stride * 3];

    int sp3 = abs(m0 - m3);
    int sq3 = abs(m7 - m4);

    const int d_strong = sp3 + sq3;

    return ((d_strong < (beta >> 3)) && (abs(m3 - m4) < ((tc * 5 + 1) >> 1)));
}

/* FIXME Macros ? */
static inline uint16_t
compute_dp_c(int16_t* src, const int stride , const uint8_t is_ctb_b)
{
    return abs(src[-stride * (3 - is_ctb_b)] - 2 * src[-stride * 2] + src[-stride]);
}

static inline uint16_t
compute_dp(int16_t* src, const int stride)
{
    return abs(src[-stride * 3] - 2 * src[-stride * 2] + src[-stride]);
}

static inline uint16_t
compute_dq(int16_t* src, const int stride)
{
    return abs(src[0] - 2 * src[stride] + src[stride * 2]);
}
struct DBFParams{
    int beta;
    int tc;
};

/* FIXME This could be done once at init if not if delta QP is disabled */
/* Note QP is average QP between the two blocks instead of storing qp
 * we might be able to store beta and tc indexes directly
 */
static struct DBFParams
compute_dbf_limits(const struct DBFInfo *const dbf_info, int qp, int bs)
{
    const int bitdepth  = 10;
    int beta_offset  = dbf_info->beta_offset;
    int tc_offset    = dbf_info->tc_offset;

    /*FIXME add LADF handling */

    const int tc_idx   = ov_clip((qp + DEFAULT_INTRA_TC_OFFSET * (bs - 1) + tc_offset), 0, MAX_QP + DEFAULT_INTRA_TC_OFFSET);
    const int beta_idx = ov_clip(qp + beta_offset, 0, MAX_QP);

    const int tc = tc_lut[tc_idx] << (bitdepth - 10);

    const int beta = beta_lut[beta_idx] << (bitdepth - 8);
    struct DBFParams dbf_params;
    dbf_params.beta = beta;
    dbf_params.tc = tc;

    return dbf_params;
}

static inline uint64_t
derive_size_3_map(const uint64_t *edge_map)
{
    uint64_t is_large_map = edge_map[0];
    is_large_map |= edge_map[1];
    is_large_map |= edge_map[2];
    is_large_map |= edge_map[3];
    is_large_map |= edge_map[4];
    is_large_map |= edge_map[5];
    is_large_map |= edge_map[6];
    return ~is_large_map;
}

static inline int
derive_filter_idx(int filter_l_p, int filter_l_q)
{
    int p_idx = (filter_l_p >> 1) - 1;
    int q_idx = (filter_l_q >> 1) - 1;
    return (((p_idx & 0x3) << 2) | (q_idx & 0x3));
}

static void
filter_7_7(int16_t *src, const int stride, const int tc)
{
    int16_t* srcP = src - stride;
    int16_t* srcQ = src;

    static const int dbCoeffs7[7] = { 59, 50, 41, 32, 23, 14, 5 };
    static const int8_t tc7[7] = { 6, 5, 4, 3, 2, 1, 1};

    int refP = (srcP[-6 * stride] + srcP[-7 * stride] + 1) >> 1;
    int refQ = (srcQ[ 6 * stride] + srcQ[ 7 * stride] + 1) >> 1;

    int refMiddle = (2 * (srcP[0] + srcQ[0])
                    + srcP[-stride] + srcP[-2 * stride] + srcP[-3 * stride] + srcP[-4 * stride] + srcP[-5 * stride] + srcP[-6 * stride] 
                    + srcQ[ stride] + srcQ[ 2 * stride] + srcQ[ 3 * stride] + srcQ[ 4 * stride] + srcQ[ 5 * stride] + srcQ[ 6 * stride] + 8) >> 4;

    for (int pos = 0; pos < 7; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcP[-stride * pos];
        srcP[-stride * pos] = ov_clip(((refMiddle * dbCoeffs7[pos] + refP * (64 - dbCoeffs7[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }

    for (int pos = 0; pos < 7; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcQ[ stride * pos];
        srcQ[ stride * pos] = ov_clip(((refMiddle * dbCoeffs7[pos] + refQ * (64 - dbCoeffs7[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }
}

static void
filter_7_5(int16_t *src, const int stride, const int tc)
{
    int16_t* srcP = src - stride;
    int16_t* srcQ = src;

    static const int dbCoeffs7[7] = { 59, 50, 41, 32, 23, 14, 5 };
    static const int dbCoeffs5[5] = { 58, 45, 32, 19, 6};
    static const int8_t tc7[7] = { 6, 5, 4, 3, 2, 1, 1};

    int refP = (srcP[-6 * stride] + srcP[-7 * stride] + 1) >> 1;
    int refQ = (srcQ[ 4 * stride] + srcQ[ 5 * stride] + 1) >> 1;

    int refMiddle = (2 * (srcP[0] + srcP[-stride] + srcQ[0] + srcQ[ stride])
                + srcP[-2 * stride] + srcP[-3 * stride] + srcP[-4 * stride] + srcP[-5 * stride]
                + srcQ[ 2 * stride] + srcQ[ 3 * stride] + srcQ[ 4 * stride] + srcQ[ 5 * stride] + 8) >> 4;


    for (int pos = 0; pos < 7; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcP[-stride * pos];
        srcP[-stride * pos] = ov_clip(((refMiddle * dbCoeffs7[pos] + refP * (64 - dbCoeffs7[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }

    for (int pos = 0; pos < 5; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcQ[ stride * pos];
        srcQ[ stride * pos] = ov_clip(((refMiddle * dbCoeffs5[pos] + refQ * (64 - dbCoeffs5[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }
}

static void
filter_5_7(int16_t *src, const int stride, const int tc)
{
    int16_t* srcP = src - stride;
    int16_t* srcQ = src;

    static const int dbCoeffs7[7] = { 59, 50, 41, 32, 23, 14, 5 };
    static const int dbCoeffs5[5] = { 58, 45, 32, 19, 6};
    static const int8_t tc7[7] = { 6, 5, 4, 3, 2, 1, 1};

    int refP = (srcP[-4 * stride] + srcP[-5 * stride] + 1) >> 1;
    int refQ = (srcQ[ 6 * stride] + srcQ[ 7 * stride] + 1) >> 1;

    int refMiddle = (2 * (srcP[0] + srcP[-stride] + srcQ[0] + srcQ[ stride])
            + srcP[-2 * stride] + srcP[-3 * stride] + srcP[-4 * stride] + srcP[-5 * stride]
            + srcQ[ 2 * stride] + srcQ[ 3 * stride] + srcQ[ 4 * stride] + srcQ[ 5 * stride] + 8) >> 4;

    for (int pos = 0; pos < 5; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcP[-stride * pos];
        srcP[-stride * pos] = ov_clip(((refMiddle * dbCoeffs5[pos] + refP * (64 - dbCoeffs5[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }

    for (int pos = 0; pos < 7; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcQ[ stride * pos];
        srcQ[ stride * pos] = ov_clip(((refMiddle * dbCoeffs7[pos] + refQ * (64 - dbCoeffs7[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }
}

static void
filter_5_5(int16_t *src, const int stride, const int tc)
{
    int16_t* srcP = src - stride;
    int16_t* srcQ = src;

    static const int dbCoeffs5[5] = { 58, 45, 32, 19, 6};
    static const int8_t tc7[7] = { 6, 5, 4, 3, 2, 1, 1};

    int refP = (srcP[-4 * stride] + srcP[-5 * stride] + 1) >> 1;
    int refQ = (srcQ[ 4 * stride] + srcQ[ 5 * stride] + 1) >> 1;

    int refMiddle = (2 * (srcP[0] + srcP[-stride] + srcP[-2 * stride]
                        + srcQ[0] + srcQ[ stride] + srcQ[ 2 * stride])
              + srcP[-3 * stride] + srcP[-4 * stride]
              + srcQ[ 3 * stride] + srcQ[ 4 * stride] + 8) >> 4;


    for (int pos = 0; pos < 5; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcP[-stride * pos];
        srcP[-stride * pos] = ov_clip(((refMiddle * dbCoeffs5[pos] + refP * (64 - dbCoeffs5[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }

    for (int pos = 0; pos < 5; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcQ[ stride * pos];
        srcQ[ stride * pos] = ov_clip(((refMiddle * dbCoeffs5[pos] + refQ * (64 - dbCoeffs5[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }
}

static void
filter_7_3(int16_t *src, const int stride, const int tc)
{
    int16_t* srcP = src - stride;
    int16_t* srcQ = src;

    static const int dbCoeffs7[7] = { 59, 50, 41, 32, 23, 14, 5 };
    static const int dbCoeffs3[3] = { 53, 32, 11 };
    static const int8_t tc7[7] = { 6, 5, 4, 3, 2, 1, 1};
    static const int8_t tc3[3] = { 6, 4, 2 };


    int refP = (srcP[-6 * stride] + srcP[-7 * stride] + 1) >> 1;
    int refQ = (srcQ[ 2 * stride] + srcQ[ 3 * stride] + 1) >> 1;

    int refMiddle = (2 * (srcP[0] + srcQ[0]) 
            + srcP[-stride] + srcP[-2 * stride] + srcP[-3 * stride] + srcP[-4 * stride] + srcP[-5 * stride] + srcP[-6 * stride]
            + srcQ[      0] + srcQ[     stride] + srcQ[     stride] + srcQ[2 *  stride] + srcQ[ 2 * stride] + srcQ[     stride] + 8) >> 4;

    for (int pos = 0; pos < 7; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcP[-stride * pos];
        srcP[-stride * pos] = ov_clip(((refMiddle * dbCoeffs7[pos] + refP * (64 - dbCoeffs7[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }

    for (int pos = 0; pos < 3; pos++) {
        int cvalue = (tc * tc3[pos]) >> 1;
        src = &srcQ[ stride * pos];
        srcQ[ stride * pos] = ov_clip(((refMiddle * dbCoeffs3[pos] + refQ * (64 - dbCoeffs3[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }
}

static void
filter_3_7(int16_t *src, const int stride, const int tc)
{
    int16_t* srcP = src - stride;
    int16_t* srcQ = src;

    static const int dbCoeffs7[7] = { 59, 50, 41, 32, 23, 14, 5 };
    static const int dbCoeffs3[3] = { 53, 32, 11 };
    static const int8_t tc7[7] = { 6, 5, 4, 3, 2, 1, 1};
    static const int8_t tc3[3] = { 6, 4, 2 };


    int refP = (srcP[-2 * stride] + srcP[-3 * stride] + 1) >> 1;
    int refQ = (srcQ[ 6 * stride] + srcQ[ 7 * stride] + 1) >> 1;

    int refMiddle = (2 * (srcQ[0] + srcP[0]) 
            + srcP[      0] + srcP[    -stride] + srcP[    -stride] + srcP[-2 * stride] + srcP[-2 * stride] + srcP[    -stride]
            + srcQ[ stride] + srcQ[2 *  stride] + srcQ[3 *  stride] + srcQ[4 *  stride] + srcQ[5 *  stride] + srcQ[6 *  stride] + 8) >> 4;

    for (int pos = 0; pos < 3; pos++) {
        int cvalue = (tc * tc3[pos]) >> 1;
        src = &srcP[-stride * pos];
        srcP[-stride * pos] = ov_clip(((refMiddle * dbCoeffs3[pos] + refP * (64 - dbCoeffs3[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }

    for (int pos = 0; pos < 7; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcQ[ stride * pos];
        srcQ[ stride * pos] = ov_clip(((refMiddle * dbCoeffs7[pos] + refQ * (64 - dbCoeffs7[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }
}

static void
filter_5_3(int16_t *src, const int stride, const int tc)
{
    int16_t* srcP = src - stride;
    int16_t* srcQ = src;

    static const int dbCoeffs3[3] = { 53, 32, 11 };
    static const int dbCoeffs5[5] = { 58, 45, 32, 19, 6};
    static const int8_t tc7[7] = { 6, 5, 4, 3, 2, 1, 1};
    static const int8_t tc3[3] = { 6, 4, 2 };


    int refP = (srcP[-4 * stride] + srcP[-5 * stride] + 1) >> 1;
    int refQ = (srcQ[ 2 * stride] + srcQ[ 3 * stride] + 1) >> 1;

    int refMiddle = (srcP[0] + srcP[-stride] + srcP[-2 * stride] + srcP[-3 * stride]
                   + srcQ[0] + srcQ[ stride] + srcQ[ 2 * stride] + srcQ[ 3 * stride] + 4) >> 3;

    for (int pos = 0; pos < 5; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcP[-stride * pos];
        srcP[-stride * pos] = ov_clip(((refMiddle * dbCoeffs5[pos] + refP * (64 - dbCoeffs5[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }

    for (int pos = 0; pos < 3; pos++) {
        int cvalue = (tc * tc3[pos]) >> 1;
        src = &srcQ[ stride * pos];
        srcQ[ stride * pos] = ov_clip(((refMiddle * dbCoeffs3[pos] + refQ * (64 - dbCoeffs3[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }
}

static void
filter_3_5(int16_t *src, const int stride, const int tc)
{
    int16_t* srcP = src - stride;
    int16_t* srcQ = src;

    static const int dbCoeffs3[3] = { 53, 32, 11 };
    static const int dbCoeffs5[5] = { 58, 45, 32, 19, 6};
    static const int8_t tc7[7] = { 6, 5, 4, 3, 2, 1, 1};
    static const int8_t tc3[3] = { 6, 4, 2 };

    int refP = (srcP[-2 * stride] + srcP[-3 * stride] + 1) >> 1;
    int refQ = (srcQ[ 4 * stride] + srcQ[ 5 * stride] + 1) >> 1;

    int refMiddle = (srcP[0] + srcP[-stride] + srcP[-2 * stride] + srcP[-3 * stride]
                   + srcQ[0] + srcQ[ stride] + srcQ[ 2 * stride] + srcQ[ 3 * stride] + 4) >> 3;


    for (int pos = 0; pos < 3; pos++) {
        int cvalue = (tc * tc3[pos]) >> 1;
        src = &srcP[-stride * pos];
        srcP[-stride * pos] = ov_clip(((refMiddle * dbCoeffs3[pos] + refP * (64 - dbCoeffs3[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }

    for (int pos = 0; pos < 5; pos++) {
        int cvalue = (tc * tc7[pos]) >> 1;
        src = &srcQ[ stride * pos];
        srcQ[ stride * pos] = ov_clip(((refMiddle * dbCoeffs5[pos] + refQ * (64 - dbCoeffs5[pos]) + 32) >> 6),*src - cvalue, *src + cvalue);
    }
}

void (*filter_lut[11])(int16_t *src, const int stride, const int tc) = {
    NULL       , &filter_3_5, &filter_3_7, NULL       ,
    &filter_5_3, &filter_5_5, &filter_5_7, NULL       ,
    &filter_7_3, &filter_7_5, &filter_7_7
};

static inline void
filter_luma_strong_large(int16_t* src, const int stride, const int tc, int max_l_p, int max_l_q)
{
    const int filter_idx = derive_filter_idx(max_l_p, max_l_q);
    filter_lut[filter_idx](src, stride, tc);
}

static inline void
filter_luma_strong_small(int16_t* src, const int stride, const int tc)
{

    const int16_t m0  = src[-stride * 4];
    const int16_t m1  = src[-stride * 3];
    const int16_t m2  = src[-stride * 2];
    const int16_t m3  = src[-stride    ];
    const int16_t m4  = src[ 0         ];
    const int16_t m5  = src[ stride    ];
    const int16_t m6  = src[ stride * 2];
    const int16_t m7  = src[ stride * 3];

    src[-stride * 3] = ov_clip(((2 * m0 + 3 * m1 + m2 + m3 + m4 + 4) >> 3),m1 - 1 * tc, m1 + 1 * tc);
    src[-stride * 2] = ov_clip(((m1 + m2 + m3 + m4 + 2) >> 2),m2 - 2 * tc, m2 + 2 * tc);
    src[-stride]     = ov_clip(((m1 + 2 * m2 + 2 * m3 + 2 * m4 + m5 + 4) >> 3),m3 - 3 * tc, m3 + 3 * tc);
    src[0]           = ov_clip(((m2 + 2 * m3 + 2 * m4 + 2 * m5 + m6 + 4) >> 3),m4 - 3 * tc, m4 + 3 * tc);
    src[stride]      = ov_clip(((m3 + m4 + m5 + m6 + 2) >> 2),m5 - 2 * tc, m5 + 2 * tc);
    src[stride * 2]  = ov_clip(((m3 + m4 + m5 + 3 * m6 + 2 * m7 + 4) >> 3),m6 - 1 * tc, m6 + 1 * tc);
}

static inline void
filter_luma_weak(int16_t* src, const int stride, const int tc, const int th_cut, const uint8_t extend_p, const uint8_t extend_q)
{
    const int16_t m1  = src[-stride * 3];
    const int16_t m2  = src[-stride * 2];
    const int16_t m3  = src[-stride    ];
    const int16_t m4  = src[ 0         ];
    const int16_t m5  = src[ stride    ];
    const int16_t m6  = src[ stride * 2];

    /* Weak filter */
    int delta = (9 * (m4 - m3) - 3 * (m5 - m2) + 8) >> 4;

    if (abs(delta) < th_cut) {
        delta = ov_clip(delta,-tc, tc);
        src[-stride] = ov_clip(m3 + delta, 0, 1023 );
        src[0]       = ov_clip(m4 - delta, 0, 1023);

        if (extend_p) {
            const int tc2 = tc >> 1;
            const int delta1 = ov_clip(((((m1 + m3 + 1) >> 1) - m2 + delta) >> 1),-tc2, tc2);
            src[-stride * 2] = ov_clip(m2 + delta1, 0, 1023);
        }

        if (extend_q) {
            const int tc2 = tc >> 1;
            const int delta2 = ov_clip(((((m6 + m4 + 1) >> 1) - m5 - delta) >> 1),-tc2, tc2);
            src[stride] = ov_clip(m5 + delta2, 0, 1023);
        }
    }
}


static inline uint8_t
use_strong_filter_c2(uint16_t* src, const int stride, const int beta, const int tc, uint8_t is_ctb_b)
{
    const int16_t m0 = src[(-stride * 4) >> is_ctb_b];
    #if 0
    const int16_t m2 = src[-stride * 2];
    #endif
    const int16_t m3 = src[-stride    ];
    const int16_t m4 = src[ 0         ];
    const int16_t m7 = src[ stride * 3];

    int sp3 = abs(m0 - m3);
    int sq3 = abs(m7 - m4);

    const int d_strong = sp3 + sq3;

    return ((d_strong < (beta >> 3)) && (abs(m3 - m4) < ((tc * 5 + 1) >> 1)));
}

static inline uint8_t
use_strong_filter_c(uint16_t* src, const int stride, const int beta, const int tc)
{
    const int16_t m0 = src[-stride * 4];
    const int16_t m3 = src[-stride    ];
    const int16_t m4 = src[ 0         ];
    const int16_t m7 = src[ stride * 3];

    int sp3 = abs(m0 - m3);
    int sq3 = abs(m7 - m4);

    const int d_strong = sp3 + sq3;

    return ((d_strong < (beta >> 3)) && (abs(m3 - m4) < ((tc * 5 + 1) >> 1)));
}

static inline void
filter_chroma_strong(uint16_t* src, const int stride, const int tc/*, const uint8_t is_ctb_b*/)
{
    const int16_t m0 = src[-stride * 4];
    const int16_t m1 = src[-stride * 3];
    const int16_t m2 = src[-stride * 2];
    const int16_t m3 = src[-stride    ];
    const int16_t m4 = src[0          ];
    const int16_t m5 = src[ stride    ];
    const int16_t m6 = src[ stride * 2];
    const int16_t m7 = src[ stride * 3];

    src[-stride * 3] = ov_clip(((3 * m0 + 2 * m1 + m2 + m3 + m4 + 4) >> 3),m1 - tc, m1 + tc);      // p)2
    src[-stride * 2] = ov_clip(((2 * m0 + m1 + 2 * m2 + m3 + m4 + m5 + 4) >> 3),m2 - tc, m2 + tc); // p)1
    src[-stride * 1] = ov_clip(((m0 + m1 + m2 + 2 * m3 + m4 + m5 + m6 + 4) >> 3),m3 - tc, m3 + tc); // p)0
    src[ 0         ] = ov_clip(((m1 + m2 + m3 + 2 * m4 + m5 + m6 + m7 + 4) >> 3),m4 - tc, m4 + tc); // q)0
    src[ stride * 1] = ov_clip(((m2 + m3 + m4 + 2 * m5 + m6 + 2 * m7 + 4) >> 3),m5 - tc, m5 + tc);  // q)1
    src[ stride * 2] = ov_clip(((m3 + m4 + m5 + 2 * m6 + 3 * m7 + 4) >> 3),m6 - tc, m6 + tc);       // q)2
}

static inline void
filter_chroma_strong_c(uint16_t* src, const int stride, const int tc, uint8_t is_ctb_b)
{
    const int16_t m0 = src[-stride * 4];
    const int16_t m1 = src[-stride * 3];
    const int16_t m2 = src[-stride * 2];
    const int16_t m3 = src[-stride    ];
    const int16_t m4 = src[0          ];
    const int16_t m5 = src[ stride    ];
    const int16_t m6 = src[ stride * 2];
    const int16_t m7 = src[ stride * 3];

    if (is_ctb_b) {
        src[-stride * 1] = ov_clip(((3 * m2 + 2 * m3 + m4 + m5 + m6 + 4) >> 3),m3 - tc, m3 + tc); // p)0
        src[0          ] = ov_clip(((2 * m2 + m3 + 2 * m4 + m5 + m6 + m7 + 4) >> 3),m4 - tc, m4 + tc); // q)0
        src[ stride * 1] = ov_clip(((m2 + m3 + m4 + 2 * m5 + m6 + 2 * m7 + 4) >> 3),m5 - tc, m5 + tc); // q)1
        src[ stride * 2] = ov_clip(((m3 + m4 + m5 + 2 * m6 + 3 * m7 + 4) >> 3),m6 - tc, m6 + tc);      // q)2
    } else {
        src[-stride * 3] = ov_clip(((3 * m0 + 2 * m1 + m2 + m3 + m4 + 4) >> 3),m1 - tc, m1 + tc);      // p)2
        src[-stride * 2] = ov_clip(((2 * m0 + m1 + 2 * m2 + m3 + m4 + m5 + 4) >> 3),m2 - tc, m2 + tc); // p)1
        src[-stride * 1] = ov_clip(((m0 + m1 + m2 + 2 * m3 + m4 + m5 + m6 + 4) >> 3),m3 - tc, m3 + tc); // p)0
        src[ 0         ] = ov_clip(((m1 + m2 + m3 + 2 * m4 + m5 + m6 + m7 + 4) >> 3),m4 - tc, m4 + tc); // q)0
        src[ stride * 1] = ov_clip(((m2 + m3 + m4 + 2 * m5 + m6 + 2 * m7 + 4) >> 3),m5 - tc, m5 + tc);  // q)1
        src[ stride * 2] = ov_clip(((m3 + m4 + m5 + 2 * m6 + 3 * m7 + 4) >> 3),m6 - tc, m6 + tc);       // q)2
    }
}

static inline void
filter_chroma_weak(uint16_t* src, const int stride, const int tc)
{
    int delta;

    const int16_t m2 = src[-stride * 2];
    const int16_t m3 = src[-stride    ];
    const int16_t m4 = src[0          ];
    const int16_t m5 = src[ stride    ];

    delta = ov_clip(((((m4 - m3) << 2) + m2 - m5 + 4) >> 3), -tc, tc);
    src[-stride] = ov_clip(m3 + delta, 0, 1023);
    src[0]       = ov_clip(m4 - delta, 0, 1023);
}

/* Check if filter is 3 or 1 sample large based on other left edges */
static uint64_t
derive_large_map_from_ngh(const uint64_t *src_map)
{
    const uint64_t *fwd = src_map + 1;
    const uint64_t *bwd = src_map - 1;
    uint64_t dst_map = 0;

    /* If an edge is detected on either of those maps
     * the corresponding bit in dst_map is set to one
     */
    dst_map |= bwd[ 0] | fwd[0];
    dst_map |= bwd[-1] | fwd[1];
    dst_map |= bwd[-2] | fwd[2];

    /* Revert the bits on the map so the that output
     * bits set to one correspond to large blocks edges
     */
    return ~dst_map;
}

static void
filter_veritcal_edge_c(const struct DBFInfo *const dbf_info, uint16_t *src, ptrdiff_t stride,
                       uint8_t qp, uint64_t bs2_map, uint64_t large_map_q)
{
    const uint8_t is_large = large_map_q & 0x1;
    const uint8_t is_bs2   = bs2_map     & 0x1;

    /* Note there should not be any need to check anything here since condition
     * is already checked in edge_map
     * FIXME check on inter when bs1 is enabled
     */
    if (is_large || is_bs2) {
        uint16_t *src0 = src;
        uint16_t *src1 = src + stride;

        const struct DBFParams dbf_params = compute_dbf_limits(dbf_info, qp, 1 + is_bs2);
        uint8_t is_strong = 0;

        if (is_large) {
            const int dp0 = compute_dp((int16_t *)src0, 1);
            const int dq0 = compute_dq((int16_t *)src0, 1);
            const int dp3 = compute_dp((int16_t *)src1, 1);
            const int dq3 = compute_dq((int16_t *)src1, 1);

            const int d0 = dp0 + dq0;
            const int d3 = dp3 + dq3;

            const int d = d0 + d3;

            is_strong = (d < dbf_params.beta) &&
                        (2 * d0 < (dbf_params.beta >> 2)) &&
                        (2 * d3 < (dbf_params.beta >> 2)) &&
                        use_strong_filter_c(src0, 1, dbf_params.beta, dbf_params.tc) &&
                        use_strong_filter_c(src1, 1, dbf_params.beta, dbf_params.tc);

            if (is_strong) {
                int j;
                for (j = 0; j < 2; ++j) {
                    filter_chroma_strong(src, 1, dbf_params.tc);
                    src += stride;
                }
            }
        }

        if (!is_strong) {
            int j;
            for (j = 0; j < 2; ++j) {
                filter_chroma_weak(src, 1, dbf_params.tc);
                src += stride;
            }
        }
    }
}

/* Filter vertical edges */
static void
vvc_dbf_chroma_hor(uint16_t *src_cb, uint16_t *src_cr, int stride,
                   const struct DBFInfo *const dbf_info,
                   uint8_t nb_unit_h, int is_last_h, uint8_t nb_unit_w,
                   uint8_t ctu_lft)
{

    const int blk_stride = stride << 1;
    /* Mask applied to edge_mask based on CTU height */
    const uint64_t vedge_mask = ((uint64_t)1 << nb_unit_h) - 1;
    const uint64_t *const edg_map_tab = &dbf_info->ctb_bound_ver_c[8];
    int i;

    /* The number of edges to process (should be ((1 << log2_ctu_s) >> 4) since we start
     * finish from CTU left border to the last edge before the right CTU border
     * (Note we know the CTU right border is an implicit edge so we can set it to 0xFF)
     */
    const uint8_t nb_vedge = ((nb_unit_w + 3) >> 2);
    const uint8_t skip_first = !ctu_lft;

    src_cb += skip_first << 3;
    src_cr += skip_first << 3;

    for (i = skip_first; i < nb_vedge; i++) {
        /* FIXME chroma_edges could be stored on a smaller grid */
        uint8_t edge_idx = i << 2;

        uint64_t bs2_map = dbf_info->bs2_map_c.ver  [edge_idx];
        uint64_t bs1_map = dbf_info->bs1_map_cb.ver [edge_idx];

        uint64_t edge_map = edg_map_tab[edge_idx];

        /* FIXME Use directly CTU start and modify maps storage and rotation
         */
        /* Discard first edge corresponding to upper CTU and mask 
         * out of picture edge
         */
        edge_map &= vedge_mask;

        /* Discard non filtered edges from edge_map */
        edge_map &= bs2_map | bs1_map;

        if (edge_map) {
            uint64_t large_map_q = derive_large_map_from_ngh(&edg_map_tab[edge_idx]);

            /* FIXME use absolute QP maps */
            const uint8_t *qp_col = &dbf_info->qp_map_cb.hor[36 + edge_idx];
            uint16_t *src = src_cb;

            /* Discard non filtered edges from edge_map */
            edge_map &= bs2_map | (bs1_map & large_map_q);

            /* Note while is expected instead of do since edge_map can be zero
             * if edge_map consists only in bs1 with non large block sizes
             */
            while (edge_map) {
                uint8_t nb_skipped_blk = ov_ctz64(edge_map);
                uint8_t qp;

                /* Skip non filtered edges */
                large_map_q >>= nb_skipped_blk;
                bs2_map     >>= nb_skipped_blk;
                qp_col       += nb_skipped_blk * 34;
                src          += nb_skipped_blk * blk_stride;

                qp = (qp_col[-1] + qp_col[0] + 1) >> 1;

                filter_veritcal_edge_c(dbf_info, src, stride, qp, bs2_map, large_map_q);

                edge_map    >>= nb_skipped_blk + 1;
                large_map_q >>= 1;
                bs2_map     >>= 1;

                src += blk_stride;
                qp_col += 34;
            }
        }
        src_cb += 1 << 3;
    }

    for (i = skip_first; i < nb_vedge; i++) {
        uint8_t edge_idx = i << 2;

        uint64_t bs2_map = dbf_info->bs2_map_c.ver  [edge_idx];
        uint64_t bs1_map = dbf_info->bs1_map_cr.ver [edge_idx];

        uint64_t edge_map = edg_map_tab[edge_idx];

        edge_map &= vedge_mask;
        edge_map &= bs2_map | bs1_map;

        if (edge_map) {
            uint64_t large_map_q = derive_large_map_from_ngh(&edg_map_tab[edge_idx]);

            const uint8_t *qp_col = &dbf_info->qp_map_cr.hor[36 + edge_idx];

            uint16_t *src = src_cr;

            edge_map &= bs2_map | (bs1_map & large_map_q);

             while (edge_map) {
                uint8_t nb_skipped_blk = ov_ctz64(edge_map);
                uint8_t qp;

                /* Skip non filtered edges */
                large_map_q >>= nb_skipped_blk;
                bs2_map     >>= nb_skipped_blk;
                qp_col       += nb_skipped_blk * 34;
                src          += nb_skipped_blk * blk_stride;

                qp = (qp_col[-1] + qp_col[0] + 1) >> 1;

                filter_veritcal_edge_c(dbf_info, src, stride, qp, bs2_map, large_map_q);

                edge_map    >>= nb_skipped_blk + 1;
                large_map_q >>= 1;
                bs2_map     >>= 1;

                src += blk_stride;
                qp_col += 34;
            }
        }
        src_cr += 1 << 3;
    }
}

static void
filter_horizontal_edge_c(const struct DBFInfo *const dbf_info, uint16_t *src, ptrdiff_t stride,
                         uint8_t qp, uint64_t bs2_map, uint64_t large_map_q, uint8_t is_ctb_b)
{
    const uint8_t is_large = large_map_q & 0x1;
    const uint8_t is_bs2   = bs2_map     & 0x1;

    /* Note there should not be any need to check anything here since condition
     * is already checked in edge_map
     * FIXME check on inter when bs1 is enabled
     */
    if (is_large || is_bs2) {
        uint16_t *src0 = src;
        uint16_t *src1 = src + 1;

        const struct DBFParams dbf_params = compute_dbf_limits(dbf_info, qp, 1 + is_bs2);
        uint8_t is_strong = 0;

        if (is_large) {
            const int dp0 = compute_dp_c((int16_t *)src0, stride, is_ctb_b);
            const int dq0 = compute_dq((int16_t *)src0, stride);
            const int dp3 = compute_dp_c((int16_t *)src1, stride, is_ctb_b);
            const int dq3 = compute_dq((int16_t *)src1, stride);

            const int d0 = dp0 + dq0;
            const int d3 = dp3 + dq3;

            const int d = d0 + d3;

            is_strong = (d < dbf_params.beta) &&
                        (2 * d0 < (dbf_params.beta >> 2)) &&
                        (2 * d3 < (dbf_params.beta >> 2)) &&
                        use_strong_filter_c2(src0, stride, dbf_params.beta, dbf_params.tc, is_ctb_b) &&
                        use_strong_filter_c2(src1, stride, dbf_params.beta, dbf_params.tc, is_ctb_b);

            if (is_strong) {
                int j;
                for (j = 0; j < 2; ++j) {
                    filter_chroma_strong_c(src, stride, dbf_params.tc, is_ctb_b);
                    src++;
                }
            }
        }

        if (!is_strong) {
            int j;
            for (j = 0; j < 2; ++j) {
                filter_chroma_weak(src, stride, dbf_params.tc);
                src++;
            }
        }
    }
}

static void
vvc_dbf_chroma_ver(uint16_t *src_cb, uint16_t *src_cr, int stride,
                   const struct DBFInfo *const dbf_info,
                   uint8_t nb_unit_w, int is_last_w, uint8_t nb_unit_h, uint8_t is_last_h,
                   uint8_t ctu_abv)
{
    const int blk_stride = 1 << 1;
    const uint64_t hedge_mask = (((uint64_t)1 << (nb_unit_w + (!!is_last_w << 1))) - 1);
    const uint8_t nb_hedge = ((nb_unit_h + 3) >> 2);
    const uint8_t skip_first = !ctu_abv;
    const uint64_t *const edg_map_tab = &dbf_info->ctb_bound_hor_c[8];
    int i;

    src_cb -= blk_stride << 1;
    src_cr -= blk_stride << 1;

    src_cb += (skip_first * stride) << 3;
    src_cr += (skip_first * stride) << 3;

    for (i = skip_first; i < nb_hedge; i++) {
        uint8_t edge_idx = i << 2;

        uint64_t edge_map = edg_map_tab[edge_idx];

        uint64_t bs2_map  = dbf_info->bs2_map_c.hor[edge_idx];
        uint64_t bs1_map  = dbf_info->bs1_map_cb.hor[edge_idx];

        edge_map &= hedge_mask;
        edge_map &= bs2_map | bs1_map;

        if (edge_map) {
            const uint8_t *qp_row = &dbf_info->qp_map_cb.hor[edge_idx * 34];

            uint64_t large_map_q = derive_large_map_from_ngh(&edg_map_tab[edge_idx]);

            uint8_t is_ctb_b = i == 0;
            uint16_t *src = src_cb;

            edge_map &= bs2_map | (bs1_map & large_map_q);

            while(edge_map) {
                uint8_t nb_skipped_blk = ov_ctz64(edge_map);
                uint8_t qp;

                /* Skip non filtered edges */
                large_map_q >>= nb_skipped_blk;
                bs2_map     >>= nb_skipped_blk;
                qp_row       += nb_skipped_blk;
                src          += nb_skipped_blk * blk_stride;

                qp = (qp_row[0] + qp_row[34] + 1) >> 1;

                filter_horizontal_edge_c(dbf_info, src, stride, qp, bs2_map,
                                         large_map_q, is_ctb_b);

                edge_map    >>= nb_skipped_blk + 1;
                large_map_q >>= 1;
                bs2_map     >>= 1;

                src += blk_stride;
                qp_row++;
            }
        }
        src_cb += stride << 3;
    }

    for (i = skip_first; i < nb_hedge; i++) {
        uint8_t edge_idx = i << 2;

        uint64_t edge_map = edg_map_tab[edge_idx];

        uint64_t bs2_map  = dbf_info->bs2_map_c.hor[edge_idx];
        uint64_t bs1_map  = dbf_info->bs1_map_cr.hor[edge_idx];

        edge_map &= hedge_mask;
        edge_map &= bs2_map | bs1_map;

        if (edge_map) {
            uint64_t large_map_q = derive_large_map_from_ngh(&edg_map_tab[edge_idx]);
            const uint8_t *qp_row = &dbf_info->qp_map_cr.hor[edge_idx * 34];
            uint16_t *src = src_cr;
            uint8_t is_ctb_b = i == 0;

            edge_map &= bs2_map | (bs1_map & large_map_q);

            while(edge_map) {
                uint8_t nb_skipped_blk = ov_ctz64(edge_map);
                uint8_t qp;

                /* Skip non filtered edges */
                large_map_q >>= nb_skipped_blk;
                bs2_map     >>= nb_skipped_blk;
                qp_row       += nb_skipped_blk;
                src          += nb_skipped_blk * blk_stride;

                qp = (qp_row[0] + qp_row[34] + 1) >> 1;

                filter_horizontal_edge_c(dbf_info, src, stride, qp, bs2_map,
                                         large_map_q, is_ctb_b);

                edge_map    >>= nb_skipped_blk + 1;
                large_map_q >>= 1;
                bs2_map     >>= 1;

                src += blk_stride;
                qp_row++;
            }
        }
        src_cr += stride << 3;
    }
}

static void
filter_veritcal_edge(const struct DBFInfo *const dbf_info, uint16_t *src, ptrdiff_t stride,
                     uint8_t qp, uint64_t bs2_map, uint64_t large_p_map,
                     uint64_t large_q_map, uint64_t small_map,
                     uint64_t affine_p, uint64_t affine_q, uint64_t aff_edg_1)
{
    int max_l_p = small_map & 0x1 ? 1 : (large_p_map & 0x1) ? 7 : 3;
    int max_l_q = small_map & 0x1 ? 1 : (large_q_map & 0x1) ? 7 : 3;
    /*FIXME check if small and large can be both true */
    //uint8_t is_large_p = large_p_map & 0x1 & !(small_map & 0x1);
    //uint8_t is_large_q = large_q_map & 0x1 & !(small_map & 0x1);

    uint8_t bs = 1 + (bs2_map & 0x1);
    /*FIXME subblock handling */

    const struct DBFParams dbf_params = compute_dbf_limits(dbf_info, qp, bs);
    uint16_t* src0 = src;
    uint16_t* src3 = src + stride * 3;

    const int dp0 = compute_dp((int16_t *)src0, 1);
    const int dq0 = compute_dq((int16_t *)src0, 1);
    const int dp3 = compute_dp((int16_t *)src3, 1);
    const int dq3 = compute_dq((int16_t *)src3, 1);

    uint8_t use_strong_large = 0;

    uint8_t is_aff_p = affine_p & 0x1;
    uint8_t is_aff_q = affine_q & 0x1;

    if (aff_edg_1 & 0x1) {
        max_l_p = OVMIN(2, max_l_p);
        max_l_q = OVMIN(2, max_l_q);
        max_l_p = max_l_q = OVMIN(max_l_p, max_l_q);
    }

    if (is_aff_p) {
        max_l_p = OVMIN(5, max_l_p);
    }
    if (is_aff_q) {
        max_l_q = OVMIN(5, max_l_q);
    }

    if (max_l_p > 3 || max_l_q > 3) {
        int dp0L = dp0;
        int dq0L = dq0;
        int dp3L = dp3;
        int dq3L = dq3;

        if (max_l_p > 3) {
            dp0L += compute_dp((int16_t *)src0 - 3, 1) + 1;
            dp3L += compute_dp((int16_t *)src3 - 3, 1) + 1;
            dp0L >>= 1;
            dp3L >>= 1;
        }

        if (max_l_q > 3) {
            dq0L += compute_dq((int16_t *)src0 + 3, 1) + 1;
            dq3L += compute_dq((int16_t *)src3 + 3, 1) + 1;
            dq0L >>= 1;
            dq3L >>= 1;
        }

        int d0L = dp0L + dq0L;
        int d3L = dp3L + dq3L;

        int dL = d0L + d3L;

        use_strong_large = (dL < dbf_params.beta) &&
            ((d0L << 1) < (dbf_params.beta >> 4)) &&
            ((d3L << 1) < (dbf_params.beta >> 4)) &&
            use_strong_filter_l0((int16_t *)src0, 1, dbf_params.beta, dbf_params.tc, max_l_p, max_l_q) &&
            use_strong_filter_l0((int16_t *)src3, 1, dbf_params.beta, dbf_params.tc, max_l_p, max_l_q);
        if (use_strong_large) {
            int16_t *_src = (int16_t *)src0;
            /* FIXME should already be 3 or higher since we would be small otherwise */
            max_l_p = max_l_p > 3 ? max_l_p : 3;
            max_l_q = max_l_q > 3 ? max_l_q : 3;
            for (int i = 0; i < 4; i++) {
                filter_luma_strong_large(_src, 1, dbf_params.tc, max_l_p, max_l_q);
                _src += stride;
            }
        }
    }

    if (!use_strong_large) {
        const int d0 = dp0 + dq0;
        const int d3 = dp3 + dq3;
        const int d  = d0  + d3;

        if (d < dbf_params.beta) {
            //uint8_t is_not_small = !(small_map & 0x1);
            uint8_t sw = (max_l_p >= 3 && max_l_q >= 3);//is_not_small;

            sw = sw && ((d0 << 1) < (dbf_params.beta >> 2))
                && ((d3 << 1) < (dbf_params.beta >> 2))
                && use_strong_filter_l1((int16_t *)src0, 1, dbf_params.beta, dbf_params.tc)
                && use_strong_filter_l1((int16_t *)src3, 1, dbf_params.beta, dbf_params.tc);

            if (sw){
                int16_t *_src = (int16_t *)src0;
                for (int i = 0; i < 4; i++) {
                    filter_luma_strong_small(_src, 1, dbf_params.tc);
                    _src += stride;
                }
            } else {
                const int dp = dp0 + dp3;
                const int dq = dq0 + dq3;
                const int side_thd = (dbf_params.beta + (dbf_params.beta >> 1)) >> 3;
                const int th_cut  = dbf_params.tc * 10;
                //uint8_t extend_p = is_not_small && (dp < side_thd);
                //uint8_t extend_q = is_not_small && (dq < side_thd);
                uint8_t extend_p = (dp < side_thd) && (max_l_p > 1 && max_l_q > 1);
                uint8_t extend_q = (dq < side_thd) && (max_l_p > 1 && max_l_q > 1);
                int16_t *_src = (int16_t *)src0;
                for (int i = 0; i < 4; i++) {
                    filter_luma_weak(_src, 1, dbf_params.tc, th_cut, extend_p, extend_q);
                    _src += stride;
                }
            }
        }
    }
}

static void
vvc_dbf_ctu_hor(uint16_t *src, int stride, const struct DBFInfo *const dbf_info,
                uint8_t nb_unit_h, int is_last_h, uint8_t nb_unit_w, uint8_t ctu_lft)
{
    const int blk_stride = stride << 2; 
    const uint64_t vedge_mask = ((uint64_t)1 << nb_unit_h) - 1;

    const uint64_t *edg_map = &dbf_info->ctb_bound_ver[8];
    const uint64_t *aff_edg_map = &dbf_info->aff_edg_ver[8];
    const uint8_t skip_first = !ctu_lft;

    int i;

    src += skip_first << 2;

    for (i = skip_first; i < nb_unit_w; ++i) {
        uint16_t* src_tmp = src;

        uint64_t edg_msk = edg_map[i] | aff_edg_map[i];
        uint64_t bs1_map  = dbf_info->bs1_map.ver[i];
        uint64_t bs2_map  = dbf_info->bs2_map.ver[i];

        edg_msk &= vedge_mask;
        edg_msk &= bs2_map | bs1_map;

        if (edg_msk) {
            uint64_t large_p_map = derive_size_3_map(&edg_map[i - 7]);
            uint64_t large_q_map = derive_size_3_map(&edg_map[i + 1]);

            uint64_t small_map = edg_map[i - 1] | edg_map[i + 1] | aff_edg_map[i - 1] | aff_edg_map[i + 1];
            uint64_t affine_p = dbf_info->affine_map.ver[i];
            uint64_t affine_q = dbf_info->affine_map.ver[i + 1];

#if 0
            uint64_t aff_edg_1 = aff_edg_map[i] & ((~aff_edg_map[i - 2] | ~aff_edg_map[i + 2])
                                                & (~edg_map[i - 3] | ~edg_map[i + 3]));
#else
            uint64_t aff_edg_1 = aff_edg_map[i] & (edg_map[i - 2] | edg_map[i + 2] |  edg_map[i - 1] | edg_map[i + 1]) & ~edg_map[i];
#endif

            const uint8_t *qp_col = &dbf_info->qp_map_y.hor[36 + i];

            //if (i != 16) {
               large_p_map  &= ~(aff_edg_map[i] & (~edg_map[i]));
               large_q_map  &= ~(aff_edg_map[i] & (~edg_map[i]));
            //}
            do {
                uint8_t nb_skipped_blk = ov_ctz64(edg_msk);
                uint8_t qp;

                /* Skip non filtered edges */
                large_p_map >>= nb_skipped_blk;
                large_q_map >>= nb_skipped_blk;
                affine_p    >>= nb_skipped_blk;
                affine_q    >>= nb_skipped_blk;
                aff_edg_1   >>= nb_skipped_blk;
                small_map   >>= nb_skipped_blk;
                bs2_map     >>= nb_skipped_blk;
                qp_col       += nb_skipped_blk * 34;
                src_tmp      += nb_skipped_blk * blk_stride;

                qp = (qp_col[-1] + qp_col[0] + 1) >> 1;

                filter_veritcal_edge(dbf_info, src_tmp, stride, qp, bs2_map, large_p_map,
                                     large_q_map, small_map, affine_p, affine_q, aff_edg_1);

                edg_msk  >>= nb_skipped_blk + 1;
                bs2_map   >>= 1;

                small_map   >>= 1;
                large_p_map >>= 1;
                large_q_map >>= 1;

                affine_p >>= 1;
                affine_q >>= 1;
                aff_edg_1 >>= 1;

                src_tmp += blk_stride;
                qp_col += 34;
            } while (edg_msk);
        }
        src += 1 << 2;
    }
}

static void
filter_horizontal_edge(const struct DBFInfo *const dbf_info, uint16_t *src, ptrdiff_t stride,
                       uint8_t qp, uint64_t bs2_map, uint64_t large_p_map,
                       uint64_t large_q_map, uint64_t small_map,
                       uint64_t affine_p, uint64_t affine_q, uint64_t aff_edg_1)
{
    int max_l_p = small_map & 0x1 ? 1 : (large_p_map & 0x1) ? 7 : 3;
    int max_l_q = small_map & 0x1 ? 1 : (large_q_map & 0x1) ? 7 : 3;
    uint8_t bs = 1 + (bs2_map & 0x1);

    /*FIXME subblock handling */

    const struct DBFParams dbf_params = compute_dbf_limits(dbf_info, qp, bs);

    int16_t *src0 = (int16_t *)src;
    int16_t *src3 = (int16_t *)src + 3;

    const int dp0 = compute_dp((int16_t *)src0, stride);
    const int dq0 = compute_dq((int16_t *)src0, stride);
    const int dp3 = compute_dp((int16_t *)src3, stride);
    const int dq3 = compute_dq((int16_t *)src3, stride);

    uint8_t use_strong_large = 0;

#if 1
    uint8_t is_aff_p = affine_p & 0x1;
    uint8_t is_aff_q = affine_q & 0x1;
    if (is_aff_p) {
        max_l_p = OVMIN(5, max_l_p);
    }

    if (is_aff_q) {
        max_l_q = OVMIN(5, max_l_q);
    }
#endif

#if 1
    if (aff_edg_1 & 0x1) {
        max_l_p = OVMIN(2, max_l_p);
        max_l_q = OVMIN(2, max_l_q);
        max_l_p = max_l_q = OVMIN(max_l_p, max_l_q);
    }
#endif
    if (max_l_p > 3 || max_l_q > 3) {
        int dp0L = dp0;
        int dq0L = dq0;
        int dp3L = dp3;
        int dq3L = dq3;

        if (max_l_p > 3) {
            dp0L = (dp0L + compute_dp(src0 - 3 * stride, stride) + 1) >> 1;
            dp3L = (dp3L + compute_dp(src3 - 3 * stride, stride) + 1) >> 1;
        }

        if (max_l_q > 3) {
            dq0L = (dq0L + compute_dq(src0 + 3 * stride, stride) + 1) >> 1;
            dq3L = (dq3L + compute_dq(src3 + 3 * stride, stride) + 1) >> 1;
        }

        int d0L = dp0L + dq0L;
        int d3L = dp3L + dq3L;

        int dL = d0L + d3L;
        use_strong_large = (dL < dbf_params.beta) &&
            ((d0L << 1) < (dbf_params.beta >> 4)) &&
            ((d3L << 1) < (dbf_params.beta >> 4)) &&
            use_strong_filter_l0(src0, stride, dbf_params.beta, dbf_params.tc, max_l_p, max_l_q) &&
            use_strong_filter_l0(src3, stride, dbf_params.beta, dbf_params.tc, max_l_p, max_l_q);
    }

    if (use_strong_large) {
            max_l_p = max_l_p > 3 ? max_l_p : 3;
            max_l_q = max_l_q > 3 ? max_l_q : 3;
        for (int i = 0; i < 4; i++) {
            int16_t *_src = src0 + i;
            filter_luma_strong_large(_src, stride, dbf_params.tc, max_l_p, max_l_q);
        }
    } else {
        const int d0 = dp0 + dq0;
        const int d3 = dp3 + dq3;
        const int d  = d0 + d3;

        if (d < dbf_params.beta) {
            uint8_t sw = (max_l_p >= 3 && max_l_q >= 3);

            sw = sw && ((d0 << 1) < (dbf_params.beta >> 2))
                && ((d3 << 1) < (dbf_params.beta >> 2))
                && use_strong_filter_l1(src0, stride, dbf_params.beta, dbf_params.tc)
                && use_strong_filter_l1(src3, stride, dbf_params.beta, dbf_params.tc);

            if (sw){
                for (int i = 0; i < 4; i++) {
                    int16_t *_src = src0 + i;
                    filter_luma_strong_small(_src, stride, dbf_params.tc);
                }
            } else {
                const int dp = dp0 + dp3;
                const int dq = dq0 + dq3;
                const int side_thd = (dbf_params.beta + (dbf_params.beta >> 1)) >> 3;
                const int th_cut  = dbf_params.tc * 10;
                uint8_t extend_p = (dp < side_thd) && (max_l_p > 1 && max_l_q > 1);
                uint8_t extend_q = (dq < side_thd) && (max_l_p > 1 && max_l_q > 1);
                for (int i = 0; i < 4; i++) {
                    int16_t *_src = src0 + i;
                    filter_luma_weak(_src, stride, dbf_params.tc, th_cut, extend_p, extend_q);
                }
            }
        }
    }
}

static void
vvc_dbf_ctu_ver(uint16_t *src, int stride, const struct DBFInfo *const dbf_info,
                uint8_t nb_unit_w, int is_last_w, uint8_t nb_unit_h, uint8_t ctu_abv)
{
    const int blk_stride = 1 << 2;
    const uint64_t hedge_mask = ((uint64_t)1 << (nb_unit_w + (!!is_last_w << 1))) - 1;
    int i;

    const uint64_t *edg_map = &dbf_info->ctb_bound_hor[8];
    const uint64_t *aff_edg_map = &dbf_info->aff_edg_hor[8];
    uint8_t skip_first = !ctu_abv;

    /* Filtering vertical edges on the whole would overlap with next CTU first
     * vertical edge.
     * Since max filter length is 7 we stop the horizontal process 2 units before
     * current CTU end and process the two previous units in order to finish filtering
     * the horizontal edges of previous CTU.
     */
    src -= blk_stride << 1;
    src += (skip_first * stride) << 2;

    for (i = skip_first; i < nb_unit_h; ++i) {
        uint16_t *src_tmp = src;

        uint64_t edg_msk = edg_map[i] | aff_edg_map[i];
        uint64_t bs2_map = dbf_info->bs2_map.hor[i];
        uint64_t bs1_map = dbf_info->bs1_map.hor[i];

        edg_msk &= hedge_mask;
        edg_msk &= bs2_map | bs1_map;

        if (edg_msk) {
            uint64_t large_p_map = derive_size_3_map(&edg_map[i - 7]);
            uint64_t large_q_map = derive_size_3_map(&edg_map[i + 1]);
            uint64_t small_map = edg_map[i - 1] | edg_map[i + 1] | aff_edg_map[i + 1] | aff_edg_map[i - 1];
#if 0
            uint64_t aff_edg_1 = aff_edg_map[i] & ((~aff_edg_map[i - 2] | ~aff_edg_map[i + 2])
                                                & (~edg_map[i - 3] | ~edg_map[i + 3]));
#else
            uint64_t aff_edg_1 = aff_edg_map[i] & (edg_map[i - 2] | edg_map[i + 2] | edg_map[i - 1] | edg_map[i + 1]) & ~edg_map[i];
#endif

            const uint8_t *qp_row = &dbf_info->qp_map_y.hor[34 * i];
            uint64_t affine_p = dbf_info->affine_map.hor[i];
            uint64_t affine_q = dbf_info->affine_map.hor[i + 1];
    //        if (i != 16) {
               large_p_map  &= ~(aff_edg_map[i] & (~edg_map[i]));
               large_q_map  &= ~(aff_edg_map[i] & (~edg_map[i]));
    //        }

            do {
                uint8_t nb_skipped_blk = ov_ctz64(edg_msk);
                uint8_t qp;

                /* Skip non filtered edges */
                large_p_map >>= nb_skipped_blk;
                large_q_map >>= nb_skipped_blk;
                affine_p >>= nb_skipped_blk;
                affine_q >>= nb_skipped_blk;
                aff_edg_1 >>= nb_skipped_blk;
                small_map   >>= nb_skipped_blk;
                bs2_map     >>= nb_skipped_blk;
                qp_row       += nb_skipped_blk;
                src_tmp      += nb_skipped_blk * blk_stride;

                qp = (qp_row[0] + qp_row[34] + 1) >> 1;

                filter_horizontal_edge(dbf_info, src_tmp, stride, qp, bs2_map,
                                       large_p_map, large_q_map, small_map,
                                       affine_p, affine_q, aff_edg_1);

                edg_msk  >>= nb_skipped_blk + 1;
                bs2_map   >>= 1;

                small_map   >>= 1;
                large_p_map >>= 1;
                large_q_map >>= 1;
                affine_p >>= 1;
                affine_q >>= 1;
                aff_edg_1 >>= 1;

                src_tmp += blk_stride;
                qp_row++;
            } while(edg_msk);
        }

        src += stride << 2;
    }
}

#if 1
void
rcn_dbf_ctu(const struct OVRCNCtx  *const rcn_ctx, struct DBFInfo *const dbf_info,
            uint8_t log2_ctu_s, uint8_t last_x, uint8_t last_y)
{
    const struct OVBuffInfo *const fbuff = &rcn_ctx->frame_buff;

    uint8_t nb_unit = (1 << log2_ctu_s) >> 2;
    /* FIXME give as argument */
    uint8_t ctu_lft = rcn_ctx->ctudec->ctu_ngh_flags & CTU_LFT_FLG;
    uint8_t ctu_abv = rcn_ctx->ctudec->ctu_ngh_flags & CTU_UP_FLG;

    /* Force transform_edges on implicit transform trees */
    uint64_t *edg_map_tab = &dbf_info->ctb_bound_ver_c[8];
    edg_map_tab[16] = -1ll;
    edg_map_tab = &dbf_info->ctb_bound_ver[8];
    edg_map_tab[16] = -1ll;
    edg_map_tab = &dbf_info->ctb_bound_hor[8];
    edg_map_tab[16] = -1ll;
    edg_map_tab = &dbf_info->ctb_bound_hor_c[8];
    edg_map_tab[16] = -1ll;
    uint64_t *aff_edg_map = &dbf_info->aff_edg_hor[8];
    aff_edg_map[16] = -1ll;
    aff_edg_map = &dbf_info->aff_edg_ver[8];
    aff_edg_map[16] = -1ll;


    #if 1
    if (!dbf_info->disable_h)
    vvc_dbf_ctu_hor(fbuff->y, fbuff->stride, dbf_info, nb_unit, !!last_y, nb_unit, ctu_lft);
    if (!dbf_info->disable_v)
    vvc_dbf_ctu_ver(fbuff->y, fbuff->stride, dbf_info, nb_unit, !!last_x, nb_unit, ctu_abv);

    if (!dbf_info->disable_h)
    vvc_dbf_chroma_hor(fbuff->cb, fbuff->cr, fbuff->stride_c, dbf_info,
                       nb_unit, !!last_y, nb_unit, ctu_lft);

    if (!dbf_info->disable_v)
    vvc_dbf_chroma_ver(fbuff->cb, fbuff->cr, fbuff->stride_c, dbf_info,
                       nb_unit, !!last_x, nb_unit, !!last_y, ctu_abv);
                       #endif

}

void
rcn_dbf_truncated_ctu(const struct OVRCNCtx  *const rcn_ctx, struct DBFInfo *const dbf_info,
                      uint8_t log2_ctu_s, uint8_t last_x, uint8_t last_y, uint8_t ctu_w, uint8_t ctu_h)
{
    const struct OVBuffInfo *const fbuff = &rcn_ctx->frame_buff;

    uint8_t nb_unit_w = (ctu_w) >> 2;
    uint8_t nb_unit_h = (ctu_h) >> 2;
    /* FIXME give as argument */
    uint8_t ctu_lft = rcn_ctx->ctudec->ctu_ngh_flags & CTU_LFT_FLG;
    uint8_t ctu_abv = rcn_ctx->ctudec->ctu_ngh_flags & CTU_UP_FLG;

    #if 1
    if (!dbf_info->disable_h)
    vvc_dbf_ctu_hor(fbuff->y, fbuff->stride, dbf_info, nb_unit_h, !!last_y, nb_unit_w, ctu_lft);
    if (!dbf_info->disable_v)
    vvc_dbf_ctu_ver(fbuff->y, fbuff->stride, dbf_info, nb_unit_w, !!last_x, nb_unit_h, ctu_abv);

    if (!dbf_info->disable_h)
    vvc_dbf_chroma_hor(fbuff->cb, fbuff->cr, fbuff->stride_c, dbf_info,
                       nb_unit_h, !!last_y, nb_unit_w, ctu_lft);

    if (!dbf_info->disable_v)
    vvc_dbf_chroma_ver(fbuff->cb, fbuff->cr, fbuff->stride_c, dbf_info,
                       nb_unit_w, !!last_x, nb_unit_h, !!last_y, ctu_abv);
                       #endif

}
#endif

