#pragma once

// Included only by browser_js_math.c. Macro scopes give each unchanged musl
// source its own private identifiers while retaining a single native object.

// log_data.c
#include "../../third_party/musl_math/log_data.c"
#undef N

// log2_data.c
#include "../../third_party/musl_math/log2_data.c"
#undef N

// exp.c
#define specialcase xj380_js_exp_specialcase
#define top12 xj380_js_exp_top12
#include "../../third_party/musl_math/exp.c"
#undef specialcase
#undef top12
#undef N
#undef InvLn2N
#undef NegLn2hiN
#undef NegLn2loN
#undef Shift
#undef T
#undef C2
#undef C3
#undef C4
#undef C5

// log.c
#define top16 xj380_js_log_top16
#include "../../third_party/musl_math/log.c"
#undef top16
#undef T
#undef T2
#undef B
#undef A
#undef Ln2hi
#undef Ln2lo
#undef N
#undef OFF
#undef LO
#undef HI

// log2.c
#define top16 xj380_js_log2_top16
#include "../../third_party/musl_math/log2.c"
#undef top16
#undef T
#undef T2
#undef B
#undef A
#undef InvLn2hi
#undef InvLn2lo
#undef N
#undef OFF
#undef LO
#undef HI

// log10.c
#define ivln10hi xj380_js_log10_ivln10hi
#define ivln10lo xj380_js_log10_ivln10lo
#define log10_2hi xj380_js_log10_log10_2hi
#define log10_2lo xj380_js_log10_log10_2lo
#define Lg1 xj380_js_log10_Lg1
#define Lg2 xj380_js_log10_Lg2
#define Lg3 xj380_js_log10_Lg3
#define Lg4 xj380_js_log10_Lg4
#define Lg5 xj380_js_log10_Lg5
#define Lg6 xj380_js_log10_Lg6
#define Lg7 xj380_js_log10_Lg7
#include "../../third_party/musl_math/log10.c"
#undef ivln10hi
#undef ivln10lo
#undef log10_2hi
#undef log10_2lo
#undef Lg1
#undef Lg2
#undef Lg3
#undef Lg4
#undef Lg5
#undef Lg6
#undef Lg7

// log1p.c
#define ln2_hi xj380_js_log1p_ln2_hi
#define ln2_lo xj380_js_log1p_ln2_lo
#define Lg1 xj380_js_log1p_Lg1
#define Lg2 xj380_js_log1p_Lg2
#define Lg3 xj380_js_log1p_Lg3
#define Lg4 xj380_js_log1p_Lg4
#define Lg5 xj380_js_log1p_Lg5
#define Lg6 xj380_js_log1p_Lg6
#define Lg7 xj380_js_log1p_Lg7
#include "../../third_party/musl_math/log1p.c"
#undef ln2_hi
#undef ln2_lo
#undef Lg1
#undef Lg2
#undef Lg3
#undef Lg4
#undef Lg5
#undef Lg6
#undef Lg7

// expm1.c
#define o_threshold xj380_js_expm1_o_threshold
#define ln2_hi xj380_js_expm1_ln2_hi
#define ln2_lo xj380_js_expm1_ln2_lo
#define invln2 xj380_js_expm1_invln2
#define Q1 xj380_js_expm1_Q1
#define Q2 xj380_js_expm1_Q2
#define Q3 xj380_js_expm1_Q3
#define Q4 xj380_js_expm1_Q4
#define Q5 xj380_js_expm1_Q5
#include "../../third_party/musl_math/expm1.c"
#undef o_threshold
#undef ln2_hi
#undef ln2_lo
#undef invln2
#undef Q1
#undef Q2
#undef Q3
#undef Q4
#undef Q5

// __expo2.c
#define k xj380_js_expo2_k
#define kln2 xj380_js_expo2_kln2
#include "../../third_party/musl_math/__expo2.c"
#undef k
#undef kln2

// acos.c
#define pio2_hi xj380_js_acos_pio2_hi
#define pio2_lo xj380_js_acos_pio2_lo
#define pS0 xj380_js_acos_pS0
#define pS1 xj380_js_acos_pS1
#define pS2 xj380_js_acos_pS2
#define pS3 xj380_js_acos_pS3
#define pS4 xj380_js_acos_pS4
#define pS5 xj380_js_acos_pS5
#define qS1 xj380_js_acos_qS1
#define qS2 xj380_js_acos_qS2
#define qS3 xj380_js_acos_qS3
#define qS4 xj380_js_acos_qS4
#define R xj380_js_acos_R
#include "../../third_party/musl_math/acos.c"
#undef pio2_hi
#undef pio2_lo
#undef pS0
#undef pS1
#undef pS2
#undef pS3
#undef pS4
#undef pS5
#undef qS1
#undef qS2
#undef qS3
#undef qS4
#undef R

// asin.c
#define pio2_hi xj380_js_asin_pio2_hi
#define pio2_lo xj380_js_asin_pio2_lo
#define pS0 xj380_js_asin_pS0
#define pS1 xj380_js_asin_pS1
#define pS2 xj380_js_asin_pS2
#define pS3 xj380_js_asin_pS3
#define pS4 xj380_js_asin_pS4
#define pS5 xj380_js_asin_pS5
#define qS1 xj380_js_asin_qS1
#define qS2 xj380_js_asin_qS2
#define qS3 xj380_js_asin_qS3
#define qS4 xj380_js_asin_qS4
#define R xj380_js_asin_R
#include "../../third_party/musl_math/asin.c"
#undef pio2_hi
#undef pio2_lo
#undef pS0
#undef pS1
#undef pS2
#undef pS3
#undef pS4
#undef pS5
#undef qS1
#undef qS2
#undef qS3
#undef qS4
#undef R

// atan.c
#define atanhi xj380_js_atan_atanhi
#define atanlo xj380_js_atan_atanlo
#define aT xj380_js_atan_aT
#include "../../third_party/musl_math/atan.c"
#undef atanhi
#undef atanlo
#undef aT

// atan2.c
#define pi xj380_js_atan2_pi
#define pi_lo xj380_js_atan2_pi_lo
#include "../../third_party/musl_math/atan2.c"
#undef pi
#undef pi_lo

// acosh.c
#undef acosh
#define acosh xj380_js_acosh_positive
#include "../../third_party/musl_math/acosh.c"
#undef acosh
#define acosh xj380_js_acosh

// asinh.c
#include "../../third_party/musl_math/asinh.c"

// atanh.c
#include "../../third_party/musl_math/atanh.c"

// cbrt.c
#define B1 xj380_js_cbrt_B1
#define B2 xj380_js_cbrt_B2
#define P0 xj380_js_cbrt_P0
#define P1 xj380_js_cbrt_P1
#define P2 xj380_js_cbrt_P2
#define P3 xj380_js_cbrt_P3
#define P4 xj380_js_cbrt_P4
#include "../../third_party/musl_math/cbrt.c"
#undef B1
#undef B2
#undef P0
#undef P1
#undef P2
#undef P3
#undef P4

// cosh.c
#include "../../third_party/musl_math/cosh.c"

// sinh.c
#include "../../third_party/musl_math/sinh.c"

// tanh.c
#include "../../third_party/musl_math/tanh.c"

// hypot.c
#define sq xj380_js_hypot_sq
#include "../../third_party/musl_math/hypot.c"
#undef sq
#undef SPLIT

// trunc.c
#include "../../third_party/musl_math/trunc.c"

// round.c
#define toint xj380_js_round_toint
#include "../../third_party/musl_math/round.c"
#undef toint
#undef EPS
