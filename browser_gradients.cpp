#include "browser_gradients.h"
#include "browser_js_math.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <vector>

namespace
{
constexpr std::size_t kMaxPixels = 4 * 1024 * 1024;
constexpr std::size_t kMaxStops = 4096;
constexpr int kMaxDimension = 16384;
constexpr double kPi = 3.14159265358979323846;

bool finite_value(double value) { return value == value && value >= -1e12 && value <= 1e12; }

double clamp(double value, double low = 0, double high = 1) { return std::clamp(value, low, high); }

bool valid_rect(const litehtml::position &value)
{
    return finite_value(value.x) && finite_value(value.y) && finite_value(value.width) && finite_value(value.height) &&
           value.width > 0 && value.height > 0 && finite_value(value.right()) && finite_value(value.bottom());
}

struct Color
{
    double x = 0, y = 0, z = 0, alpha = 0;
};

double srgb_decode(double value)
{
    return value <= 0.04045 ? value / 12.92 : xj380_js_pow((value + 0.055) / 1.055, 2.4);
}

double srgb_encode(double value)
{
    // A shared transfer table avoids three libm calls per output pixel. Linear
    // interpolation is substantially finer than one 8-bit channel increment.
    static const std::array<double, 4097> table = [] {
        std::array<double, 4097> values {};
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            const double v = static_cast<double>(i) / 4096;
            values[i] = v <= 0.0031308 ? v * 12.92 : 1.055 * xj380_js_pow(v, 1.0 / 2.4) - 0.055;
        }
        return values;
    }();
    const double scaled = clamp(value) * 4096;
    const auto index = static_cast<unsigned>(scaled);
    return index == 4096 ? table[4096] : table[index] + (table[index + 1] - table[index]) * (scaled - index);
}

bool supported(litehtml::color_space_t space)
{
    return space == litehtml::color_space_none || space == litehtml::color_space_srgb ||
           space == litehtml::color_space_srgb_linear || space == litehtml::color_space_oklab;
}

Color convert(litehtml::web_color value, litehtml::color_space_t space)
{
    Color result {value.red / 255.0, value.green / 255.0, value.blue / 255.0, value.alpha / 255.0};
    if (space == litehtml::color_space_srgb_linear || space == litehtml::color_space_oklab)
    {
        result.x = srgb_decode(result.x); result.y = srgb_decode(result.y); result.z = srgb_decode(result.z);
    }
    if (space == litehtml::color_space_oklab)
    {
        const double l = xj380_js_cbrt(0.4122214708 * result.x + 0.5363325363 * result.y + 0.0514459929 * result.z);
        const double m = xj380_js_cbrt(0.2119034982 * result.x + 0.6806995451 * result.y + 0.1073969566 * result.z);
        const double s = xj380_js_cbrt(0.0883024619 * result.x + 0.2817188376 * result.y + 0.6299787005 * result.z);
        result.x = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
        result.y = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
        result.z = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    }
    result.x *= result.alpha; result.y *= result.alpha; result.z *= result.alpha;
    return result;
}

unsigned char channel(double value)
{
    return static_cast<unsigned char>(clamp(value) * 255 + 0.5);
}

XCOLORA pixel(Color value, litehtml::color_space_t space, double coverage)
{
    if (value.alpha <= 0 || coverage <= 0) return {0, 0, 0, 0};
    value.x /= value.alpha; value.y /= value.alpha; value.z /= value.alpha;
    if (space == litehtml::color_space_oklab)
    {
        double l = value.x + 0.3963377774 * value.y + 0.2158037573 * value.z;
        double m = value.x - 0.1055613458 * value.y - 0.0638541728 * value.z;
        double s = value.x - 0.0894841775 * value.y - 1.2914855480 * value.z;
        l = l * l * l; m = m * m * m; s = s * s * s;
        value.x = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
        value.y = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
        value.z = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    }
    if (space == litehtml::color_space_srgb_linear || space == litehtml::color_space_oklab)
    { value.x = srgb_encode(value.x); value.y = srgb_encode(value.y); value.z = srgb_encode(value.z); }
    return {channel(value.x), channel(value.y), channel(value.z), channel(value.alpha * coverage)};
}

