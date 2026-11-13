#pragma once

#include "../../include/user/language.h"

// zh_MS has its own catalog identity, but uses the standard zh-CN web tag.
inline const char *browser_language_tag(int language)
{
    switch (xj380_language_normalize(language)) {
    case XJ380_LANGUAGE_EN_US: return "en-US";
    case XJ380_LANGUAGE_ZH_TW: return "zh-TW";
    case XJ380_LANGUAGE_JA_JP: return "ja-JP";
    case XJ380_LANGUAGE_FR: return "fr";
    case XJ380_LANGUAGE_DE: return "de";
    default: return "zh-CN";
    }
}

inline const char *browser_language_primary(int language)
{
    switch (xj380_language_normalize(language)) {
    case XJ380_LANGUAGE_EN_US: return "en";
    case XJ380_LANGUAGE_JA_JP: return "ja";
    case XJ380_LANGUAGE_FR: return "fr";
    case XJ380_LANGUAGE_DE: return "de";
    default: return "zh";
    }
}

inline const char *browser_language_region(int language)
{
    switch (xj380_language_normalize(language)) {
    case XJ380_LANGUAGE_EN_US: return "US";
    case XJ380_LANGUAGE_ZH_TW: return "TW";
    case XJ380_LANGUAGE_JA_JP: return "JP";
    case XJ380_LANGUAGE_FR:
    case XJ380_LANGUAGE_DE: return "";
    default: return "CN";
    }
}
