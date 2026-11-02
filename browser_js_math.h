#pragma once

// Binary64 browser math has a separate namespace from the freestanding runtime.
// All entry points and their reduction helpers use the native C double ABI.
#ifdef __cplusplus
extern "C" {
#endif

double xj380_js_tan(double value);
double xj380_js_sin(double value);
double xj380_js_cos(double value);
double xj380_js_fmod(double value, double divisor);
double xj380_js_floor(double value);
double xj380_js_fabs(double value);
double xj380_js_ceil(double value);
double xj380_js_sqrt(double value);
double xj380_js_pow(double value, double exponent);
double xj380_js_scalbn(double value, int exponent);
double xj380_js_acos(double value);
double xj380_js_acosh(double value);
double xj380_js_asin(double value);
double xj380_js_asinh(double value);
double xj380_js_atan(double value);
double xj380_js_atan2(double y, double x);
double xj380_js_atanh(double value);
double xj380_js_cbrt(double value);
double xj380_js_cosh(double value);
double xj380_js_exp(double value);
double xj380_js_expm1(double value);
double xj380_js_hypot(double x, double y);
double xj380_js_log(double value);
double xj380_js_log10(double value);
double xj380_js_log1p(double value);
double xj380_js_log2(double value);
double xj380_js_sinh(double value);
double xj380_js_tanh(double value);
double xj380_js_trunc(double value);
double xj380_js_round(double value);

#ifdef __cplusplus
}
#endif