struct Stop
{
    double offset = 0, hint_exponent = 1;
    Color color;
};

struct Stops
{
    std::vector<Stop> values;
    litehtml::color_space_t space;
    bool repeating = false, averaged = false;
    double period = 0;
    Color average;

    bool prepare(const litehtml::background_layer::gradient_base &gradient, double physical_scale)
    {
        space = gradient.color_space;
        if (!supported(space) || gradient.color_points.empty() || gradient.color_points.size() > kMaxStops) return false;
        values.reserve(gradient.color_points.size());
        for (const auto &point : gradient.color_points)
        {
            if (!finite_value(point.offset)) return false;
            const double offset = values.empty() ? point.offset : std::max<double>(values.back().offset, point.offset);
            values.push_back({offset, 1, convert(point.color, space)});
        }
        for (std::size_t i = 0; i + 1 < values.size(); ++i)
        {
            const auto &hint = gradient.color_points[i].hint;
            if (!hint || values[i + 1].offset <= values[i].offset) continue;
            if (!finite_value(*hint)) return false;
            const double ratio = (*hint - values[i].offset) / (values[i + 1].offset - values[i].offset);
            values[i].hint_exponent = ratio <= 0 ? 0 : ratio >= 1 ? -1 : xj380_js_log(0.5) / xj380_js_log(ratio);
        }
        repeating = gradient.repeating;
        period = values.back().offset - values.front().offset;
        averaged = repeating && (period <= 0 || period * physical_scale < 1);
        if (averaged)
        {
            // CSS Images 3 defines unresolved repeating gradients as a weighted
            // average in premultiplied sRGBA. A zero-length period first gets
            // equally spaced stops, preserving endpoint half weights.
            space = litehtml::color_space_srgb;
            if (values.size() == 1) average = convert(gradient.color_points.front().color, space);
            for (std::size_t i = 0; i + 1 < values.size(); ++i)
            {
                const double weight = period > 0 ? (values[i + 1].offset - values[i].offset) / (2 * period) :
                                                  0.5 / (values.size() - 1);
                const auto left = convert(gradient.color_points[i].color, space);
                const auto right = convert(gradient.color_points[i + 1].color, space);
                average.x += (left.x + right.x) * weight;
                average.y += (left.y + right.y) * weight;
                average.z += (left.z + right.z) * weight;
                average.alpha += (left.alpha + right.alpha) * weight;
            }
        }
        return true;
    }

    Color sample(double offset) const
    {
        if (averaged) return average;
        if (repeating)
        {
            const double relative = offset - values.front().offset;
            offset = values.front().offset + relative - xj380_js_floor(relative / period) * period;
            // Cancellation at large coordinates can reach the upper endpoint;
            // the beginning of the next period wins at a repeat boundary.
            if (offset >= values.back().offset || offset < values.front().offset) offset = values.front().offset;
        }
        const auto right = std::upper_bound(values.begin(), values.end(), offset,
            [](double position, const Stop &stop) { return position < stop.offset; });
        if (right == values.begin()) return right->color;
        if (right == values.end()) return values.back().color;
        const auto &left = *(right - 1);
        double amount = clamp((offset - left.offset) / (right->offset - left.offset));
        if (left.hint_exponent == 0) amount = amount > 0 ? 1 : 0;
        else if (left.hint_exponent < 0) amount = 0;
        else if (left.hint_exponent != 1) amount = xj380_js_pow(amount, left.hint_exponent);
        const double inverse = 1 - amount;
        return {left.color.x * inverse + right->color.x * amount,
                left.color.y * inverse + right->color.y * amount,
                left.color.z * inverse + right->color.z * amount,
                left.color.alpha * inverse + right->color.alpha * amount};
    }
};

struct Corner
{
    double x, y, radius_x, radius_y;
};

struct Clip
{
    litehtml::position box;
    std::array<Corner, 4> corners;
    bool rounded = false;

