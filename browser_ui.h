#pragma once

#include "browser_platform.h"
#include "../xapi/include/ime_utf8.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

// Browser-owned chrome only. Website colors, layout and drawing stay with the
// document renderer. Rectangles use exclusive right/bottom edges throughout.
namespace browser_ui
{

struct Rect
{
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const
    {
        return !empty() && px >= x && px < right() && py >= y && py < bottom();
    }
    Rect inset(int amount) const
    {
        return {x + amount, y + amount, std::max(0, w - amount * 2), std::max(0, h - amount * 2)};
    }
    Rect intersect(Rect other) const
    {
        const int left = std::max(x, other.x), top = std::max(y, other.y);
        return {left, top, std::max(0, std::min(right(), other.right()) - left),
                std::max(0, std::min(bottom(), other.bottom()) - top)};
    }
    bool operator==(Rect other) const { return x == other.x && y == other.y && w == other.w && h == other.h; }
};

struct Theme
{
    uint32_t toolbar, surface, field, text, secondary, line, hover, accent, on_accent;
    uint32_t warning, warning_text, selection, disabled;

    static Theme light()
    {
        return {0xf8fafdff, 0xffffffff, 0xe9eef6ff, 0x1d2a40ff, 0x56657bff, 0xdce3edff,
                0xdfe7f3ff, 0x255fdbff, 0xffffffff, 0xfff7e7ff, 0x83510aff, 0xcfe0ffff, 0xabb5c4ff};
    }
    static Theme dark()
    {
        return {0x202735ff, 0x141b26ff, 0x141b27ff, 0xe7edf8ff, 0xadb9ceff, 0x354054ff,
                0x303b50ff, 0x91b7ffff, 0x0c2148ff, 0x352b1bff, 0xf3cb8aff, 0x355581ff, 0x637087ff};
    }
};

struct Layout
{
    Rect bounds, toolbar, back, forward, reload, address, address_text, site, search, menu;
    Rect banner, banner_text, banner_action, banner_close;
    Rect find, find_text, find_count, find_prev, find_next, find_close, popup;
    Rect error_title, error_body, error_reload, error_back, error_details;
    int content_top = 0;

