#pragma once

// Force-include only in the QuickJS core. Read the system declarations before
// renaming calls: command-line -Dtan=... also rewrites libc SIMD declarations.
#include <math.h>
#include "browser_js_math.h"

#define tan xj380_js_tan
#define sin xj380_js_sin
#define cos xj380_js_cos
#define fmod xj380_js_fmod
#define floor xj380_js_floor
#define fabs xj380_js_fabs
#define ceil xj380_js_ceil
#define sqrt xj380_js_sqrt
#define pow xj380_js_pow
#define scalbn xj380_js_scalbn
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