    bool prepare(const litehtml::background_layer &layer, const litehtml::position &extra)
    {
        if (!valid_rect(layer.border_box) || !valid_rect(layer.clip_box) || !valid_rect(extra)) return false;
        box = layer.border_box.intersect(layer.clip_box).intersect(extra);
        if (!valid_rect(box)) return false;
        const auto &b = layer.border_box;
        const auto &r = layer.border_radius;
        const double raw[] = {r.top_left_x, r.top_left_y, r.top_right_x, r.top_right_y,
                              r.bottom_right_x, r.bottom_right_y, r.bottom_left_x, r.bottom_left_y};
        for (double value : raw) if (!finite_value(value)) return false;
        double radius[8];
        for (unsigned i = 0; i < 8; ++i) radius[i] = std::max(0.0, raw[i]);
        double scale = 1;
        const double pairs[] = {radius[0] + radius[2], radius[6] + radius[4],
                                radius[1] + radius[7], radius[3] + radius[5]};
        for (unsigned i = 0; i < 4; ++i)
            if (pairs[i] > 0) scale = std::min(scale, (i < 2 ? b.width : b.height) / pairs[i]);
        for (double &value : radius) { value *= scale; if (value > 0) rounded = true; }
        corners = {{{b.left() + radius[0], b.top() + radius[1], radius[0], radius[1]},
                    {b.right() - radius[2], b.top() + radius[3], radius[2], radius[3]},
                    {b.right() - radius[4], b.bottom() - radius[5], radius[4], radius[5]},
                    {b.left() + radius[6], b.bottom() - radius[7], radius[6], radius[7]}}};
        return true;
    }

    bool contains(double x, double y) const
    {
        if (x < box.left() || x >= box.right() || y < box.top() || y >= box.bottom()) return false;
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto &corner = corners[i];
            if (corner.radius_x <= 0 || corner.radius_y <= 0) continue;
            const bool side_x = (i == 0 || i == 3) ? x < corner.x : x > corner.x;
            const bool side_y = i < 2 ? y < corner.y : y > corner.y;
            if (side_x && side_y)
            {
                const double dx = (x - corner.x) / corner.radius_x, dy = (y - corner.y) / corner.radius_y;
                if (dx * dx + dy * dy > 1) return false;
            }
        }
        return true;
    }

    double coverage(int x, int y) const
    {
        if (!rounded) return contains(x + 0.5, y + 0.5) ? 1 : 0;
        // A rounded rectangle is convex. If the four extreme sample points are
        // inside, the whole sampling grid is inside as well; only edge pixels
        // need the more expensive sixteen-sample coverage calculation.
        if (contains(x + 0.125, y + 0.125) && contains(x + 0.875, y + 0.125) &&
            contains(x + 0.125, y + 0.875) && contains(x + 0.875, y + 0.875))
        {
            return 1;
        }
        unsigned inside = 0;
        for (unsigned sy = 0; sy < 4; ++sy)
            for (unsigned sx = 0; sx < 4; ++sx)
                if (contains(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4)) ++inside;
        return inside / 16.0;
    }
};