    static Layout make(int width, int height, bool banner_visible)
    {
        Layout l;
        l.bounds = {0, 0, std::max(1, width), std::max(1, height)};
        const bool narrow = width < 480;
        const int top = narrow ? 52 : 12;
        l.content_top = narrow ? 96 : 56;
        l.toolbar = {0, 0, width, l.content_top};
        l.back = {narrow ? 8 : 12, top, 32, 32};
        l.forward = {l.back.right() + 4, top, 32, 32};
        l.reload = {l.forward.right() + 4, top, 32, 32};
        l.search = {width - 80, top, 32, 32};
        l.menu = {width - 44, top, 32, 32};
        l.address = narrow ? Rect {8, 8, std::max(0, width - 16), 36}
                           : Rect {124, 10, std::max(0, width - 212), 36};
        // Very small windows may omit secondary controls, but can never send
        // negative coordinates or wrapped unsigned dimensions to XAPI.
        if (width < 220) l.forward = l.search = {};
        if (width < 150) l.back = l.reload = l.menu = {};
        l.site = {l.address.x + 4, l.address.y + 4, 28, 28};
        l.address_text = {l.address.x + 36, l.address.y + 3, std::max(0, l.address.w - 44), 30};
        if (l.address.w < 64) l.site = l.address_text = {};
        l.banner = {0, l.content_top, width, 48};
        l.banner_close = {std::max(0, width - 40), l.banner.y + 8, 32, 32};
        l.banner_action = {std::max(24, width - 152), l.banner.y + 8, 104, 32};
        l.banner_text = {38, l.banner.y + 8, std::max(0, width - 198), 32};
        if (width < 560)
        {
            l.banner.h = 76;
            l.banner_text.w = std::max(0, width - 80);
            l.banner_action = {33, l.banner.y + 40, 104, 30};
        }
        const int find_width = std::max(0, std::min(352, width - 24));
        l.find = {width - find_width - 12, l.content_top + (banner_visible ? l.banner.h : 0) + 12,
                  find_width, 46};
        l.find_close = {l.find.right() - 36, l.find.y + 7, 28, 32};
        l.find_next = {l.find_close.x - 30, l.find.y + 7, 28, 32};
        l.find_prev = {l.find_next.x - 30, l.find.y + 7, 28, 32};
        l.find_count = {l.find_prev.x - 48, l.find.y + 7, 46, 32};
        l.find_text = {l.find.x + 10, l.find.y + 7, std::max(0, l.find_count.x - l.find.x - 16), 32};
        l.popup = {std::max(8, width - 284), l.content_top + 8, std::max(0, std::min(272, width - 16)), 206};
        const int card_width = std::max(0, std::min(560, width - 48));
        const int card_left = std::max(12, (width - card_width) / 2);
        const int card_top = l.content_top + std::max(24, std::min(68, (height - l.content_top) / 9));
        l.error_title = {card_left, card_top + 76, card_width, 64};
        l.error_body = {card_left, card_top + 150, card_width, 100};
        const int button_width = std::max(0, std::min(132, (card_width - 12) / 2));
        l.error_reload = {card_left, card_top + 268, button_width, 38};
        l.error_back = {l.error_reload.right() + 10, l.error_reload.y, button_width, 38};
        l.error_details = {card_left, card_top + 328, card_width, 32};
        return l;
    }
};

enum class Icon
{
    Back, Forward, Reload, Stop, Search, Menu, Site, Globe, Up, Down, Alert, Info, Shield, Arrow, Sun, Moon
};

struct InputView
{
    std::string text;
    int first = 0, caret = 0;
};

inline XTextStyle text_style(unsigned size, unsigned unit = XTEXT_SIZE_NATIVE, const std::string &family = {})
{
    std::string lower = family;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool mono = lower.find("monospace") != std::string::npos || lower.find("courier") != std::string::npos ||
                      lower.find("consolas") != std::string::npos;
    return {mono ? XTEXT_FONT_MONO : XTEXT_FONT_UI, size, unit, XTEXT_ANCHOR_REFERENCE, XTEXT_REFERENCE_AUTO, 0};
}

inline int text_width(const std::string &value, const XTextStyle &style)
{
    if (value.empty()) return 0;
    XTextMetrics measured {};
    return xapi_GetTextMetrics(&style, value.c_str(), &measured) < 0 ? 0 : measured.advance_x;
}

inline int text_width(const std::string &value, unsigned size = 10)
{
    return text_width(value, text_style(size));
}

inline int text_position(const std::string &value, unsigned byte_offset, const XTextStyle &style)
{
    int position = 0;
    XTextMetrics measured {};
    if (xapi_GetTextPosition(&style, value.c_str(), std::min<std::size_t>(byte_offset, value.size()), &position) < 0 ||
        xapi_GetTextMetrics(&style, value.c_str(), &measured) < 0) return 0;
    // Reference labels align their ink left edge. Keep the caret in that same
    // full-run origin, including the kerning before the following character.
    return std::max(0, position - (style.anchor == XTEXT_ANCHOR_REFERENCE ? measured.ink_left : 0));
}

inline int text_position(const std::string &value, unsigned byte_offset, unsigned size = 10)
{
    return text_position(value, byte_offset, text_style(size));
}

inline InputView input_view(const char *text, int length, int cursor, int width, const XTextStyle &style)
{
    InputView result;
    if (width <= 0) return result;
    cursor = std::clamp(cursor, 0, length);
    std::vector<int> boundaries {0};
    boundaries.reserve(length + 1);
    for (int offset = 0; offset < length;)
    {
        offset += std::max(1, static_cast<int>(ime_utf8_scalar_bytes(text + offset, length - offset)));
        boundaries.push_back(offset);
    }
    const int caret_index = static_cast<int>(std::upper_bound(boundaries.begin(), boundaries.end(), cursor) -
                                            boundaries.begin()) - 1;
    cursor = boundaries[std::max(0, caret_index)];
    // Long addresses must not issue thousands of font-width syscalls just
    // to position a caret. Search scalar boundaries, retaining valid UTF-8.
    int first = 0, last = std::max(0, caret_index);
    while (first < last)
    {
        const int middle = (first + last) / 2;
        if (text_width(std::string(text + boundaries[middle], cursor - boundaries[middle]), style) > std::max(0, width - 3))
            first = middle + 1;
        else last = middle;
    }
    result.first = boundaries[first];
    last = static_cast<int>(boundaries.size()) - 1;
    while (first < last)
    {
        const int middle = (first + last + 1) / 2;
        if (text_width(std::string(text + result.first, boundaries[middle] - result.first), style) <= width) first = middle;
        else last = middle - 1;
    }
    const int end = boundaries[first];
    result.text.assign(text + result.first, end - result.first);
    result.caret = text_position(result.text, std::max(0, cursor - result.first), style);
    return result;
}

inline InputView input_view(const char *text, int length, int cursor, int width, unsigned size = 10)
{
    return input_view(text, length, cursor, width, text_style(size));
}

// A single reusable UI raster surface avoids hundreds of drawing syscalls for
// rounded controls and antialiased icons. There are no font/asset dependencies.
class Canvas
{
    struct Text
    {
        int x, y;
        XTextRect clip;
        XTextStyle style;
        uint32_t color;
        std::string value;
    };
    Rect area_;
    std::vector<XCOLORA> pixels_;
    std::vector<Text> texts_;

