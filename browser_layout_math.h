#pragma once

// Force-include only in litehtml's background geometry translation unit. Read
// hosted declarations first, then redirect its calls to the browser's bounded
// binary64 implementation without changing the shared XAPI math ABI.
#include <cmath>
#include "browser_js_math.h"

inline float xj380_layout_fmodf(float value, float divisor)
{
    return static_cast<float>(xj380_js_fmod(value, divisor));
}

inline float xj380_layout_sqrtf(float value)
{
    return static_cast<float>(xj380_js_sqrt(value));
}

#define tan xj380_js_tan
#define atan2 xj380_js_atan2
#define sqrt xj380_js_sqrt
#define fmodf xj380_layout_fmodf
#define sqrtf xj380_layout_sqrtf
