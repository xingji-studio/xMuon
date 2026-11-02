#include "browser_inline_svg.h"
#include <litehtml/document_container.h>
#include <litehtml/render_item.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace
{
constexpr std::size_t kMaxSourceBytes = 128 * 1024;
constexpr std::size_t kMaxSvgNodes = 4096;
constexpr unsigned kMaxRasterDimension = 4096;
constexpr unsigned kMaxRasterPixels = 4 * 1024 * 1024;

bool space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ',';
}

bool number(const char *&cursor, float &result)
{
    while (space(*cursor)) ++cursor;
    bool negative = false;
    if (*cursor == '-' || *cursor == '+') negative = *cursor++ == '-';
    bool digits = false;
    double value = 0, fraction = 0.1;
    while (*cursor >= '0' && *cursor <= '9')
    {
        digits = true;
        value = value * 10 + (*cursor++ - '0');
        if (value > 1000000000000.0) return false;
    }
    if (*cursor == '.')
    {
        ++cursor;
        while (*cursor >= '0' && *cursor <= '9')
        {
            digits = true;
            value += (*cursor++ - '0') * fraction;
            fraction *= 0.1;
        }
    }
    if (!digits) return false;
    if (*cursor == 'e' || *cursor == 'E')
    {
        ++cursor;
        bool minus = false;
        if (*cursor == '-' || *cursor == '+') minus = *cursor++ == '-';
        unsigned exponent = 0;
        bool exponent_digits = false;
        while (*cursor >= '0' && *cursor <= '9')
        {
            exponent_digits = true;
            exponent = exponent * 10 + (*cursor++ - '0');
            if (exponent > 12) return false;
        }
        if (!exponent_digits) return false;
        while (exponent--) value = minus ? value / 10 : value * 10;
    }
    if (negative) value = -value;
    if (value < -1000000000000.0 || value > 1000000000000.0) return false;
    result = static_cast<float>(value);
    return true;
}

bool view_box(const litehtml::element &element, float &width, float &height)
{
    const char *text = element.get_attr("viewbox");
    if (!text || std::char_traits<char>::length(text) > 256) return false;
    float x, y;
    if (!number(text, x) || !number(text, y) || !number(text, width) || !number(text, height)) return false;
    while (space(*text)) ++text;
    return !*text && width > 0 && height > 0;
}

float dimension(const litehtml::element &element, const char *attribute)
{
    const char *text = element.get_attr(attribute);
    if (!text || std::char_traits<char>::length(text) > 128) return 0;
    float value;
    if (!number(text, value) || value <= 0) return 0;
    while (space(*text)) ++text;
    if (!*text || std::strcmp(text, "px") == 0) return value;
    if (std::strcmp(text, "in") == 0) return value * 96;
    if (std::strcmp(text, "cm") == 0) return value * (96.0f / 2.54f);
    if (std::strcmp(text, "mm") == 0) return value * (96.0f / 25.4f);
    if (std::strcmp(text, "pt") == 0) return value * (96.0f / 72.0f);
    if (std::strcmp(text, "pc") == 0) return value * 16;
    return 0; // Percentages have no intrinsic dimension.
}

litehtml::size intrinsic(const litehtml::element &element)
{
    float width = dimension(element, "width"), height = dimension(element, "height");
    float box_width = 0, box_height = 0;
    const bool ratio = view_box(element, box_width, box_height);
    if (width > 0 && height <= 0) height = ratio ? width * box_height / box_width : 150;
    else if (height > 0 && width <= 0) width = ratio ? height * box_width / box_height : 300;
    else if (width <= 0 && height <= 0)
    {
        width = 300; height = 150;
        if (ratio)
        {
            if (box_width / box_height > 2) height = 300 * box_height / box_width;
            else width = 150 * box_width / box_height;
        }
    }
    // Layout can still be constrained by CSS; avoid infinities from extreme ratios.
    return {std::clamp(width, 0.01f, 1000000.0f), std::clamp(height, 0.01f, 1000000.0f)};
}