    void pixel(int x, int y, uint32_t color, float coverage = 1)
    {
        if (!area_.contains(x, y) || coverage <= 0) return;
        auto &p = pixels_[static_cast<size_t>(y - area_.y) * area_.w + x - area_.x];
        if (coverage >= 1 && (color & 255) == 255)
        {
            p = {static_cast<unsigned char>(color >> 24), static_cast<unsigned char>(color >> 16),
                 static_cast<unsigned char>(color >> 8), 255};
            return;
        }
        const float alpha = std::min(1.0f, coverage) * (color & 255) / 255.0f;
        p.Red = static_cast<unsigned char>(p.Red * (1 - alpha) + (color >> 24) * alpha);
        p.Green = static_cast<unsigned char>(p.Green * (1 - alpha) + ((color >> 16) & 255) * alpha);
        p.Blue = static_cast<unsigned char>(p.Blue * (1 - alpha) + ((color >> 8) & 255) * alpha);
        p.Alpha = 255;
    }

public:
    Rect area() const { return area_; }
    const std::vector<XCOLORA> &pixels() const { return pixels_; }
    void begin(Rect area, uint32_t color, const std::vector<XCOLORA> *background = nullptr)
    {
        area_ = area;
        texts_.clear();
        const size_t count = area.empty() ? 0 : static_cast<size_t>(area.w) * area.h;
        pixels_.resize(count);
        if (background && background->size() == count)
            std::copy(background->begin(), background->end(), pixels_.begin());
        else std::fill(pixels_.begin(), pixels_.end(), XCOLORA {static_cast<unsigned char>(color >> 24),
            static_cast<unsigned char>(color >> 16), static_cast<unsigned char>(color >> 8), 255});
    }

    void fill(Rect rect, uint32_t color)
    {
        rect = rect.intersect(area_);
        for (int y = rect.y; y < rect.bottom(); ++y)
            for (int x = rect.x; x < rect.right(); ++x) pixel(x, y, color);
    }