template<class Offset>
bool raster(const litehtml::background_layer &layer, const litehtml::position &extra,
            const litehtml::background_layer::gradient_base &gradient, double physical_scale,
            BrowserGradientRaster &out, Offset offset)
{
    out = {};
    if (!valid_rect(layer.origin_box)) return false;
    try
    {
        Stops stops;
        Clip clip;
        if (!stops.prepare(gradient, physical_scale) || !clip.prepare(layer, extra)) return false;
        const double left_value = xj380_js_floor(clip.box.left()), top_value = xj380_js_floor(clip.box.top());
        const double right_value = xj380_js_ceil(clip.box.right()), bottom_value = xj380_js_ceil(clip.box.bottom());
        if (left_value < -100000000 || top_value < -100000000 || right_value > 100000000 || bottom_value > 100000000)
            return false;
        const int left = static_cast<int>(left_value), top = static_cast<int>(top_value);
        const int width = static_cast<int>(right_value - left_value), height = static_cast<int>(bottom_value - top_value);
        if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension ||
            static_cast<std::size_t>(width) > kMaxPixels / static_cast<std::size_t>(height)) return false;
        out.bounds = {static_cast<float>(left), static_cast<float>(top), static_cast<float>(width), static_cast<float>(height)};
        out.image.width = width; out.image.height = height;
        out.image.pixels.resize(static_cast<std::size_t>(width) * height);
        const bool repeat_x = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_x;
        const bool repeat_y = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_y;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int pixel_x = left + x, pixel_y = top + y;
                const double coverage = clip.coverage(pixel_x, pixel_y);
                auto &destination = out.image.pixels[static_cast<std::size_t>(y) * width + x];
                double sx = pixel_x + 0.5, sy = pixel_y + 0.5;
                if (!coverage || (!repeat_x && (sx < layer.origin_box.left() || sx >= layer.origin_box.right())) ||
                    (!repeat_y && (sy < layer.origin_box.top() || sy >= layer.origin_box.bottom())))
                { destination = {0, 0, 0, 0}; continue; }
                // Map the sample into the original tile without moving the
                // gradient endpoints. This also handles tiles with a negative
                // origin and ensures scrolling/clipping cannot restart stops.
                if (repeat_x) sx -= xj380_js_floor((sx - layer.origin_box.x) / layer.origin_box.width) * layer.origin_box.width;
                if (repeat_y) sy -= xj380_js_floor((sy - layer.origin_box.y) / layer.origin_box.height) * layer.origin_box.height;
                destination = pixel(stops.sample(offset(sx, sy)), stops.space, coverage);
            }
        }
        return true;
    }
    catch (const std::bad_alloc &) { out = {}; return false; }
}
} // namespace

bool browser_raster_linear_gradient(const litehtml::background_layer &layer, const litehtml::position &clip,
                                    const litehtml::background_layer::linear_gradient &gradient,
                                    BrowserGradientRaster &out)
{
    const double dx = static_cast<double>(gradient.end.x) - gradient.start.x;
    const double dy = static_cast<double>(gradient.end.y) - gradient.start.y;
    const double length = dx * dx + dy * dy;
    if (!finite_value(gradient.start.x) || !finite_value(gradient.start.y) || !finite_value(gradient.end.x) || !finite_value(gradient.end.y) ||
        !finite_value(length)) { out = {}; return false; }
    return raster(layer, clip, gradient, xj380_js_sqrt(length), out, [&](double x, double y) {
        return length > 0 ? ((x - gradient.start.x) * dx + (y - gradient.start.y) * dy) / length : 1e12;
    });
}

bool browser_raster_radial_gradient(const litehtml::background_layer &layer, const litehtml::position &clip,
                                    const litehtml::background_layer::radial_gradient &gradient,
                                    BrowserGradientRaster &out)
{
    if (!finite_value(gradient.position.x) || !finite_value(gradient.position.y) || !finite_value(gradient.radius.x) ||
        !finite_value(gradient.radius.y) || gradient.radius.x < 0 || gradient.radius.y < 0) { out = {}; return false; }
    return raster(layer, clip, gradient, std::min(gradient.radius.x, gradient.radius.y), out, [&](double x, double y) {
        if (!gradient.radius.x || !gradient.radius.y) return 1e12;
        const double dx = (x - gradient.position.x) / gradient.radius.x;
        const double dy = (y - gradient.position.y) / gradient.radius.y;
        return xj380_js_sqrt(dx * dx + dy * dy);
    });
}

bool browser_raster_conic_gradient(const litehtml::background_layer &layer, const litehtml::position &clip,
                                   const litehtml::background_layer::conic_gradient &gradient,
                                   BrowserGradientRaster &out)
{
    if (!finite_value(gradient.position.x) || !finite_value(gradient.position.y) || !finite_value(gradient.angle)) { out = {}; return false; }
    const double dx = std::max<double>(gradient.position.x - layer.origin_box.left(),
                                      layer.origin_box.right() - gradient.position.x);
    const double dy = std::max<double>(gradient.position.y - layer.origin_box.top(),
                                      layer.origin_box.bottom() - gradient.position.y);
    return raster(layer, clip, gradient, 2 * kPi * xj380_js_sqrt(dx * dx + dy * dy), out, [&](double x, double y) {
        const double turn = xj380_js_atan2(x - gradient.position.x, gradient.position.y - y) / (2 * kPi) - gradient.angle / 360;
        return turn - xj380_js_floor(turn);
    });
}
