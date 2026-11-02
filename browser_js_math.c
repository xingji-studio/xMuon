// Keep upstream math sources unchanged and isolate every linked math symbol.
// The imported routines and dependency/license audit are documented alongside
// third_party/musl_math/UPSTREAM.json. Do not compile this with fast-math.
#include <math.h>
#include <float.h>
#include "browser_js_math.h"

#if !defined(__x86_64__) || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024 || FLT_EVAL_METHOD != 0
#error "Browser math requires x86_64 binary64 evaluation"
#endif
#if defined(__FAST_MATH__) || __FINITE_MATH_ONLY__
#error "Browser math requires IEEE infinities, NaNs and signed zero"
#endif

#define hidden __attribute__((visibility("hidden")))
#define tan xj380_js_tan
#define sin xj380_js_sin
#define cos xj380_js_cos
#define fmod xj380_js_fmod
#define floor xj380_js_floor
#define fabs xj380_js_fabs
#define ceil xj380_js_ceil
#define pow xj380_js_pow
#define scalbn xj380_js_scalbn
#define sqrt xj380_js_sqrt
#define acos xj380_js_acos
#define acosh xj380_js_acosh
#define asin xj380_js_asin
#define asinh xj380_js_asinh
#define atan xj380_js_atan
#define atan2 xj380_js_atan2
#define atanh xj380_js_atanh
#define cbrt xj380_js_cbrt
#define cosh xj380_js_cosh
#define exp xj380_js_exp
#define expm1 xj380_js_expm1
#define hypot xj380_js_hypot
#define log xj380_js_log
#define log10 xj380_js_log10
#define log1p xj380_js_log1p
#define log2 xj380_js_log2
#define sinh xj380_js_sinh
#define tanh xj380_js_tanh
#define trunc xj380_js_trunc
#define round xj380_js_round
#define __tan xj380_js_kernel_tan
#define __sin xj380_js_kernel_sin
#define __cos xj380_js_kernel_cos
#define __rem_pio2 xj380_js_rem_pio2
#define __rem_pio2_large xj380_js_rem_pio2_large
#define __math_invalid xj380_js_math_invalid
#define __math_xflow xj380_js_math_xflow
#define __math_oflow xj380_js_math_oflow
#define __math_uflow xj380_js_math_uflow
#define __math_divzero xj380_js_math_divzero
#define __pow_log_data xj380_js_pow_log_data
#define __exp_data xj380_js_exp_data
#define __expo2 xj380_js_expo2
#define __log_data xj380_js_log_data
#define __log2_data xj380_js_log2_data
// The complete upstream private header also declares long-double kernels.
// They are not imported, and must not collide with glibc's public aliases.
#define __sinl xj380_js_unused_kernel_sinl
#define __cosl xj380_js_unused_kernel_cosl
#define __tanl xj380_js_unused_kernel_tanl

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wparentheses"
#if !defined(__clang__)
// The generic upstream reducer also serves other precisions; GCC's sanitizer
// instrumentation loses its loop bounds when analyzing fq[0]. For binary64
// calls nx is 1..3 and prec is 1, so the initialization loop always runs.
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif

// floor and argument reduction have independent file-local constants upstream.
#define toint xj380_js_floor_toint
#include "../../third_party/musl_math/floor.c"
#undef toint
#define toint xj380_js_ceil_toint
#include "../../third_party/musl_math/ceil.c"
#undef toint

#include "../../third_party/musl_math/scalbn.c"
#include "../../third_party/musl_math/fmod.c"

#define toint xj380_js_reduction_toint
#define pio4 xj380_js_reduction_pio4
#include "../../third_party/musl_math/__rem_pio2.c"
#undef pio4
#undef toint
#include "../../third_party/musl_math/__rem_pio2_large.c"

#include "../../third_party/musl_math/__tan.c"
#include "../../third_party/musl_math/__sin.c"
#include "../../third_party/musl_math/__cos.c"
#include "../../third_party/musl_math/tan.c"
#include "../../third_party/musl_math/sin.c"
#include "../../third_party/musl_math/cos.c"

// Use musl's portable non-FMA pow path even when the host compiler enables
// fast FMA: the OS build must not acquire an external fma runtime dependency.
#undef __FP_FAST_FMA
#define __FP_FAST_FMA 0
#include "../../third_party/musl_math/pow_data.c"
#undef N
#undef A
#include "../../third_party/musl_math/exp_data.c"
#undef N
#include "../../third_party/musl_math/__math_invalid.c"
#include "../../third_party/musl_math/__math_xflow.c"
#include "../../third_party/musl_math/__math_oflow.c"
#include "../../third_party/musl_math/__math_uflow.c"
#include "../../third_party/musl_math/__math_divzero.c"
#include "../../third_party/musl_math/pow.c"
#undef T
#undef A
#undef N
#undef Ln2hi
#undef Ln2lo
#undef OFF
#undef InvLn2N
#undef NegLn2hiN
#undef NegLn2loN
#undef Shift
#undef C2
#undef C3
#undef C4
#undef C5
#undef C6
#undef SIGN_BIAS

#include "browser_js_math_extra.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

double xj380_js_sqrt(double value)
{
    // x86_64 guarantees SSE2. SQRTSD implements binary64 rounding and handles
    // signed zero, subnormals, infinities and NaNs without an iteration loop.
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "xm"(value));
    return result;
}

double xj380_js_fabs(double value)
{
    union { double value; uint64_t bits; } number = {value};
    number.bits &= UINT64_C(0x7fffffffffffffff);
    return number.value;
}

double xj380_js_acosh(double value)
{
    // Upstream's magnitude-selected middle branch assumes x >= 1. With large
    // negative x, cancellation in x + sqrt(x*x-1) can otherwise yield a finite
    // logarithm instead of the required domain NaN. Validate before reduction.
    if (value < 1.0) return xj380_js_math_invalid(value);
    return xj380_js_acosh_positive(value);
}