    void round(Rect rect, float radius, uint32_t color)
    {
        if (rect.empty()) return;
        radius = std::min(radius, std::min(rect.w, rect.h) / 2.0f);
        const auto clip = rect.intersect(area_);
        for (int y = clip.y; y < clip.bottom(); ++y)
            for (int x = clip.x; x < clip.right(); ++x)
            {
                const float dx = std::max(std::abs(x + .5f - rect.x - rect.w / 2.0f) - rect.w / 2.0f + radius, 0.0f);
                const float dy = std::max(std::abs(y + .5f - rect.y - rect.h / 2.0f) - rect.h / 2.0f + radius, 0.0f);
                const float coverage = dx > 0 && dy > 0
                    ? std::clamp(radius + .5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f) : 1;
                pixel(x, y, color, coverage);
            }
    }

    void line(float ax, float ay, float bx, float by, uint32_t color, float stroke = 1.6f)
    {
        const float dx = bx - ax, dy = by - ay, length2 = dx * dx + dy * dy;
        const int left = std::max(area_.x, static_cast<int>(std::floor(std::min(ax, bx) - stroke)));
        const int right = std::min(area_.right(), static_cast<int>(std::ceil(std::max(ax, bx) + stroke + 1)));
        const int top = std::max(area_.y, static_cast<int>(std::floor(std::min(ay, by) - stroke)));
        const int bottom = std::min(area_.bottom(), static_cast<int>(std::ceil(std::max(ay, by) + stroke + 1)));
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
            {
                const float t = length2 ? std::clamp(((x + .5f - ax) * dx + (y + .5f - ay) * dy) / length2,
                                                    0.0f, 1.0f) : 0;
                const float px = x + .5f - ax - t * dx, py = y + .5f - ay - t * dy;
                pixel(x, y, color, std::clamp(stroke / 2 + .5f - std::sqrt(px * px + py * py), 0.0f, 1.0f));
            }
    }

    void icon(Icon symbol, Rect rect, uint32_t color, int size = 18)
    {
        const float scale = size / 20.0f;
        const float left = rect.x + (rect.w - size) / 2.0f, top = rect.y + (rect.h - size) / 2.0f;
        auto stroke = [&](float a, float b, float c, float d) {
            line(left + a * scale, top + b * scale, left + c * scale, top + d * scale,
                 color, std::max(1.3f, 1.6f * scale));
        };
        static constexpr float circle[17][2] = {
            {1, 0}, {.92388f, .382683f}, {.707107f, .707107f}, {.382683f, .92388f},
            {0, 1}, {-.382683f, .92388f}, {-.707107f, .707107f}, {-.92388f, .382683f},
            {-1, 0}, {-.92388f, -.382683f}, {-.707107f, -.707107f}, {-.382683f, -.92388f},
            {0, -1}, {.382683f, -.92388f}, {.707107f, -.707107f}, {.92388f, -.382683f}, {1, 0}
        };
        auto ellipse = [&](float cx, float cy, float rx, float ry, int first = 0, int last = 16) {
            for (int i = first; i < last; ++i)
            {
                stroke(cx + circle[i][0] * rx, cy + circle[i][1] * ry,
                       cx + circle[i + 1][0] * rx, cy + circle[i + 1][1] * ry);
            }
        };
        switch (symbol)
        {
        case Icon::Back: stroke(11, 4, 5, 10); stroke(5, 10, 11, 16); stroke(5, 10, 17, 10); break;
        case Icon::Forward: stroke(9, 4, 15, 10); stroke(15, 10, 9, 16); stroke(3, 10, 15, 10); break;
        case Icon::Arrow: stroke(4, 10, 16, 10); stroke(11, 5, 16, 10); stroke(11, 15, 16, 10); break;
        case Icon::Reload:
            ellipse(10, 10, 6.7f, 6.7f, 1, 15); stroke(16.5f, 3, 16.5f, 7.5f); stroke(12, 7.5f, 16.5f, 7.5f); break;
        case Icon::Stop: stroke(5, 5, 15, 15); stroke(5, 15, 15, 5); break;
        case Icon::Search: ellipse(8.5f, 8.5f, 5.5f, 5.5f); stroke(12.5f, 12.5f, 17, 17); break;
        case Icon::Up: stroke(5, 12, 10, 7); stroke(10, 7, 15, 12); break;
        case Icon::Down: stroke(5, 8, 10, 13); stroke(10, 13, 15, 8); break;
        case Icon::Menu:
            for (int x : {4, 10, 16}) { stroke(x, 10, x + .1f, 10); } break;
        case Icon::Site:
            stroke(3, 5, 17, 5); stroke(3, 10, 17, 10); stroke(3, 15, 17, 15);
            stroke(7, 3, 7, 7); stroke(13, 8, 13, 12); stroke(8, 13, 8, 17); break;
        case Icon::Globe:
            ellipse(10, 10, 8, 8); ellipse(10, 10, 3.5f, 8); stroke(2, 10, 18, 10); break;
        case Icon::Shield:
            stroke(10, 2, 17, 5); stroke(17, 5, 16, 13); stroke(16, 13, 10, 18);
            stroke(10, 18, 4, 13); stroke(4, 13, 3, 5); stroke(3, 5, 10, 2);
            stroke(6.5f, 9, 9, 12); stroke(9, 12, 14, 7); break;
        case Icon::Alert: case Icon::Info:
            ellipse(10, 10, 8, 8); stroke(10, 5.5f, 10, 11); stroke(10, 14, 10.1f, 14); break;
        case Icon::Sun:
            ellipse(10, 10, 3.5f, 3.5f);
            for (int i = 0; i < 8; ++i)
            {
                stroke(10 + circle[i * 2][0] * 6, 10 + circle[i * 2][1] * 6,
                       10 + circle[i * 2][0] * 8, 10 + circle[i * 2][1] * 8);
            }
            break;
        case Icon::Moon:
            ellipse(10, 10, 7, 7, .2f, 4.5f); stroke(8.5f, 3.2f, 7, 8); stroke(7, 8, 11, 13);
            stroke(11, 13, 16.8f, 11.5f); break;
        }
    }