const char *svg_tag(const char *tag)
{
    for (const char *name : {"svg", "g", "defs", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon", "stop"})
        if (std::strcmp(tag, name) == 0) return name;
    if (std::strcmp(tag, "lineargradient") == 0) return "linearGradient";
    if (std::strcmp(tag, "radialgradient") == 0) return "radialGradient";
    return nullptr;
}

const char *attribute_name(const std::string &name)
{
    if (name == "viewbox") return "viewBox";
    if (name == "preserveaspectratio") return "preserveAspectRatio";
    if (name == "gradientunits") return "gradientUnits";
    if (name == "gradienttransform") return "gradientTransform";
    if (name == "spreadmethod") return "spreadMethod";
    return name.c_str();
}

bool xml_name(const std::string &name)
{
    if (name.empty()) return false;
    for (const unsigned char ch : name)
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == ':' || ch == '.')) return false;
    return true;
}

std::string length_text(float value)
{
    // Intrinsic dimensions are finite and clamped. Format with integer
    // arguments so the SVG always uses a decimal point, independent of locale.
    unsigned whole = static_cast<unsigned>(value);
    unsigned fraction = static_cast<unsigned>((value - whole) * 1000000 + 0.5f);
    if (fraction >= 1000000) { ++whole; fraction = 0; }
    if (!fraction) return std::to_string(whole);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%06u", whole, fraction);
    std::string result(buffer);
    while (result.back() == '0') result.pop_back();
    return result;
}

struct Source
{
    std::string value;
    bool append(const std::string &text)
    {
        if (text.size() > kMaxSourceBytes - value.size()) return false;
        value += text;
        return true;
    }
    bool quoted(const std::string &text)
    {
        for (char ch : text)
        {
            const char *entity = ch == '&' ? "&amp;" : ch == '<' ? "&lt;" : ch == '>' ? "&gt;" :
                                 ch == '"' ? "&quot;" : nullptr;
            if (entity) { if (!append(entity)) return false; }
            else
            {
                if (value.size() == kMaxSourceBytes) return false;
                if (static_cast<unsigned char>(ch) >= 0x20 || ch == '\n' || ch == '\r' || ch == '\t') value += ch;
            }
        }
        return true;
    }
};