    void text_with_style(Rect rect, const std::string &value, uint32_t color, const XTextStyle &style)
    {
        const Rect layout = rect;
        rect = rect.intersect(area_);
        if (rect.empty() || value.empty()) return;
        XTextMetrics measured {};
        if (xapi_GetTextMetrics(&style, nullptr, &measured) < 0) return;
        const int reference_height = measured.reference_bottom - measured.reference_top;
        std::string clipped = value;
        while (!clipped.empty() && text_width(clipped, style) > layout.w)
            clipped.resize(ime_utf8_previous(clipped.c_str(), clipped.size()));
        if (!clipped.empty())
            texts_.push_back({layout.x, layout.y + (layout.h - reference_height) / 2,
                              {rect.x, rect.y, rect.w, rect.h}, style, color, std::move(clipped)});
    }

    void text(Rect rect, const std::string &value, uint32_t color, unsigned size = 10)
    {
        text_with_style(rect, value, color, text_style(size));
    }

    void present(HDLE window)
    {
        if (area_.empty()) return;
        xapi_WriteBufferA(window, area_.x, area_.y, area_.w, area_.h, pixels_.data());
        for (const auto &text : texts_)
        {
            xapi_DrawTextEx(window, text.x, text.y, text.value.c_str(), &text.style, text.color, &text.clip);
        }
    }
};

struct Backdrop
{
    Rect rect;
    std::vector<XCOLORA> pixels;
    bool valid = false;
    void reset() { valid = false; }
    void restore(HDLE window)
    {
        if (valid && !rect.empty()) xapi_WriteBufferA(window, rect.x, rect.y, rect.w, rect.h, pixels.data());
        reset();
    }
    void capture(HDLE window, Rect next, bool repaint)
    {
        if (valid && rect == next && !repaint) return;
        rect = next;
        pixels.resize(next.empty() ? 0 : static_cast<size_t>(next.w) * next.h);
        if (!next.empty()) xapi_ReadBufferA(window, next.x, next.y, next.w, next.h, pixels.data());
        valid = true;
    }
};

} // namespace browser_ui