std::string color_value(const litehtml::element &element, std::string text)
{
    // NanoSVG accepts RGB paint colors. SVG fill/stroke-opacity remains intact;
    // the alpha component of a CSS currentColor is not yet translated here.
    char color[8];
    const auto value = element.css().get_color();
    std::snprintf(color, sizeof(color), "#%02x%02x%02x", value.red, value.green, value.blue);
    std::string lowered(text);
    for (char &ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::size_t offset = 0;
    while ((offset = lowered.find("currentcolor", offset)) != std::string::npos)
    {
        text.replace(offset, 12, color);
        lowered.replace(offset, 12, color);
        offset += 7;
    }
    return text;
}

bool serialize(Source &output, const litehtml::element &element, unsigned width, unsigned height,
               std::size_t &nodes, unsigned depth, bool root)
{
    if (++nodes > kMaxSvgNodes || depth > 128) return false;
    const char *tag = svg_tag(element.get_tagName());
    if (!tag) return true; // HTML, scripts and external-image subtrees are not vector paint.
    if (!output.append("<") || !output.append(tag)) return false;
    for (const auto &attribute : element.attributes())
    {
        const auto &name = attribute.first;
        if (!xml_name(name) || name.compare(0, 2, "on") == 0 || name == "src") continue;
        if (root && (name == "width" || name == "height")) continue;
        if ((name == "href" || name == "xlink:href") && (attribute.second.empty() || attribute.second[0] != '#')) continue;
        const bool color = name == "fill" || name == "stroke" || name == "color" || name == "stop-color" || name == "style";
        if (!output.append(" ") || !output.append(attribute_name(name)) || !output.append("=\"") ||
            !output.quoted(color ? color_value(element, attribute.second) : attribute.second) || !output.append("\"")) return false;
    }
    if (root)
    {
        if (!element.get_attr("xmlns") && !output.append(" xmlns=\"http://www.w3.org/2000/svg\"")) return false;
        if (!output.append(" width=\"") || !output.append(std::to_string(width)) || !output.append("\" height=\"") ||
            !output.append(std::to_string(height)) || !output.append("\"")) return false;
        if (!element.get_attr("viewbox"))
        {
            const auto size = intrinsic(element);
            if (!output.append(" viewBox=\"0 0 ") || !output.append(length_text(size.width)) || !output.append(" ") ||
                !output.append(length_text(size.height)) || !output.append("\"")) return false;
        }
    }
    if (!output.append(">")) return false;
    for (const auto &child : element.children())
        if (!serialize(output, *child, width, height, nodes, depth + 1, false)) return false;
    return output.append("</") && output.append(tag) && output.append(">");
}

void vector_styles(const litehtml::element::ptr &node, std::size_t &nodes, unsigned depth)
{
    if (++nodes > kMaxSvgNodes || depth > 128) return;
    if (!svg_tag(node->get_tagName())) return;
    node->compute_styles(false);
    for (const auto &child : node->children()) vector_styles(child, nodes, depth + 1);
}

std::string data_uri(const std::string &source)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string result = "data:image/svg+xml,";
    result.reserve(result.size() + source.size() * 3);
    for (const unsigned char ch : source)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '.' || ch == '_' || ch == '~') result += static_cast<char>(ch);
        else { result += '%'; result += hex[ch >> 4]; result += hex[ch & 15]; }
    }
    return result;
}
} // namespace

BrowserInlineSvgElement::BrowserInlineSvgElement(const litehtml::document::ptr &document) : litehtml::el_image(document) {}

void BrowserInlineSvgElement::compute_styles(bool recursive)
{
    litehtml::html_tag::compute_styles(false);
    // The subtree remains available to DOM/CSS, but unsupported foreign HTML
    // must not start image fetches or become a second HTML rendering subtree.
    std::size_t nodes = 1;
    if (recursive) for (const auto &child : children()) vector_styles(child, nodes, 1);
}

void BrowserInlineSvgElement::get_content_size(litehtml::size &size, litehtml::pixel_t)
{
    size = intrinsic(*this);
}

std::string BrowserInlineSvgElement::source_for_size(unsigned width, unsigned height) const
{
    if (!width || !height || width > kMaxRasterDimension || height > kMaxRasterDimension ||
        width > kMaxRasterPixels / height) return {};
    try
    {
        Source output;
        std::size_t nodes = 0;
        if (!serialize(output, *this, width, height, nodes, 0, true)) return {};
        return std::move(output.value);
    }
    catch (const std::bad_alloc &) { return {}; }
}

void BrowserInlineSvgElement::draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
                                  const litehtml::position *clip, const std::shared_ptr<litehtml::render_item> &item)
{
    litehtml::html_tag::draw(hdc, x, y, clip, item);
    auto position = item->pos();
    position.x += x; position.y += y;
    position.round();
    if (position.width <= 0 || position.height <= 0 || position.width > kMaxRasterDimension ||
        position.height > kMaxRasterDimension || !position.does_intersect(clip)) return;
    try
    {
        const auto source = source_for_size(static_cast<unsigned>(position.width), static_cast<unsigned>(position.height));
        if (source.empty()) return;
        litehtml::background_layer layer;
        layer.clip_box = clip ? position.intersect(*clip) : position;
        layer.origin_box = layer.border_box = position;
        layer.repeat = litehtml::background_repeat_no_repeat;
        get_document()->container()->draw_image(hdc, layer, data_uri(source), {});
    }
    catch (const std::bad_alloc &) { /* Keep the page usable if an icon cannot be rasterized. */ }
}
