#include "browser_fetch.h"
#include "browser_resources.h"
#include "browser_ui.h"
#include "browser_http_bridge.h"
#include "browser_image_decode.h"
#include "browser_inline_svg.h"
#include "browser_gradients.h"
#include "browser_platform.h"
#include "browser_events.h"
#include "browser_parser_memory.h"
#include "browser_html_scripts.h"
#include "browser_url.h"
#include "browser_forms.h"
#include "browser_js.h"
#include <litehtml.h>
#include <litehtml/url.h>
#include <litehtml/render_image.h>
#include "../../include/xapi_json.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>
#include "../xapi/include/ime_utf8.h"
#include "../xapi/include/xime.h"
#include "../../include/user/settings.h"
#include "../xapi/include/xclipboard.h"

namespace
{

constexpr int BROWSER_WIDTH = 1024;
constexpr int BROWSER_HEIGHT = 720;
constexpr int BROWSER_LOOP_SLEEP_MS = 16;
constexpr int BROWSER_SCROLLBAR_WIDTH = 14;
constexpr int BROWSER_SCROLLBAR_MIN_THUMB = 24;
constexpr int BROWSER_SCROLL_STEP = 48;
constexpr int BROWSER_TEXT_LINE_HEIGHT = 20;
constexpr unsigned char BROWSER_KEY_F3 = 139;
constexpr unsigned char BROWSER_KEY_SHIFT           = 133;
constexpr unsigned char BROWSER_KEY_HOME            = 151;
constexpr unsigned char BROWSER_KEY_LEFT            = 154;
constexpr unsigned char BROWSER_KEY_RIGHT           = 155;
constexpr unsigned char BROWSER_KEY_END             = 156;
constexpr bool BROWSER_ENABLE_LITEHTML = true;
constexpr const char *BROWSER_USER_CSS =
    "table{font-size:12px;line-height:16px;border-collapse:collapse;}"
    "td,th{font-size:12px;line-height:16px;padding:2px 4px;}"
    "noscript{display:none!important;}";

constexpr std::uint32_t COLOR_PANEL = 0xfafaf9ff;
constexpr std::uint32_t COLOR_TEXT = 0x1f2933ff;
constexpr std::uint32_t COLOR_LINK = 0x1976d2ff;

struct BrowserUserInfo
{
    char name[64];
    int  user_type;
};

extern "C" {
int           open(const char *path, int flags, ...);
void          xj380_reset_permission_prompts(void);
void          xapi_GetCurrentUser(BrowserUserInfo *user_info);
}

static int g_browser_language = XJ380_LANGUAGE_ZH_CN;

static int browser_normalize_language(int language)
{
    return language == XJ380_LANGUAGE_EN_US ? XJ380_LANGUAGE_EN_US : XJ380_LANGUAGE_ZH_CN;
}

static int browser_read_language()
{
    BrowserUserInfo user {};
    xapi_GetCurrentUser(&user);
    if (user.name[0] == '\0') {
        return XJ380_LANGUAGE_ZH_CN;
    }

    char path[256];
    std::snprintf(path, sizeof(path), "/users/%s/settings.json", user.name);

    int fd = open(path, 0);
    if (fd < 0) {
        return XJ380_LANGUAGE_ZH_CN;
    }

    int language = XJ380_LANGUAGE_ZH_CN;
    char *json_text = static_cast<char *>(std::malloc(65536));
    if (json_text != nullptr) {
        std::size_t json_length = 0;
        while (json_length < 65535) {
            int got = read(fd, json_text + json_length, 65535 - json_length);
            if (got <= 0) break;
            json_length += static_cast<std::size_t>(got);
        }
        json_text[json_length] = '\0';
        if (json_length > 0) {
            xapi_JsonDocument document{};
            if (xapi_parseJson(json_text, &document) == XAPI_JSON_PARSE_OK && document.root != nullptr &&
                document.root->type == XAPI_JSON_VALUE_OBJECT) {
                xapi_JsonValue *value = xapi_jsonObjectGet(document.root, "language");
                if (value != nullptr && value->type == XAPI_JSON_VALUE_NUMBER) {
                    language = static_cast<int>(value->as.numberValue);
                }
            }
            xapi_freeJsonDocument(&document);
        }
        std::free(json_text);
    }

    close(fd);
    return browser_normalize_language(language);
}

static char *browser_tr(const char *zh_cn, const char *en_us)
{
    return (char *)(browser_normalize_language(g_browser_language) == XJ380_LANGUAGE_EN_US ? en_us : zh_cn);
}

static std::uint32_t litehtml_color(const litehtml::web_color &color)
{
    return xj380_rgba(color.red, color.green, color.blue, color.alpha);
}

// Decoder pixels use straight alpha. Compose into the page surface before
// upload: WriteBufferA copies RGBA and must not punch image-shaped holes in it.
static XCOLORA browser_source_over(const XCOLORA &source, const XCOLORA &destination)
{
    const unsigned alpha = source.Alpha;
    if (alpha == 0) return destination;
    if (alpha == 255 || destination.Alpha == 0) return source;
    const unsigned inverse = 255 - alpha;
    if (destination.Alpha == 255)
    {
        auto channel = [&](unsigned foreground, unsigned background) {
            return static_cast<unsigned char>((foreground * alpha + background * inverse + 127) / 255);
        };
        return {channel(source.Red, destination.Red), channel(source.Green, destination.Green),
                channel(source.Blue, destination.Blue), 255};
    }
    const unsigned destination_weight = destination.Alpha * inverse;
    const unsigned output_weight = alpha * 255 + destination_weight;
    auto channel = [&](unsigned foreground, unsigned background) {
        return static_cast<unsigned char>((foreground * alpha * 255 + background * destination_weight +
                                           output_weight / 2) / output_weight);
    };
    return {channel(source.Red, destination.Red), channel(source.Green, destination.Green),
            channel(source.Blue, destination.Blue), static_cast<unsigned char>((output_weight + 127) / 255)};
}

static std::string lower_copy(const std::string &value)
{
    std::string lowered = value;
    for (char &ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

static std::string trim_copy(const std::string &value)
{
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

static std::string shorten_url_for_log(const std::string &url)
{
    constexpr std::size_t kMaxLogUrlBytes = 120;
    if (url.size() <= kMaxLogUrlBytes) {
        return url;
    }
    return url.substr(0, kMaxLogUrlBytes) + "...";
}

static void log_line(const std::string &text)
{
    // Preserve JavaScript stack frames instead of silently truncating the
    // entire diagnostic at the serial call's bounded buffer size.
    std::size_t offset = 0;
    do
    {
        auto length = std::min<std::size_t>(480, text.size() - offset);
        const auto newline = text.find('\n', offset);
        if (newline != std::string::npos && newline - offset < length) length = newline - offset;
        while (length && offset + length < text.size() &&
               (static_cast<unsigned char>(text[offset + length]) & 0xc0) == 0x80) --length;
        if (!length && offset < text.size() && text[offset] != '\n') length = 1;
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), "browser: %.*s\n", static_cast<int>(length), text.data() + offset);
        xapi_OutputSerial(buffer);
        offset += length;
        if (offset < text.size() && text[offset] == '\n') ++offset;
    } while (offset < text.size());
}

static std::size_t find_case_insensitive(const std::string &text, const std::string &needle, std::size_t start = 0)
{
    if (needle.empty() || start >= text.size()) {
        return std::string::npos;
    }

    auto it = std::search(text.begin() + static_cast<std::ptrdiff_t>(start),
                          text.end(),
                          needle.begin(),
                          needle.end(),
                          [](char lhs, char rhs) {
                              return std::tolower(static_cast<unsigned char>(lhs)) ==
                                     std::tolower(static_cast<unsigned char>(rhs));
                          });
    if (it == text.end()) {
        return std::string::npos;
    }
    return static_cast<std::size_t>(it - text.begin());
}

static void erase_tag_block(std::string &html, const char *tag_name)
{
    const std::string open_tag = std::string("<") + tag_name;
    const std::string close_tag = std::string("</") + tag_name;

    std::size_t cursor = 0;
    while (cursor < html.size()) {
        const std::size_t open_pos = find_case_insensitive(html, open_tag, cursor);
        if (open_pos == std::string::npos) {
            break;
        }

        const std::size_t close_pos = find_case_insensitive(html, close_tag, open_pos + open_tag.size());
        if (close_pos == std::string::npos) {
            html.erase(open_pos);
            break;
        }

        const std::size_t close_end = html.find('>', close_pos);
        if (close_end == std::string::npos) {
            html.erase(open_pos);
            break;
        }

        html.erase(open_pos, close_end - open_pos + 1);
        cursor = open_pos;
    }
}

static void erase_tag_markers(std::string &html, const char *tag_name)
{
    const std::string open_tag = std::string("<") + tag_name;
    const std::string close_tag = std::string("</") + tag_name;

    std::size_t cursor = 0;
    while (cursor < html.size()) {
        const std::size_t open_pos = find_case_insensitive(html, open_tag, cursor);
        if (open_pos == std::string::npos) {
            break;
        }
        const std::size_t open_end = html.find('>', open_pos);
        if (open_end == std::string::npos) {
            break;
        }
        html.erase(open_pos, open_end - open_pos + 1);
        cursor = open_pos;
    }

    cursor = 0;
    while (cursor < html.size()) {
        const std::size_t close_pos = find_case_insensitive(html, close_tag, cursor);
        if (close_pos == std::string::npos) {
            break;
        }
        const std::size_t close_end = html.find('>', close_pos);
        if (close_end == std::string::npos) {
            break;
        }
        html.erase(close_pos, close_end - close_pos + 1);
        cursor = close_pos;
    }
}

static std::string escape_html_text(const std::string &text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (unsigned char ch : text) {
        switch (ch) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            if (ch == '\r') {
                continue;
            }
            escaped.push_back(static_cast<char>(ch));
            break;
        }
    }
    return escaped;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool is_executable_script_type(const std::string &type)
{
    const std::string lowered = lower_copy(trim_copy(type));
    if (lowered.empty()) {
        return true;
    }
    return lowered.find("javascript") != std::string::npos ||
           lowered.find("ecmascript") != std::string::npos ||
           lowered == "text/jscript" || lowered == "module";
}

static bool is_module_script_type(const std::string &type)
{
    return lower_copy(trim_copy(type)) == "module";
}

// Discovery is separate from execution so the VM observes the native DOM.
struct BrowserPreparedDocument
{
    bool        fallback_used = false;
    std::string html;
    int script_blocks = 0;
};

enum class BrowserInputMode
{
    None,
    Url,
    Find,
    Page,
};

enum class BrowserNavReason
{
    Normal,
    History,
    Reload,
    Replace,
};

enum class BrowserCommand
{
    None,
    Back,
    Forward,
    Reload,
    Stop,
    FocusUrl,
    FocusFind,
    FindPrev,
    FindNext,
    CloseFind,
    Menu,
    SiteInfo,
    DismissNotice,
    RetryNow,
    ToggleTheme,
    ErrorDetails,
    ResourceInfo,
};


struct BrowserButton
{
    BrowserCommand command;
    browser_ui::Rect rect;
    browser_ui::Icon icon;
    const char *label = nullptr;
};

enum class BrowserPopup { None, Menu, SiteInfo, ResourceInfo };

static bool percent_decode(const std::string &input, std::string &output)
{
    output.clear();
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '%') {
            if (i + 2 >= input.size()) {
                return false;
            }
            const int hi = hex_value(input[i + 1]);
            const int lo = hex_value(input[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            output.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            continue;
        }
        output.push_back(ch);
    }
    return true;
}

static int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

static bool decode_base64(const std::string &input, std::string &output)
{
    output.clear();
    output.reserve((input.size() * 3) / 4);

    unsigned int accum = 0;
    int bits = 0;
    for (char ch : input) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const int value = base64_value(ch);
        if (value < 0) {
            return false;
        }

        accum = (accum << 6) | value;
        bits += 6;
        while (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<char>((accum >> bits) & 0xff));
        }
    }
    return true;
}

static bool decode_data_url(const std::string &url, BrowserFetchResult &result)
{
    const std::string lowered = lower_copy(url);
    if (lowered.rfind("data:", 0) != 0) {
        return false;
    }

    const std::size_t comma = url.find(',');
    if (comma == std::string::npos) {
        result.ok = false;
        result.error = browser_tr("data URL 无效", "Invalid data URL");
        return true;
    }

    const std::string meta = url.substr(5, comma - 5);
    const std::string payload = url.substr(comma + 1);
    const std::string lowered_meta = lower_copy(meta);
    const bool is_base64 = lowered_meta.find(";base64") != std::string::npos;

    std::string decoded;
    const bool ok = is_base64 ? decode_base64(payload, decoded) : percent_decode(payload, decoded);
    if (!ok) {
        result.ok = false;
        result.error = is_base64 ? browser_tr("data URL base64 无效", "Invalid data URL base64") :
                                   browser_tr("data URL 转义无效", "Invalid data URL escaping");
        return true;
    }

    std::string content_type = "text/plain;charset=US-ASCII";
    if (!meta.empty()) {
        const std::size_t semi = meta.find(';');
        const std::string explicit_type = trim_copy(meta.substr(0, semi));
        if (!explicit_type.empty()) {
            content_type = explicit_type;
        }
    }

    result.ok = true;
    result.status_code = 200;
    result.final_url = url;
    result.content_type = content_type;
    result.body = std::move(decoded);
    return true;
}

static bool looks_like_html(const BrowserFetchResult &response)
{
    const std::string content_type = lower_copy(response.content_type);
    if (content_type.find("text/html") != std::string::npos ||
        content_type.find("application/xhtml+xml") != std::string::npos) {
        return true;
    }

    const std::size_t probe_len = std::min<std::size_t>(response.body.size(), 1024);
    const std::string probe = response.body.substr(0, probe_len);
    return find_case_insensitive(probe, "<html") != std::string::npos ||
           find_case_insensitive(probe, "<!doctype html") != std::string::npos ||
           find_case_insensitive(probe, "<body") != std::string::npos;
}

static std::string build_fallback_document(const std::string &title,
                                           const std::string &message,
                                           const std::string &excerpt)
{
    std::string html;
    html.reserve(excerpt.size() + 512);
    html += "<html><head><meta charset=\"utf-8\"><title>";
    html += escape_html_text(title);
    html += "</title></head><body style=\"font-family:sans-serif;background:#fafaf9;color:#1f2933;padding:24px;\">";
    html += "<h1 style=\"font-size:24px;margin:0 0 12px 0;\">";
    html += escape_html_text(title);
    html += "</h1><p style=\"margin:0 0 16px 0;\">";
    html += escape_html_text(message);
    html += "</p><pre style=\"white-space:pre-wrap;word-break:break-word;background:#ffffff;border:1px solid #c7d2da;padding:12px;\">";
    html += escape_html_text(excerpt);
    html += "</pre></body></html>";
    return html;
}

static std::string decode_basic_entities(const std::string &text)
{
    std::string decoded;
    decoded.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            decoded.push_back(text[i]);
            continue;
        }

        if (text.compare(i, 5, "&amp;") == 0) {
            decoded.push_back('&');
            i += 4;
        } else if (text.compare(i, 4, "&lt;") == 0) {
            decoded.push_back('<');
            i += 3;
        } else if (text.compare(i, 4, "&gt;") == 0) {
            decoded.push_back('>');
            i += 3;
        } else if (text.compare(i, 6, "&quot;") == 0) {
            decoded.push_back('"');
            i += 5;
        } else if (text.compare(i, 5, "&#39;") == 0) {
            decoded.push_back('\'');
            i += 4;
        } else if (text.compare(i, 6, "&nbsp;") == 0) {
            decoded.push_back(' ');
            i += 5;
        } else {
            decoded.push_back('&');
        }
    }

    return decoded;
}

static std::string html_to_text(std::string html)
{
    erase_tag_block(html, "script");
    erase_tag_block(html, "style");
    erase_tag_markers(html, "noscript");

    std::string text;
    text.reserve(html.size());

    bool in_tag = false;
    bool last_was_space = false;
    for (std::size_t i = 0; i < html.size(); ++i) {
        const char ch = html[i];

        if (!in_tag && ch == '<') {
            const std::size_t tag_end = html.find('>', i);
            const std::size_t tag_text_start = i + 1;
            const std::size_t tag_text_len = (tag_end == std::string::npos ? html.size() : tag_end) - tag_text_start;
            const std::string tag = lower_copy(trim_copy(html.substr(tag_text_start, tag_text_len)));
            const bool closing_tag = !tag.empty() && tag.front() == '/';
            const std::size_t tag_name_start = closing_tag ? 1 : 0;
            std::size_t tag_name_end = tag_name_start;
            while (tag_name_end < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[tag_name_end])) == 0 &&
                   tag[tag_name_end] != '/' &&
                   tag[tag_name_end] != '>') {
                ++tag_name_end;
            }
            const std::string tag_name = tag.substr(tag_name_start, tag_name_end - tag_name_start);

            if (tag_name == "br" || tag_name == "p" || tag_name == "div" || tag_name == "li" ||
                tag_name == "h1" || tag_name == "h2" || tag_name == "h3" || tag_name == "hr" ||
                tag_name == "table" || tag_name == "tr") {
                if (text.empty() || text.back() != '\n') {
                    text.push_back('\n');
                }
                last_was_space = false;
            }
            if (!closing_tag && (tag_name == "td" || tag_name == "th")) {
                if (!text.empty() && text.back() != '\n' && text.back() != '\t') {
                    text.push_back('\t');
                }
                last_was_space = false;
            }

            in_tag = true;
            continue;
        }

        if (in_tag) {
            if (ch == '>') {
                in_tag = false;
            }
            continue;
        }

        if (ch == '\r') {
            continue;
        }
        if (ch == '\n' || ch == '\t') {
            if (!text.empty() && text.back() != '\n') {
                text.push_back('\n');
            }
            last_was_space = false;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!last_was_space && !text.empty() && text.back() != '\n') {
                text.push_back(' ');
                last_was_space = true;
            }
            continue;
        }

        text.push_back(ch);
        last_was_space = false;
    }

    return decode_basic_entities(text);
}

static std::vector<std::string> wrap_text_for_width(const std::string &text, int pixel_width)
{
    std::vector<std::string> lines;
    std::string current_line;

    const auto flush_line = [&]() {
        lines.push_back(current_line);
        current_line.clear();
    };

    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        std::size_t line_end = text.find('\n', cursor);
        const bool explicit_break = line_end != std::string::npos;
        const std::string raw_line = explicit_break ? text.substr(cursor, line_end - cursor) : text.substr(cursor);

        if (raw_line.empty()) {
            lines.emplace_back();
        } else {
            std::size_t start = 0;
            while (start < raw_line.size()) {
                while (start < raw_line.size() && raw_line[start] == ' ') {
                    ++start;
                }
                if (start >= raw_line.size()) {
                    break;
                }

                std::size_t end = start;
                std::string candidate;
                std::size_t last_fit = start;

                while (end <= raw_line.size()) {
                    const std::string part = raw_line.substr(start, end - start);
                    const unsigned long long width = xapi_CalcTextWidth(const_cast<char *>(part.c_str()), 16);
                    if (width > static_cast<unsigned long long>(pixel_width) && !candidate.empty()) {
                        break;
                    }
                    if (width <= static_cast<unsigned long long>(pixel_width)) {
                        candidate = part;
                        last_fit = end;
                    }

                    std::size_t next_space = raw_line.find(' ', end);
                    if (next_space == std::string::npos) {
                        end = raw_line.size();
                    } else if (next_space == end) {
                        ++end;
                    } else {
                        end = next_space;
                    }

                    if (end >= raw_line.size()) {
                        const std::string tail = raw_line.substr(start);
                        const unsigned long long tail_width = xapi_CalcTextWidth(const_cast<char *>(tail.c_str()), 16);
                        if (tail_width <= static_cast<unsigned long long>(pixel_width)) {
                            candidate = tail;
                            last_fit = raw_line.size();
                        }
                        break;
                    }
                }

                if (candidate.empty()) {
                    const std::size_t first_end = start + std::max<std::size_t>(
                        1, ime_utf8_scalar_bytes(raw_line.data() + start, raw_line.size() - start));
                    std::size_t hard_end = first_end;
                    while (hard_end <= raw_line.size()) {
                        const std::string part = raw_line.substr(start, hard_end - start);
                        const unsigned long long width = xapi_CalcTextWidth(const_cast<char *>(part.c_str()), 16);
                        if (width > static_cast<unsigned long long>(pixel_width) && hard_end > first_end)
                        {
                            hard_end = ime_utf8_previous(raw_line.data(), hard_end);
                            break;
                        }
                        if (hard_end == raw_line.size()) {
                            break;
                        }
                        hard_end += std::max<std::size_t>(
                            1, ime_utf8_scalar_bytes(raw_line.data() + hard_end, raw_line.size() - hard_end));
                    }
                    candidate = raw_line.substr(start, hard_end - start);
                    last_fit = hard_end;
                }

                current_line = trim_copy(candidate);
                flush_line();
                start = last_fit;
            }
        }

        if (!explicit_break) {
            break;
        }
        cursor = line_end + 1;
    }

    if (lines.empty()) {
        lines.emplace_back(browser_tr("没有内容。", "No content."));
    }

    return lines;
}

class BrowserApp;

class BrowserContainer final : public litehtml::document_container
{
    friend class BrowserApp;
public:
    explicit BrowserContainer(BrowserApp &app) : app_(app) {}

    litehtml::uint_ptr create_font(const litehtml::font_description &descr,
                                   const litehtml::document *,
                                   litehtml::font_metrics *fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    XTextStyle font_style(litehtml::uint_ptr hFont) const;
    litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc,
                   const char *text,
                   litehtml::uint_ptr hFont,
                   litehtml::web_color color,
                   const litehtml::position &pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override { return pt * 96.0f / 72.0f; }
    litehtml::pixel_t get_default_font_size() const override { return 16.0f; }
    const char *get_default_font_name() const override { return "sans"; }
    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker &marker) override;
    void load_image(const char *src, const char *baseurl, bool redraw_on_ready) override;
    void get_image_size(const char *src, const char *baseurl, litehtml::size &sz) override;
    void draw_image(litehtml::uint_ptr hdc,
                    const litehtml::background_layer &layer,
                    const std::string &url,
                    const std::string &base_url) override;
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const litehtml::web_color &color) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc,
                              const litehtml::background_layer &layer,
                              const litehtml::background_layer::linear_gradient &gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc,
                              const litehtml::background_layer &layer,
                              const litehtml::background_layer::radial_gradient &gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc,
                             const litehtml::background_layer &layer,
                             const litehtml::background_layer::conic_gradient &gradient) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders &borders, const litehtml::position &draw_pos, bool root) override;

    void set_caption(const char *caption) override;
    void set_base_url(const char *base_url) override;
    void link(const std::shared_ptr<litehtml::document> &, const litehtml::element::ptr &) override {}
    void on_anchor_click(const char *url, const litehtml::element::ptr &) override;
    void on_mouse_event(const litehtml::element::ptr &, litehtml::mouse_event) override {}
    void set_cursor(const char *) override {}
    void transform_text(litehtml::string &text, litehtml::text_transform tt) override;
    void import_css(litehtml::string &text, const litehtml::string &url, litehtml::string &baseurl) override;
    void set_clip(const litehtml::position &pos, const litehtml::border_radiuses &) override;
    void del_clip() override;
    void get_viewport(litehtml::position &viewport) const override { viewport = viewport_; }
    litehtml::element::ptr create_element(const char *, const litehtml::string_map &,
                                         const std::shared_ptr<litehtml::document> &) override;

    void get_media_features(litehtml::media_features &media) const override;
    void get_language(litehtml::string &language, litehtml::string &culture) const override;

    void begin_draw(const litehtml::position &clip) { clip_stack_.clear(); clip_stack_.push_back(clip); }
    void end_draw() { clip_stack_.clear(); }
    void set_viewport(int width, int height)
    {
        viewport_.x = 0;
        viewport_.y = 0;
        viewport_.width = static_cast<float>(width);
        viewport_.height = static_cast<float>(height);
    }

private:
    struct FontHandle
    {
        int                    size = 16;
        XTextStyle             style {};
        XTextMetrics           native_metrics {};
        litehtml::font_metrics metrics {};
    };

    BrowserApp                &app_;
    litehtml::position         viewport_;
    std::vector<litehtml::position> clip_stack_;

    litehtml::position current_clip() const;
    void fill_rect(HDLE hdc, const litehtml::position &pos, std::uint32_t color) const;
    void stroke_border(HDLE hdc,
                       float x1,
                       float y1,
                       float x2,
                       float y2,
                       float width,
                       std::uint32_t color) const;
    void blit_image(HDLE hdc,
                    const BrowserDecodedImage &image,
                    const litehtml::position &dest,
                    const litehtml::position &clip) const;
};

class BrowserApp
{
public:
    BrowserApp() : container_(*this)
    {
        g_browser_language = browser_read_language();
        url_input_[0] = 0;
        input_len_ = static_cast<int>(std::strlen(url_input_));
        input_cursor_ = input_len_;
        std::snprintf(status_, sizeof(status_), "%s", browser_tr("就绪", "Ready"));
    }

    ~BrowserApp();

    int run();
    void initialize_window();
    void open_initial_url(const char *url) { if (url && *url) navigate_to(url); }
    void handle_message(unsigned long long type, unsigned long long hData, unsigned long long lData);
    void request_navigation(const std::string &url);
    void set_title(const std::string &title);
    void set_base_url(const std::string &url)
    {
        if (!base_url_set_ && !url.empty())
        {
            base_url_set_ = true;
            base_url_ = browser_resolve_url(loading_ ? loading_document_url_ : current_url_, url);
        }
    }
    void enqueue_message(unsigned long long type, unsigned long long high, unsigned long long low);
    void drain_messages(bool during_fetch = false);
    bool continue_fetch();
    const std::string &base_url() const { return base_url_; }
    std::string resolve_url(const std::string &url, const std::string &base_url) const;
    BrowserFetchResult fetch_resource(const std::string &url, bool navigation = false);
    const BrowserDecodedImage *load_image_resource(const std::string &url);
    void invalidate_view() { invalidate_content(); }

private:
    friend class BrowserContainer;
    friend class BrowserFormElement;
    struct DeferredEvent
    {
        BrowserEvent event {};
        litehtml::document::weak_ptr document;
    };
    static constexpr unsigned deferred_event_capacity = 1024;
    DeferredEvent deferred_events_[deferred_event_capacity] {};
    unsigned deferred_event_head_ = 0, deferred_event_count_ = 0;
    // Published by the main thread; the message producer never reads the DOM.
    std::atomic<unsigned> input_event_epoch_ {0};
    bool draining_events_ = false;
    bool deferred_resize_ = false;
    unsigned long long resize_width_ = 0, resize_height_ = 0;
    bool unsafe_input_event(const BrowserEvent &event) const;
    void release_input_event(const BrowserEvent &event);
    void clear_deferred_events(bool preserve_resize = false);
    browser_forms::Model forms_;
    int focused_form_ = -1;
    unsigned long long form_generation_ = 0;
    char page_input_[16385] {};
    int page_len_ = 0, page_cursor_ = 0, page_anchor_ = 0;
    browser_ui::Canvas form_canvas_;
    std::vector<XCOLORA> form_background_;
    struct HistoryState
    {
        std::vector<browser_forms::State> forms;
        int scroll = 0;
        unsigned int document = 0;
        std::string script_state = "null";
    };
    std::vector<HistoryState> history_states_;
    void save_history_state();
    void focus_form(int index, bool select_all = false);
    void tab_form(int direction);
    void activate_form(int index);
    void submit_form(int index, bool dispatch_event = true);
    void draw_form_control(HDLE hdc, const litehtml::element::ptr &element,
                           const litehtml::position &position, const litehtml::position *clip);
    unsigned long long form_control_id() const
    {
        return 0x100000000ULL + (form_generation_ << 32) +
               static_cast<unsigned>(focused_form_ + 1);
    }
    struct CachedResource
    {
        std::string        url;
        BrowserFetchResult result;
        unsigned int serial = 0;
        unsigned long long expires = 0, cookie_generation = 0;
    };

    struct CachedImage
    {
        std::string         url;
        BrowserDecodedImage image;
        bool                failed = false;
        unsigned int serial = 0;
        unsigned long long expires = 0, cookie_generation = 0;
    };
    struct ImageSize
    {
        std::string url;
        int width = 0, height = 0;
        unsigned int failed_serial = ~0U;
    };

    struct DeferredScript
    {
        std::string url, base, source;
        litehtml::element::ptr node;
        bool deferred = false, async = false;
        bool applied = false;
        bool module = false, parser_inserted = true;
        unsigned long long module_entry = 0;
        std::string integrity;
    };
    struct ModuleTransfer
    {
        unsigned long long token = 0;
        std::string url, referrer;
    };
    struct ScriptLink
    {
        litehtml::element::ptr node;
        std::string url;
        bool stylesheet = false;
        unsigned long long module_entry = 0;
    };
    struct ScriptHistoryUpdate
    {
        std::string url, state;
        bool replace = false;
    };
    BrowserResourceQueue resource_jobs_;
    std::vector<std::string> image_decode_queue_;
    std::vector<DeferredScript> deferred_scripts_;
    std::unique_ptr<BrowserScriptRuntime> script_runtime_;
    std::vector<litehtml::element::ptr> dynamic_scripts_;
    std::vector<ModuleTransfer> module_transfers_;
    std::vector<ScriptLink> script_links_;
    std::vector<ScriptHistoryUpdate> script_history_updates_;
    std::vector<std::weak_ptr<litehtml::element>> started_script_nodes_;
    std::string script_history_base_;
    std::string loading_history_state_ = "null";
    int script_history_delta_ = 0;
    bool script_history_go_pending_ = false, script_scroll_pending_ = false;
    double script_scroll_x_ = 0, script_scroll_y_ = 0;
    litehtml::element::ptr script_submission_;
    std::string script_navigation_;
    bool script_replace_ = false, script_dom_ready_ = false, script_load_ready_ = false;
    bool processing_scripts_ = false;
    bool module_fetch_cancelled_ = false;
    unsigned int executed_scripts_ = 0;
    std::size_t script_bytes_ = 0;
    std::string prepared_html_;
    unsigned int resource_serial_ = 0, document_serial_ = 0, css_import_count_ = 0;
    bool force_reload_ = false, resource_failed_ = false, processing_resources_ = false;
    bool rebuild_pending_ = false;
    std::string resource_error_, retry_notice_url_;
    BrowserEventQueue      events_;
    bool                   base_url_set_ = false;
    bool                   loading_ = false;
    bool                   cancelled_ = false;
    unsigned int           active_fetches_ = 0;
    unsigned int           loading_serial_ = 0;
    int                    pending_history_index_ = -1;
    HDLE                   window_ = 0;
    BrowserContainer       container_;
    litehtml::document::ptr document_;
    std::string            base_url_;
    std::string            loading_document_url_;
    std::string            pending_url_;
    std::string            page_title_;
    bool                   need_redraw_ = true;
    bool                   frame_dirty_ = true;
    bool                   header_dirty_ = true;
    bool                   content_dirty_ = true;
    bool                   load_pending_ = false;
    bool                   startup_guard_ = true;
    bool                   suppress_history_record_ = false;
    BrowserNavReason       pending_nav_reason_ = BrowserNavReason::Normal;
    BrowserInputMode       input_mode_ = BrowserInputMode::Url;
    int                    window_width_ = BROWSER_WIDTH;
    int                    window_height_ = BROWSER_HEIGHT;
    int                    scroll_y_ = 0;
    int                    text_content_height_ = 0;
    unsigned int           navigation_serial_ = 0;
    char                   url_input_[4096] {};
    int                    input_len_ = 0;
    int                    input_cursor_ = 0;
    int                         input_anchor_ = 0;
    bool                        shift_down_   = false;
    bool                   ctrl_down_ = false, alt_down_ = false;
    bool                   find_visible_ = false, dark_theme_ = false;
    bool                   page_failed_ = false, page_secure_ = false, failure_details_open_ = false;
    bool                   notice_dismissed_ = false, notice_painted_ = false;
    BrowserPopup           popup_ = BrowserPopup::None;
    BrowserCommand         hovered_command_ = BrowserCommand::None;
    browser_ui::Canvas     chrome_canvas_;
    browser_ui::Backdrop   chrome_backdrop_, hint_backdrop_, notice_backdrop_;
    int pointer_x_ = -1, pointer_y_ = -1;
    std::string            loading_target_, failure_url_, failure_detail_;
    long long              failure_code_ = 0;
    unsigned int           failure_tls_ = 0, failure_attempts_ = 0;

    char                   find_input_[128] {};
    int                    find_len_ = 0;
    int                    find_cursor_ = 0;
    int                         find_anchor_      = 0;
    int                    find_match_index_ = -1;
    int                    find_match_count_ = 0;
    char                   status_[128] {};
    std::string            network_notice_;
    std::string            hovered_url_;
    std::string            page_text_;
    std::string            current_url_;
    std::vector<std::string> history_;
    int                    history_index_ = -1;
    std::vector<std::string> text_lines_;
    std::vector<CachedResource> resource_cache_;
    std::vector<CachedImage> image_cache_;
    std::vector<ImageSize> image_sizes_;

    void invalidate_header() { header_dirty_ = true; need_redraw_ = true; }
    void invalidate_content() { content_dirty_ = true; need_redraw_ = true; }
    void invalidate_all() { frame_dirty_ = true; header_dirty_ = true; content_dirty_ = true; need_redraw_ = true; }
    void load_current_url();
    void reset_resource_jobs();
    bool resources_pending() const;
    bool enqueue_resource(const std::string &url, unsigned int kind, unsigned int priority = 10);
    const BrowserFetchResult *resource_response(const std::string &url) const;
    void pump_resources(bool update_document = true);
    bool wait_stylesheets(unsigned int serial);
    void decode_image_resource(const std::string &url, const BrowserFetchResult &response);
    void rebuild_resource_document(bool recreate);
    void prioritize_visible_images();
    void render();
    void render_frame();
    void render_header();
    void render_content();
    void refresh_content_view();
    void set_status(const std::string &text);
    void update_retry_status(const BrowserFetchResult &failure, const BrowserRetryStatus &retry);
    void prepare_text_view(const BrowserFetchResult &response);
    BrowserPreparedDocument prepare_document_html(const BrowserFetchResult &response);
    void initialize_scripts(const std::string &url);
    void pump_scripts();
    void sync_script_document();
    void flush_script_submission();
    bool dispatch_script(const litehtml::element::ptr &target, const std::string &type,
                         const std::string &key = "", int key_code = 0);
    void execute_script(const std::string &source, const std::string &url);
    bool start_module_script(DeferredScript &program);
    void pump_module_requests();
    void queue_script_link(const litehtml::element::ptr &node);
    void pump_script_links();
    bool update_script_history(const std::string &url, bool replace, const std::string &state);
    void flush_script_history();
    void flush_script_effects();
    BrowserScriptViewport script_viewport() const;
    const BrowserFetchResult *find_cached_resource(const std::string &url) const;
    void store_cached_resource(const std::string &url, const BrowserFetchResult &result);
    const CachedImage *find_cached_image(const std::string &url) const;
    const ImageSize *find_image_size(const std::string &url) const;
    browser_ui::Layout chrome_layout() const
    {
        return browser_ui::Layout::make(window_width_, window_height_, notice_visible());
    }
    browser_ui::Theme chrome_theme() const
    {
        return dark_theme_ ? browser_ui::Theme::dark() : browser_ui::Theme::light();
    }
    bool notice_visible() const { return !page_failed_ && !notice_dismissed_ && !network_notice_.empty(); }
    bool browser_busy() const { return loading_ || load_pending_ || active_fetches_ || resources_pending(); }
    int input_right() const { return chrome_layout().address_text.right(); }
    int find_right() const { return chrome_layout().find_text.right(); }
    int content_x() const { return 0; }
    int content_y() const { return chrome_layout().content_top; }
    int content_width() const { return std::max(1, window_width_ - BROWSER_SCROLLBAR_WIDTH); }
    int content_height() const { return std::max(1, window_height_ - content_y()); }
    int scrollbar_x() const { return content_width(); }
    std::vector<BrowserButton> chrome_buttons() const;
    browser_ui::Rect control_rect(BrowserCommand command) const;
    void render_overlays(bool content_repainted = false);
    void render_native_page();
    void draw_chrome_button(const BrowserButton &button);
    void draw_chrome_input(browser_ui::Rect field, bool url);
    void draw_chrome_paragraph(browser_ui::Rect rect, const std::string &text, uint32_t color);
    void close_chrome_popup();
    void update_window_title();
    std::string page_host() const;
    int  scroll_content_height() const;
    void clamp_scroll();
    void draw_scrollbar();
    bool handle_scrollbar_click(int x, int y);
    bool hit_content_area(int x, int y) const;
    bool hit_url_input(int x, int y) const;
    bool hit_find_input(int x, int y) const;
    BrowserCommand command_at(int x, int y) const;
    bool command_enabled(BrowserCommand command) const;
    void execute_command(BrowserCommand command);
    void navigate_to(const std::string &url, BrowserNavReason reason = BrowserNavReason::Normal);
    void record_history(const std::string &url, bool replace = false, bool force_entry = false);
    void go_history(int delta);
    void reload_current();
    void stop_loading();
    void update_ime_context();
    void focus_url(bool select_all);
    void focus_find(bool select_all);
    void insert_char(uint64_t packed);
    bool                      replace_input(const char *text, size_t bytes);
    void backspace_input();
    char *active_input()
    {
        return input_mode_ == BrowserInputMode::Page ? page_input_ :
               input_mode_ == BrowserInputMode::Url ? url_input_ : find_input_;
    }
    int &active_length()
    {
        return input_mode_ == BrowserInputMode::Page ? page_len_ :
               input_mode_ == BrowserInputMode::Url ? input_len_ : find_len_;
    }
    int &active_cursor()
    {
        return input_mode_ == BrowserInputMode::Page ? page_cursor_ :
               input_mode_ == BrowserInputMode::Url ? input_cursor_ : find_cursor_;
    }
    int &active_anchor()
    {
        return input_mode_ == BrowserInputMode::Page ? page_anchor_ :
               input_mode_ == BrowserInputMode::Url ? input_anchor_ : find_anchor_;
    }
    void update_find_matches();
    void find_next(int direction);
    struct TextRun { std::size_t offset, bytes; litehtml::position box; XTextStyle style; };
    std::vector<TextRun> text_runs_;
    std::vector<std::size_t> find_matches_;
    void collect_text(const litehtml::element::ptr &element);
    void scroll_to_fragment(const std::string &url);
    void draw_find_highlights();
    int  text_width_px(const std::string &text, unsigned int size) const;
    std::string href_from_element(litehtml::element::const_ptr element) const;
    void process_pointer_move(int x, int y);
    void process_pointer_click(int x, int y);
};

static BrowserApp *g_app = nullptr;

#include "browser_form_controls.h"
#include "browser_scripting.h"

BrowserApp::~BrowserApp()
{
    clear_deferred_events();
    BrowserEvent event;
    while (events_.pop(event)) release_input_event(event);
}

void BrowserApp::release_input_event(const BrowserEvent &event)
{
    // Discarding input for an old document must still release paste ownership
    // and remember physical modifier transitions, rather than leave Ctrl held.
    if (event.type == MSG_PASTE) xapi_FinishPaste(event.high, 0);
    if (event.type == MSG_EDIT) xapi_FinishPaste(event.low, 0);
    if (event.type == MSG_KEYDOWN || event.type == MSG_KEYUP)
    {
        if (event.low == BROWSER_KEY_SHIFT) shift_down_ = event.type == MSG_KEYDOWN;
        if (event.low == 134) ctrl_down_ = event.type == MSG_KEYDOWN;
        if (event.low == 135) alt_down_ = event.type == MSG_KEYDOWN;
    }
}

void BrowserApp::clear_deferred_events(bool preserve_resize)
{
    while (deferred_event_count_)
    {
        auto &entry = deferred_events_[deferred_event_head_];
        if (preserve_resize && entry.event.type == MSG_RESIZE)
        {
            deferred_resize_ = true;
            resize_width_ = entry.event.high;
            resize_height_ = entry.event.low;
        }
        else release_input_event(entry.event);
        entry = {};
        deferred_event_head_ = (deferred_event_head_ + 1) % deferred_event_capacity;
        --deferred_event_count_;
    }
}

bool BrowserApp::unsafe_input_event(const BrowserEvent &event) const
{
    // Even an otherwise ignored window message refreshes the IME context in
    // handle_message, which reads page geometry while a field is focused.
    if (input_mode_ == BrowserInputMode::Page) return true;
    if (event.type == MSG_RESIZE || event.type == MSG_ROLLER) return true;
    if (event.type == MSG_LBUTTON)
    {
        const auto command = command_at(static_cast<int>(event.high), static_cast<int>(event.low));
        if (command == BrowserCommand::Back || command == BrowserCommand::Forward ||
            command == BrowserCommand::FocusFind || command == BrowserCommand::FindNext ||
            command == BrowserCommand::FindPrev) return true;
        return command == BrowserCommand::None && static_cast<int>(event.low) >= content_y() &&
            !hit_find_input(static_cast<int>(event.high), static_cast<int>(event.low));
    }
    if (event.type == MSG_CHAR || event.type == MSG_SPCHAR || event.type == MSG_KEYDOWN ||
        event.type == MSG_KEYUP || event.type == MSG_PASTE || event.type == MSG_EDIT)
    {
        if (input_mode_ == BrowserInputMode::Page || input_mode_ == BrowserInputMode::Find ||
            (input_mode_ == BrowserInputMode::None && document_)) return true;
        if (event.type == MSG_SPCHAR && (event.low == '\t' || event.low == BROWSER_KEY_F3)) return true;
        if (event.type == MSG_KEYDOWN && alt_down_ &&
            (event.low == BROWSER_KEY_LEFT || event.low == BROWSER_KEY_RIGHT)) return true;
        // A URL fragment can scroll the current document rather than start IO.
        if (event.type == MSG_SPCHAR && event.low == '\n' && input_mode_ == BrowserInputMode::Url)
        {
            const std::string target(url_input_);
            const auto fragment = target.find('#');
            if (fragment != std::string::npos &&
                target.substr(0, fragment) == current_url_.substr(0, current_url_.find('#'))) return true;
        }
    }
    return false;
}

void BrowserApp::enqueue_message(unsigned long long type, unsigned long long high, unsigned long long low)
{
    const BrowserEvent event {type, high, low, input_event_epoch_.load(std::memory_order_acquire)};
    while (!events_.push(event))
    {
        if (type == MSG_MOVE) return; // Motion is disposable; keyboard/paste ownership is not.
        xapi_Sleep(1);
    }
}

void BrowserApp::drain_messages(bool during_fetch)
{
    if (!during_fetch && draining_events_) return;
    during_fetch = during_fetch || processing_scripts_ || processing_resources_;
    struct DrainScope
    {
        bool &flag;
        bool previous;
        DrainScope(bool &value, bool normal) : flag(value), previous(value) { if (normal) flag = true; }
        ~DrainScope() { flag = previous; }
    } scope(draining_events_, !during_fetch);

    // Navigation invalidates the previous document's barrier immediately, so
    // stale page input cannot block a new URL entered while loading. A window
    // resize remains global even when its original document is gone.
    while (deferred_event_count_)
    {
        auto &entry = deferred_events_[deferred_event_head_];
        if (entry.event.epoch == input_event_epoch_.load(std::memory_order_relaxed)) break;
        if (entry.event.type == MSG_RESIZE)
        {
            deferred_resize_ = true;
            resize_width_ = entry.event.high;
            resize_height_ = entry.event.low;
        }
        else release_input_event(entry.event);
        entry = {};
        deferred_event_head_ = (deferred_event_head_ + 1) % deferred_event_capacity;
        --deferred_event_count_;
    }
    if (!during_fetch && deferred_resize_)
    {
        deferred_resize_ = false;
        handle_message(MSG_RESIZE, resize_width_, resize_height_);
    }

    unsigned processed = 0;
    while (processed++ < 256)
    {
        BrowserEvent event;
        if (!during_fetch && deferred_event_count_)
        {
            // Pop before dispatch: page handlers can perform nested HTTP IO
            // and append more input, or initiate a new navigation.
            auto entry = std::move(deferred_events_[deferred_event_head_]);
            deferred_events_[deferred_event_head_] = {};
            deferred_event_head_ = (deferred_event_head_ + 1) % deferred_event_capacity;
            --deferred_event_count_;
            event = entry.event;
            if (event.type != MSG_RESIZE &&
                (event.epoch != input_event_epoch_.load(std::memory_order_relaxed) ||
                 !document_ || entry.document.lock() != document_))
            {
                release_input_event(event);
                continue;
            }
        }
        else
        {
            if (!events_.peek(event)) break;
            if (event.type != MSG_RESIZE &&
                event.epoch != input_event_epoch_.load(std::memory_order_relaxed))
            {
                events_.pop(event);
                release_input_event(event);
                continue;
            }
            if (during_fetch)
            {
                // Hover motion is expendable. Clicks, focus changes, scroll,
                // resize and keys are not: a click establishes a FIFO barrier
                // even if no page field has received focus yet.
                if (event.type == MSG_MOVE)
                {
                    events_.pop(event);
                    continue;
                }
                const auto command = event.type == MSG_LBUTTON ?
                    command_at(static_cast<int>(event.high), static_cast<int>(event.low)) : BrowserCommand::None;
                const bool escape = event.type == MSG_SPCHAR && event.low == 128 && (loading_ || load_pending_);
                if (command == BrowserCommand::Stop || escape)
                {
                    events_.pop(event);
                    // Do not dispatch page listeners or refresh page IME
                    // geometry from inside IO. Background Escape stays queued.
                    if (escape && popup_ != BrowserPopup::None) close_chrome_popup();
                    else if (escape && find_visible_) execute_command(BrowserCommand::CloseFind);
                    else stop_loading();
                    continue;
                }
                bool control = ctrl_down_;
                if (event.type == MSG_KEYDOWN && (event.low == 'l' || event.low == 'L'))
                {
                    for (unsigned offset = 0; offset < deferred_event_count_; ++offset)
                    {
                        const auto &pending = deferred_events_[(deferred_event_head_ + offset) %
                                                              deferred_event_capacity].event;
                        if (pending.low == 134 && (pending.type == MSG_KEYDOWN || pending.type == MSG_KEYUP))
                            control = pending.type == MSG_KEYDOWN;
                    }
                }
                const bool url_focus = event.type == MSG_LBUTTON &&
                    (command == BrowserCommand::FocusUrl ||
                     hit_url_input(static_cast<int>(event.high), static_cast<int>(event.low)));
                const bool url_shortcut = event.type == MSG_KEYDOWN && control &&
                    (event.low == 'l' || event.low == 'L');
                if (url_focus || url_shortcut)
                {
                    // Explicit browser-chrome focus supersedes an unprocessed
                    // page gesture. Keep native resize and modifier state, but
                    // never replay the old page click over the new URL field.
                    // Do not advance the epoch here: the URL characters may
                    // already be published in the original SPSC queue.
                    clear_deferred_events(true);
                    events_.pop(event);
                    if (url_shortcut) ctrl_down_ = true;
                    handle_message(event.type, event.high, event.low);
                    continue;
                }
                if (deferred_event_count_ || unsafe_input_event(event))
                {
                    // Leave the original SPSC head intact when full. The IO
                    // operation can finish; the safe main loop then relieves
                    // backpressure without silently losing keys or leases.
                    if (deferred_event_count_ == deferred_event_capacity) break;
                    events_.pop(event);
                    const unsigned index = (deferred_event_head_ + deferred_event_count_) % deferred_event_capacity;
                    deferred_events_[index] = {event, document_};
                    ++deferred_event_count_;
                    continue;
                }
            }
            events_.pop(event);
        }
        handle_message(event.type, event.high, event.low);
    }
}

bool BrowserApp::continue_fetch()
{
    drain_messages(true);
    // Parser-blocking script IO can take several turns. Advance requests that
    // the page already started without evaluating JavaScript from inside IO.
    if (loading_ && !processing_resources_ && !cancelled_) pump_resources(false);
    if (header_dirty_)
    {
        header_dirty_ = false;
        render_header(); render_overlays();
        xapi_RefreshWindow(window_);
        need_redraw_ = frame_dirty_ || content_dirty_ || header_dirty_;
    }
    return !cancelled_ && (!loading_ || loading_serial_ == navigation_serial_);
}

litehtml::position BrowserContainer::current_clip() const
{
    if (clip_stack_.empty()) {
        return viewport_;
    }
    return clip_stack_.back();
}

void BrowserContainer::fill_rect(HDLE hdc, const litehtml::position &pos, std::uint32_t color) const
{
    const auto clipped = pos.intersect(current_clip());
    const unsigned alpha = color & 255;
    if (clipped.width <= 0 || clipped.height <= 0 || alpha == 0) return;
    const unsigned left = static_cast<unsigned>(std::max(0.0f, std::floor(clipped.x)));
    const unsigned top = static_cast<unsigned>(std::max(0.0f, std::floor(clipped.y)));
    const unsigned right = static_cast<unsigned>(std::max(0.0f, std::ceil(clipped.right())));
    const unsigned bottom = static_cast<unsigned>(std::max(0.0f, std::ceil(clipped.bottom())));
    if (right <= left || bottom <= top) return;
    if (alpha == 255)
    {
        // CSS rectangles are half-open; native DrawRect endpoints are inclusive.
        xapi_DrawRect(hdc, left, top, right - 1, bottom - 1, color, true);
        return;
    }
    std::vector<XCOLORA> pixels(static_cast<std::size_t>(right - left) * (bottom - top));
    xapi_ReadBufferA(hdc, left, top, right - left, bottom - top, pixels.data());
    const XCOLORA foreground {static_cast<unsigned char>(color >> 24), static_cast<unsigned char>(color >> 16),
                             static_cast<unsigned char>(color >> 8), static_cast<unsigned char>(alpha)};
    for (auto &pixel : pixels) pixel = browser_source_over(foreground, pixel);
    xapi_WriteBufferA(hdc, left, top, right - left, bottom - top, pixels.data());
}

void BrowserContainer::stroke_border(HDLE hdc,
                                     float x1,
                                     float y1,
                                     float x2,
                                     float y2,
                                     float width,
                                     std::uint32_t color) const
{
    if (width <= 0.0f || x2 <= x1 || y2 <= y1) {
        return;
    }

    litehtml::position rect(x1, y1, x2 - x1, y2 - y1);
    fill_rect(hdc, rect, color);
}

litehtml::uint_ptr BrowserContainer::create_font(const litehtml::font_description &descr,
                                                 const litehtml::document *,
                                                 litehtml::font_metrics *fm)
{
    FontHandle *font = new FontHandle();
    font->size = static_cast<int>(std::clamp(descr.size, 1.0f, 256.0f) + 0.5f);
    std::string family = descr.family;
    std::transform(family.begin(), family.end(), family.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool mono = family.find("monospace") != std::string::npos || family.find("courier") != std::string::npos ||
                      family.find("consolas") != std::string::npos;
    font->style = {mono ? XTEXT_FONT_MONO : XTEXT_FONT_UI, static_cast<std::uint32_t>(font->size),
                   XTEXT_SIZE_EM_PIXELS, XTEXT_ANCHOR_BASELINE, XTEXT_REFERENCE_AUTO, 0};
    if (xapi_GetTextMetrics(&font->style, nullptr, &font->native_metrics) < 0)
    {
        delete font;
        if (fm) *fm = {};
        return 0;
    }
    const auto &measured = font->native_metrics;
    font->metrics.font_size = static_cast<float>(font->size);
    font->metrics.height = static_cast<float>(measured.ascent + measured.descent);
    font->metrics.ascent = static_cast<float>(measured.ascent);
    font->metrics.descent = static_cast<float>(measured.descent);
    font->metrics.x_height = static_cast<float>(measured.x_height);
    font->metrics.ch_width = static_cast<float>(measured.ch_width);
    font->metrics.draw_spaces = true;
    font->metrics.sub_shift = static_cast<float>(std::max(1, measured.descent / 2));
    font->metrics.super_shift = static_cast<float>(std::max(1, measured.x_height / 3));

    if (fm != nullptr) {
        *fm = font->metrics;
    }
    return reinterpret_cast<litehtml::uint_ptr>(font);
}

XTextStyle BrowserContainer::font_style(litehtml::uint_ptr hFont) const
{
    const auto *font = reinterpret_cast<const FontHandle *>(hFont);
    XTextStyle style = font ? font->style : browser_ui::text_style(16, XTEXT_SIZE_EM_PIXELS);
    style.anchor = XTEXT_ANCHOR_REFERENCE;
    return style;
}

void BrowserContainer::delete_font(litehtml::uint_ptr hFont)
{
    delete reinterpret_cast<FontHandle *>(hFont);
}

litehtml::pixel_t BrowserContainer::text_width(const char *text, litehtml::uint_ptr hFont)
{
    FontHandle *font = reinterpret_cast<FontHandle *>(hFont);
    if (text == nullptr || font == nullptr) {
        return 0;
    }
    XTextMetrics measured {};
    return xapi_GetTextMetrics(&font->style, text, &measured) < 0 ? 0.0f : static_cast<float>(measured.advance_x);
}

void BrowserContainer::draw_text(litehtml::uint_ptr hdc,
                                 const char *text,
                                 litehtml::uint_ptr hFont,
                                 litehtml::web_color color,
                                 const litehtml::position &pos)
{
    FontHandle *font = reinterpret_cast<FontHandle *>(hFont);
    if (text == nullptr || font == nullptr) {
        return;
    }

    const litehtml::position clip = current_clip();
    const int left = static_cast<int>(std::ceil(clip.x));
    const int top = static_cast<int>(std::ceil(clip.y));
    const int right = static_cast<int>(std::floor(clip.right()));
    const int bottom = static_cast<int>(std::floor(clip.bottom()));
    if (right <= left || bottom <= top) return;
    const XTextRect native_clip {left, top, right - left, bottom - top};
    const int x = static_cast<int>(std::floor(pos.x));
    const int baseline = static_cast<int>(std::floor(pos.y)) + font->native_metrics.ascent;
    // Keep the entire UTF-8 run and its pen advances intact when it crosses a
    // clip boundary. The renderer applies the clip while drawing the baseline.
    xapi_DrawTextEx(hdc, x, baseline, text, &font->style, litehtml_color(color), &native_clip);
}

void BrowserContainer::draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker &marker)
{
    litehtml::position pos(marker.pos.x, marker.pos.y, 10.0f, 10.0f);
    fill_rect(static_cast<HDLE>(hdc), pos, litehtml_color(marker.color));
}

void BrowserContainer::load_image(const char *src, const char *baseurl, bool redraw_on_ready)
{
    const std::string resolved = app_.resolve_url(src == nullptr ? std::string() : std::string(src),
                                                  baseurl == nullptr ? std::string() : std::string(baseurl));
    if (resolved.empty()) {
        return;
    }

    if (app_.load_image_resource(resolved) != nullptr && redraw_on_ready) {
        app_.invalidate_view();
    }
}

void BrowserContainer::get_image_size(const char *src, const char *baseurl, litehtml::size &sz)
{
    sz.width = 0;
    sz.height = 0;

    const std::string resolved = app_.resolve_url(src == nullptr ? std::string() : std::string(src),
                                                  baseurl == nullptr ? std::string() : std::string(baseurl));
    if (resolved.empty()) {
        return;
    }

    if (const auto *known = app_.find_image_size(resolved))
        if (known->width > 0 && known->height > 0)
        {
            sz.width = static_cast<float>(known->width);
            sz.height = static_cast<float>(known->height);
            return;
        }
    const BrowserDecodedImage *image = app_.load_image_resource(resolved);
    if (image == nullptr || image->empty()) {
        sz.width = 32;
        sz.height = 32;
        return;
    }

    sz.width = static_cast<float>(image->width);
    sz.height = static_cast<float>(image->height);
}

void BrowserContainer::draw_image(litehtml::uint_ptr hdc,
                                  const litehtml::background_layer &layer,
                                  const std::string &url,
                                  const std::string &base_url)
{
    const std::string resolved = app_.resolve_url(url, base_url);
    if (resolved.empty()) {
        return;
    }

    const BrowserDecodedImage *image = app_.load_image_resource(resolved);
    if (image == nullptr || image->empty()) {
        return;
    }

    litehtml::position dest = layer.origin_box;
    if (dest.width <= 0 || dest.height <= 0) {
        dest = layer.border_box;
    }

    litehtml::position clip = layer.clip_box.intersect(current_clip());
    blit_image(static_cast<HDLE>(hdc), *image, dest, clip);
}

void BrowserContainer::blit_image(HDLE hdc,
                                  const BrowserDecodedImage &image,
                                  const litehtml::position &dest,
                                  const litehtml::position &clip) const
{
    if (image.empty() || dest.width <= 0 || dest.height <= 0 || clip.width <= 0 || clip.height <= 0) {
        return;
    }

    const int left = std::max(static_cast<int>(std::floor(dest.x)), static_cast<int>(std::floor(clip.x)));
    const int top = std::max(static_cast<int>(std::floor(dest.y)), static_cast<int>(std::floor(clip.y)));
    const int right = std::min(static_cast<int>(std::ceil(dest.right())), static_cast<int>(std::ceil(clip.right())));
    const int bottom = std::min(static_cast<int>(std::ceil(dest.bottom())), static_cast<int>(std::ceil(clip.bottom())));
    if (right <= left || bottom <= top) {
        return;
    }

    const std::size_t draw_width = static_cast<std::size_t>(right - left);
    const std::size_t draw_height = static_cast<std::size_t>(bottom - top);
    std::vector<XCOLORA> buffer(draw_width * draw_height);
    bool translucent = false, visible = false;

    for (std::size_t y = 0; y < draw_height; ++y) {
        const float sample_y = ((static_cast<float>(top) + static_cast<float>(y) + 0.5f) - dest.y) / dest.height;
        int src_y = static_cast<int>(sample_y * static_cast<float>(image.height));
        src_y = std::clamp(src_y, 0, image.height - 1);

        for (std::size_t x = 0; x < draw_width; ++x) {
            const float sample_x = ((static_cast<float>(left) + static_cast<float>(x) + 0.5f) - dest.x) / dest.width;
            int src_x = static_cast<int>(sample_x * static_cast<float>(image.width));
            src_x = std::clamp(src_x, 0, image.width - 1);
            const XCOLORA pixel = image.pixels[static_cast<std::size_t>(src_y) * image.width + src_x];
            buffer[y * draw_width + x] = pixel;
            translucent = translucent || pixel.Alpha != 255;
            visible = visible || pixel.Alpha != 0;
        }
    }

    if (!visible) return;
    if (translucent)
    {
        std::vector<XCOLORA> background(buffer.size());
        xapi_ReadBufferA(hdc, left, top, draw_width, draw_height, background.data());
        for (std::size_t i = 0; i < buffer.size(); ++i)
            buffer[i] = browser_source_over(buffer[i], background[i]);
    }

    xapi_WriteBufferA(hdc,
                      static_cast<unsigned int>(left),
                      static_cast<unsigned int>(top),
                      static_cast<unsigned int>(draw_width),
                      static_cast<unsigned int>(draw_height),
                      buffer.data());
}

void BrowserContainer::draw_solid_fill(litehtml::uint_ptr hdc,
                                       const litehtml::background_layer &layer,
                                       const litehtml::web_color &color)
{
    fill_rect(static_cast<HDLE>(hdc), layer.border_box, litehtml_color(color));
}

void BrowserContainer::draw_linear_gradient(litehtml::uint_ptr hdc,
                                            const litehtml::background_layer &layer,
                                            const litehtml::background_layer::linear_gradient &gradient)
{
    BrowserGradientRaster raster;
    if (browser_raster_linear_gradient(layer, current_clip(), gradient, raster))
        blit_image(static_cast<HDLE>(hdc), raster.image, raster.bounds, raster.bounds);
}

void BrowserContainer::draw_radial_gradient(litehtml::uint_ptr hdc,
                                            const litehtml::background_layer &layer,
                                            const litehtml::background_layer::radial_gradient &gradient)
{
    BrowserGradientRaster raster;
    if (browser_raster_radial_gradient(layer, current_clip(), gradient, raster))
        blit_image(static_cast<HDLE>(hdc), raster.image, raster.bounds, raster.bounds);
}

void BrowserContainer::draw_conic_gradient(litehtml::uint_ptr hdc,
                                           const litehtml::background_layer &layer,
                                           const litehtml::background_layer::conic_gradient &gradient)
{
    BrowserGradientRaster raster;
    if (browser_raster_conic_gradient(layer, current_clip(), gradient, raster))
        blit_image(static_cast<HDLE>(hdc), raster.image, raster.bounds, raster.bounds);
}

void BrowserContainer::draw_borders(litehtml::uint_ptr hdc,
                                    const litehtml::borders &borders,
                                    const litehtml::position &draw_pos,
                                    bool)
{
    const HDLE handle = static_cast<HDLE>(hdc);
    stroke_border(handle, draw_pos.x, draw_pos.y, draw_pos.right(), draw_pos.y + borders.top.width, borders.top.width, litehtml_color(borders.top.color));
    stroke_border(handle, draw_pos.x, draw_pos.bottom() - borders.bottom.width, draw_pos.right(), draw_pos.bottom(), borders.bottom.width, litehtml_color(borders.bottom.color));
    stroke_border(handle, draw_pos.x, draw_pos.y, draw_pos.x + borders.left.width, draw_pos.bottom(), borders.left.width, litehtml_color(borders.left.color));
    stroke_border(handle, draw_pos.right() - borders.right.width, draw_pos.y, draw_pos.right(), draw_pos.bottom(), borders.right.width, litehtml_color(borders.right.color));
}

void BrowserContainer::set_caption(const char *caption)
{
    app_.set_title(caption == nullptr ? std::string() : std::string(caption));
}

void BrowserContainer::set_base_url(const char *base_url)
{
    app_.set_base_url(base_url == nullptr ? std::string() : std::string(base_url));
}

void BrowserContainer::on_anchor_click(const char *url, const litehtml::element::ptr &)
{
    if (url == nullptr || *url == '\0') {
        return;
    }

    app_.request_navigation(app_.resolve_url(url, app_.base_url()));
}

void BrowserContainer::import_css(litehtml::string &text, const litehtml::string &url, litehtml::string &baseurl)
{
    text.clear();

    if (url.empty()) {
        return;
    }

    std::string resolve_base = baseurl.empty() ? app_.base_url() : baseurl;
    if (resolve_base.empty()) {
        resolve_base = app_.base_url();
    }

    const std::string resolved = browser_resolve_url(resolve_base, url);
    if (++app_.css_import_count_ > 64)
    {
        app_.resource_failed_ = true;
        app_.network_notice_ = browser_tr("CSS 导入超过限制", "CSS import limit exceeded");
        return;
    }
    const BrowserFetchResult *response = app_.resource_response(resolved);
    if (!response)
    {
        app_.enqueue_resource(resolved, BrowserStylesheet, 0);
        return;
    }
    if (!response->ok || response->status_code < 200 || response->status_code >= 300) return;
    text = response->body;
    baseurl = response->final_url.empty() ? resolved : response->final_url;
}

void BrowserContainer::transform_text(litehtml::string &text, litehtml::text_transform tt)
{
    switch (tt) {
    case litehtml::text_transform_uppercase:
        for (char &ch : text) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        break;
    case litehtml::text_transform_lowercase:
        for (char &ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        break;
    case litehtml::text_transform_capitalize: {
        bool capitalize = true;
        for (char &ch : text) {
            if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
                capitalize = true;
            } else if (capitalize) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                capitalize = false;
            }
        }
        break;
    }
    default:
        break;
    }
}

void BrowserContainer::set_clip(const litehtml::position &pos, const litehtml::border_radiuses &)
{
    clip_stack_.push_back(pos.intersect(current_clip()));
}

void BrowserContainer::del_clip()
{
    if (!clip_stack_.empty()) {
        clip_stack_.pop_back();
    }
}

void BrowserContainer::get_media_features(litehtml::media_features &media) const
{
    media.type = litehtml::media_type_screen;
    media.width = viewport_.width;
    media.height = viewport_.height;
    media.device_width = viewport_.width;
    media.device_height = viewport_.height;
    media.color = 8;
    media.color_index = 256;
    media.monochrome = 0;
    media.resolution = 96.0f;
}

void BrowserContainer::get_language(litehtml::string &language, litehtml::string &culture) const
{
    if (g_browser_language == XJ380_LANGUAGE_EN_US) {
        language = "en";
        culture = "US";
    } else {
        language = "zh";
        culture = "CN";
    }
}

void BrowserApp::set_title(const std::string &title)
{
    page_title_ = title;
    update_window_title();
    invalidate_header();
}

void BrowserApp::set_status(const std::string &text)
{
    std::snprintf(status_, sizeof(status_), "%s", text.c_str());
    invalidate_header();
}

BrowserPreparedDocument BrowserApp::prepare_document_html(const BrowserFetchResult &response)
{
    BrowserPreparedDocument prepared;

    if (!looks_like_html(response)) {
        prepared.fallback_used = true;
        return prepared;
    }

    constexpr std::size_t kMaxHtmlBytes = 2 * 1024 * 1024;
    if (response.body.size() > kMaxHtmlBytes) { prepared.fallback_used = true; return prepared; }
    std::string html = response.body;
    const auto discovered = browser_html_scripts(html);
    const std::string page_base = base_url_;
    if (discovered.has_base)
    {
        base_url_ = browser_resolve_url(page_base, discovered.base_href);
        base_url_set_ = true;
    }
    // Discover executable scripts without treating comments, textarea or inert
    // template contents as programs. Keep script nodes in the DOM for queries;
    // evaluation begins only after the native document and controls exist.
    deferred_scripts_.clear();
    for (const auto &script : discovered.scripts)
    {
        if (!is_executable_script_type(script.type) || (script.nomodule && !is_module_script_type(script.type))) continue;
        if (deferred_scripts_.size() >= 128)
        {
            log_line("Script discovery budget exceeded");
            break;
        }
        DeferredScript program;
        program.base = discovered.has_base && discovered.base_begin < script.begin ? base_url_ : page_base;
        program.source = html.substr(script.content_begin, script.content_end - script.content_begin);
        program.module = is_module_script_type(script.type);
        program.integrity = script.integrity;
        if (!script.src.empty()) program.url = resolve_url(script.src, program.base);
        program.async = (program.module || !script.src.empty()) && script.async;
        program.deferred = !program.async && (program.module || (!script.src.empty() && script.defer));
        if ((program.deferred || program.async) && !program.url.empty() &&
            (!program.module || (program.integrity.empty() && browser_same_origin(page_base, program.url))))
            enqueue_resource(program.url, BrowserScript, 1);
        deferred_scripts_.push_back(std::move(program));
        ++prepared.script_blocks;
    }
    if (find_case_insensitive(html, "<html") == std::string::npos &&
        find_case_insensitive(html, "<body") == std::string::npos) {
        html = build_fallback_document(browser_tr("HTML 片段", "HTML Fragment"),
                                       browser_tr("响应不是完整 HTML 文档，已使用简化包装进行渲染。",
                                                  "The response is not a full HTML document, so it was rendered in a simplified wrapper."),
                                       html);
    }

    prepared.html = std::move(html);
    return prepared;
}

std::string BrowserApp::resolve_url(const std::string &url, const std::string &base_url) const
{
    const std::string trimmed = trim_copy(url);
    if (trimmed.empty()) {
        return std::string();
    }
    if (lower_copy(trimmed).rfind("data:", 0) == 0) {
        return trimmed;
    }

    std::string resolve_base = trim_copy(base_url);
    if (resolve_base.empty()) {
        resolve_base = base_url_.empty() ? std::string() : base_url_;
    }

    const std::string final_url = browser_resolve_url(resolve_base, trimmed);
    const std::string scheme = lower_copy(final_url.substr(0, final_url.find(':')));
    if (scheme != "http" && scheme != "https") {
        return std::string();
    }
    return final_url;
}

static unsigned long long resource_expiration(const BrowserFetchResult &response)
{
    if (response.cache_vary_star) return 0;
    const std::string control = lower_copy(response.cache_control);
    if (control.find("no-store") != std::string::npos || control.find("no-cache") != std::string::npos) return 0;
    std::size_t pos = 0;
    while (pos < control.size())
    {
        const auto end = control.find(',', pos);
        const auto field = trim_copy(control.substr(pos, end == std::string::npos ? end : end - pos));
        if (field.compare(0, 8, "max-age=") == 0)
        {
            std::string value = trim_copy(field.substr(8));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            unsigned long long seconds = 0;
            if (value.empty()) return 0;
            for (char ch : value)
            {
                if (ch < '0' || ch > '9') return 0;
                seconds = std::min(3600ULL, seconds * 10 + static_cast<unsigned>(ch - '0'));
            }
            const auto now = browser_http_now_ms();
            return now == ~0ULL || response.cache_age >= seconds ? 0 :
                now + (seconds - response.cache_age) * 1000;
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return 0;
}

const BrowserFetchResult *BrowserApp::find_cached_resource(const std::string &url) const
{
    for (const CachedResource &entry : resource_cache_) {
        const unsigned int serial = loading_ ? loading_serial_ : document_serial_;
        if (entry.url == url && entry.cookie_generation == browser_cookie_generation() && (entry.serial == serial ||
            (!force_reload_ && entry.expires > browser_http_now_ms()))) {
            return &entry.result;
        }
    }
    return nullptr;
}

void BrowserApp::store_cached_resource(const std::string &url, const BrowserFetchResult &result)
{
    constexpr std::size_t kMaxCacheBytes = 16 * 1024 * 1024;
    CachedResource entry;
    entry.url = url;
    entry.serial = navigation_serial_;
    entry.expires = resource_expiration(result);
    entry.cookie_generation = browser_cookie_generation();
    const bool cacheable = result.body.size() <= kMaxCacheBytes;
    if (cacheable) entry.result = result;

    // Replacements share the insertion budget. Copy first so the input may
    // safely refer to an existing entry, then discard its previous bytes.
    resource_cache_.erase(std::remove_if(resource_cache_.begin(), resource_cache_.end(),
                                        [&](const CachedResource &item) { return item.url == entry.url; }),
                          resource_cache_.end());
    if (!cacheable) return;
    std::size_t cached_bytes = entry.result.body.size();
    for (const auto &item : resource_cache_) cached_bytes += item.result.body.size();
    while (!resource_cache_.empty() && cached_bytes > kMaxCacheBytes)
    {
        cached_bytes -= resource_cache_.front().result.body.size();
        resource_cache_.erase(resource_cache_.begin());
    }
    constexpr std::size_t kMaxCachedResources = 24;
    if (resource_cache_.size() >= kMaxCachedResources) {
        resource_cache_.erase(resource_cache_.begin());
    }

    resource_cache_.push_back(std::move(entry));
}

const BrowserApp::CachedImage *BrowserApp::find_cached_image(const std::string &url) const
{
    for (const CachedImage &entry : image_cache_) {
        const unsigned int serial = loading_ ? loading_serial_ : document_serial_;
        if (entry.url == url && entry.cookie_generation == browser_cookie_generation() && (entry.serial == serial ||
            (!force_reload_ && entry.expires > browser_http_now_ms()))) {
            return &entry;
        }
    }
    return nullptr;
}

const BrowserApp::ImageSize *BrowserApp::find_image_size(const std::string &url) const
{
    for (const auto &item : image_sizes_) if (item.url == url) return &item;
    return nullptr;
}

void BrowserApp::update_retry_status(const BrowserFetchResult &failure, const BrowserRetryStatus &retry)
{
    const std::string reason = browser_fetch_error_message(failure.error_code, failure.tls_failure,
                                                          g_browser_language == XJ380_LANGUAGE_EN_US);
    char notice[512];
    if (retry.phase == BrowserRetryStatus::Phase::Waiting)
    {
        std::snprintf(notice, sizeof(notice),
                      browser_tr("%s；%u 秒后重试（第 %u/%u 次尝试）",
                                 "%s; retrying in %u s (attempt %u/%u)"),
                      reason.c_str(), (retry.remaining_ms + 999) / 1000, retry.attempt, retry.max_attempts);
        set_status(browser_tr("等待重试", "Retry pending"));
    }
    else
    {
        std::snprintf(notice, sizeof(notice), browser_tr("%s；正在重试（第 %u/%u 次尝试）",
                                                       "%s; retrying (attempt %u/%u)"),
                      reason.c_str(), retry.attempt, retry.max_attempts);
        set_status(browser_tr("正在重试", "Retrying"));
        log_line(std::string(notice) + " " + failure.error);
    }
    network_notice_ = notice;
    // Dismissal lasts for this navigation, including subsequent countdown ticks.
    // Fetch callbacks can run during parsing/drawing. Only repaint the
    // toolbar here; document operations remain on the outer call stack.
    render_header();
    render_overlays();
    // Flush recorded damage, which includes the window decoration offset.
    // RefreshPartWindow uses sheet coordinates and would miss the last row.
    xapi_RefreshWindow(window_);
}

BrowserFetchResult BrowserApp::fetch_resource(const std::string &url, bool navigation)
{
    if (const BrowserFetchResult *cached = find_cached_resource(url)) {
        return *cached;
    }

    BrowserFetchResult inline_result;
    if (decode_data_url(url, inline_result)) {
        if (inline_result.ok) {
            store_cached_resource(url, inline_result);
        }
        return inline_result;
    }

    if (cancelled_)
    {
        inline_result.cancelled = true;
        inline_result.error_code = -125;
        inline_result.error = inline_result.error_summary = browser_fetch_error_message(-125, 0);
        return inline_result;
    }
    struct FetchScope
    {
        unsigned int &count;
        bool active = true;
        explicit FetchScope(unsigned int &value) : count(value) { ++count; }
        void finish()
        {
            if (active) --count;
            active = false;
        }
        ~FetchScope() { finish(); }
    } fetch_scope {active_fetches_};
    struct FetchContext
    {
        BrowserApp *app;
        unsigned serial;
        std::string previous_status;
        bool retrying = false;
    } context {this, navigation_serial_, status_};
    BrowserFetchOptions options;
    options.cookie_site = !navigation && loading_ ? loading_document_url_ : current_url_;
    options.top_level_navigation = navigation;
    options.keep_running = [](void *value) {
        auto &context = *static_cast<FetchContext *>(value);
        return context.app->continue_fetch() && context.serial == context.app->navigation_serial_;
    };
    options.retry_status = [](const BrowserFetchResult &failure, const BrowserRetryStatus &retry, void *value) {
        auto &context = *static_cast<FetchContext *>(value);
        auto &app = *context.app;
        if (context.serial != app.navigation_serial_ || app.cancelled_) return;
        context.retrying = true;
        app.update_retry_status(failure, retry);
    };
    options.user_data = &context;
    BrowserFetchResult result = browser_fetch_url(url, options);
    fetch_scope.finish();
    if (context.serial == navigation_serial_ && !result.cancelled)
    {
        if (context.retrying) set_status(context.previous_status);
        if (result.ok && context.retrying)
        {
            network_notice_.clear();
            invalidate_header();
        }
        else if (!result.ok)
        {
            network_notice_ = browser_tr("资源加载失败：", "Resource failed: ") +
                browser_fetch_error_message(result.error_code, result.tls_failure,
                                            g_browser_language == XJ380_LANGUAGE_EN_US);
            invalidate_header();
        }
        if (context.retrying || !result.ok)
        {
            render_header();
            xapi_RefreshWindow(window_);
        }
    }
    else if (cancelled_)
    {
        render_header();
        xapi_RefreshWindow(window_);
    }
    if (result.ok && result.status_code >= 200 && result.status_code < 300)
    {
        const std::string cache_key = result.final_url.empty() ? url : result.final_url;
        store_cached_resource(cache_key, result);
        if (cache_key != url) {
            store_cached_resource(url, result);
        }
    }
    return result;
}

void BrowserApp::reset_resource_jobs()
{
    resource_serial_ = navigation_serial_;
    image_decode_queue_.clear();
    rebuild_pending_ = false;
    resource_error_.clear();
    retry_notice_url_.clear();
    BrowserFetchOptions options;
    options.cookie_site = loading_ ? loading_document_url_ : current_url_;
    options.top_level_navigation = false;
    options.user_data = this;
    options.keep_running = [](void *value) {
        const auto &app = *static_cast<BrowserApp *>(value);
        return !app.cancelled_ && app.resource_serial_ == app.navigation_serial_;
    };
    options.retry_status = [](const BrowserFetchResult &failure, const BrowserRetryStatus &retry, void *value) {
        auto &app = *static_cast<BrowserApp *>(value);
        if (!app.cancelled_ && app.resource_serial_ == app.navigation_serial_)
        {
            app.retry_notice_url_ = failure.final_url;
            app.update_retry_status(failure, retry);
        }
    };
    resource_jobs_.reset(options, decode_data_url);
}

bool BrowserApp::resources_pending() const
{
    if (!dynamic_scripts_.empty() || !module_transfers_.empty() || !script_links_.empty() ||
        !script_navigation_.empty() || script_submission_ || script_history_go_pending_ || script_scroll_pending_) return true;
    if (script_runtime_ && script_runtime_->module_work_pending()) return true;
    if (resource_serial_ != navigation_serial_) return false;
    if (resource_jobs_.pending() || !image_decode_queue_.empty() || rebuild_pending_) return true;
    for (const auto &script : deferred_scripts_) if (!script.applied) return true;
    return false;
}

bool BrowserApp::enqueue_resource(const std::string &url, unsigned int kind, unsigned int priority)
{
    if (url.empty() || cancelled_ || resource_serial_ != navigation_serial_) return false;
    if (const auto *cached = find_cached_resource(url)) return resource_jobs_.provide(url, *cached, kind, priority);
    if (kind & BrowserImage) resource_jobs_.refetch_image(url);
    if (resource_jobs_.enqueue(url, kind, priority)) return true;
    resource_failed_ = true;
    network_notice_ = browser_tr("页面资源超过数量限制", "Page resource limit exceeded");
    invalidate_header();
    return false;
}

const BrowserFetchResult *BrowserApp::resource_response(const std::string &url) const
{
    if (const auto *cached = find_cached_resource(url)) return cached;
    if (resource_serial_ == navigation_serial_)
        if (const auto *job = resource_jobs_.find(url))
            if (job->finished) return &job->response;
    return nullptr;
}

void BrowserApp::prioritize_visible_images()
{
    if (!document_ || !document_->root()) return;
    auto visit = [&](auto &&self, const litehtml::element::ptr &element) -> void {
        if (!element) return;
        const auto placement = element->get_placement();
        if (placement.bottom() >= scroll_y_ && placement.y <= scroll_y_ + content_height() * 2)
        {
            const char *src = element->get_attr("src");
            if (src)
            {
                const auto url = resolve_url(src, base_url_);
                if (const auto *job = resource_jobs_.find(url))
                    if (job->kind & BrowserImage) resource_jobs_.enqueue(url, BrowserImage, 2);
            }
        }
        for (const auto &child : element->children()) self(self, child);
    };
    visit(visit, document_->root());
}

bool BrowserApp::wait_stylesheets(unsigned int serial)
{
    while (resource_jobs_.pending(BrowserStylesheet))
    {
        if (!continue_fetch() || serial != navigation_serial_) return false;
        pump_resources(false);
        xapi_Sleep(1);
    }
    return continue_fetch() && serial == navigation_serial_ && !cancelled_;
}

void BrowserApp::rebuild_resource_document(bool recreate)
{
    if (!document_ || prepared_html_.empty()) return;
    const int old_scroll = scroll_y_, old_match = find_match_index_;
    std::size_t anchor = 0;
    int delta = 0;
    bool anchored = false;
    for (const auto &run : text_runs_)
        if (run.box.bottom() > old_scroll)
        {
            anchor = run.offset;
            delta = old_scroll - static_cast<int>(run.box.y);
            anchored = true;
            break;
        }
    const bool page_focused = input_mode_ == BrowserInputMode::Page;
    const std::string focus_key = page_focused && focused_form_ >= 0 &&
        focused_form_ < static_cast<int>(forms_.controls.size()) ? forms_.controls[focused_form_].key : "";
    const int old_cursor = page_cursor_, old_anchor = page_anchor_;
    if (recreate)
    {
        css_import_count_ = 0;
        BrowserParserScope parser;
        const auto old_controls = forms_.controls;
        document_->rebuild();
        forms_.collect(document_->root());
        browser_preserve_form_defaults(old_controls, forms_);
        ++form_generation_;
        if (page_focused) { focused_form_ = -1; input_mode_ = BrowserInputMode::None; }
    }
    container_.set_viewport(content_width(), content_height());
    document_->render(static_cast<float>(content_width()));
    page_text_.clear();
    text_runs_.clear();
    collect_text(document_->root());
    update_find_matches();
    if (old_match >= 0 && old_match < find_match_count_) find_match_index_ = old_match;
    scroll_y_ = old_scroll;
    if (anchored)
        for (const auto &run : text_runs_)
            if (run.offset >= anchor)
            {
                scroll_y_ = static_cast<int>(run.box.y) + delta;
                break;
            }
    clamp_scroll();
    if (recreate && page_focused)
    {
        focused_form_ = -1;
        input_mode_ = BrowserInputMode::None;
        for (std::size_t i = 0; i < forms_.controls.size(); ++i)
            if (!focus_key.empty() && forms_.controls[i].key == focus_key)
            {
                focus_form(i);
                page_cursor_ = std::clamp(old_cursor, 0, page_len_);
                page_anchor_ = std::clamp(old_anchor, 0, page_len_);
                while (page_cursor_ > 0 && (static_cast<unsigned char>(page_input_[page_cursor_]) & 0xc0) == 0x80)
                    --page_cursor_;
                while (page_anchor_ > 0 && (static_cast<unsigned char>(page_input_[page_anchor_]) & 0xc0) == 0x80)
                    --page_anchor_;
                break;
            }
        update_ime_context();
    }
    prioritize_visible_images();
    invalidate_content();
}

void BrowserApp::pump_resources(bool update_document)
{
    if (processing_resources_) return;
    if (cancelled_ || resource_serial_ != navigation_serial_)
    {
        if (update_document && !loading_ && !load_pending_) pump_scripts();
        return;
    }
    processing_resources_ = true;
    struct PumpScope { bool &flag; ~PumpScope() { flag = false; } } scope {processing_resources_};
    const auto serial = navigation_serial_;
    const bool was_pending = resources_pending();
    bool resized = false, recreate = false;
    for (const auto *job : resource_jobs_.poll(active_fetches_))
    {
        if (serial != navigation_serial_ || cancelled_) return;
        const auto &response = job->response;
        if (retry_notice_url_ == job->url || (!response.final_url.empty() && retry_notice_url_ == response.final_url))
        {
            retry_notice_url_.clear();
            network_notice_ = resource_error_;
            set_status(browser_tr("正在加载资源", "Loading resources"));
        }
        if (response.ok && response.status_code >= 200 && response.status_code < 300)
        {
            store_cached_resource(job->url, response);
            if (!response.final_url.empty() && response.final_url != job->url)
                store_cached_resource(response.final_url, response);
        }
        else if (!response.cancelled)
        {
            resource_failed_ = true;
            network_notice_ = browser_tr("资源加载失败：", "Resource failed: ") +
                browser_fetch_error_message(response.error_code, response.tls_failure,
                                            g_browser_language == XJ380_LANGUAGE_EN_US);
            if (response.ok) network_notice_ = "HTTP " + std::to_string(response.status_code) + " " + job->url;
            resource_error_ = network_notice_;
            notice_dismissed_ = false;
            log_line(network_notice_ + " " + response.error);
            invalidate_header();
        }
        if (job->kind & BrowserImage) image_decode_queue_.push_back(job->url);
        if ((job->kind & BrowserStylesheet) && document_ && !loading_) recreate = true;
    }
    if (!update_document || loading_) return;
    // Decode at most one image per turn. No engine callback can enter here.
    if (!image_decode_queue_.empty())
    {
        const auto url = std::move(image_decode_queue_.front());
        image_decode_queue_.erase(image_decode_queue_.begin());
        const auto *previous = find_image_size(url);
        const int width = previous ? previous->width : 0, height = previous ? previous->height : 0;
        if (const auto *job = resource_jobs_.find(url)) decode_image_resource(url, job->response);
        resource_jobs_.release_body(url);
        const auto *current = find_image_size(url);
        resized = current && current->width > 0 && (current->width != width || current->height != height);
        invalidate_content();
    }
    if (rebuild_pending_ && !resource_jobs_.pending(BrowserStylesheet)) { recreate = true; rebuild_pending_ = false; }
    if ((resized || recreate) && document_) rebuild_resource_document(recreate);
    pump_scripts();
    if (serial != navigation_serial_ || cancelled_) return;
    if (was_pending && !resources_pending())
    {
        if (!resource_failed_) network_notice_.clear();
        set_status(resource_failed_ ? browser_tr("部分资源失败", "Partial load") : browser_tr("已加载", "Loaded"));
    }
}

const BrowserDecodedImage *BrowserApp::load_image_resource(const std::string &url)
{
    if (url.empty()) return nullptr;
    if (const auto *known = find_image_size(url))
        if (known->failed_serial == navigation_serial_) return nullptr;
    if (const auto *cached = find_cached_image(url)) return cached->failed ? nullptr : &cached->image;
    // Small self-contained vectors need no network transfer. Paint them through
    // the same bounded decoded-image cache even when the transfer queue is full.
    // Larger data images retain the normal cooperative decoding path.
    constexpr std::size_t max_inline_vector_url = 512 * 1024;
    if (url.size() <= max_inline_vector_url && lower_copy(url.substr(0, 19)) == "data:image/svg+xml,")
    {
        BrowserFetchResult response;
        if (decode_data_url(url, response))
        {
            decode_image_resource(url, response);
            const auto *cached = find_cached_image(url);
            return cached && !cached->failed ? &cached->image : nullptr;
        }
    }
    enqueue_resource(url, BrowserImage);
    // Keep a previous decoded image visible while a reload validates its bytes.
    for (const auto &entry : image_cache_)
        if (entry.url == url && !entry.failed && entry.cookie_generation == browser_cookie_generation()) return &entry.image;
    return nullptr;
}

void BrowserApp::decode_image_resource(const std::string &url, const BrowserFetchResult &response)
{
    auto remember = [&](int width, int height, bool failed) {
        auto item = std::find_if(image_sizes_.begin(), image_sizes_.end(), [&](const ImageSize &value) {
            return value.url == url;
        });
        if (item == image_sizes_.end())
        {
            if (image_sizes_.size() >= 128) image_sizes_.erase(image_sizes_.begin());
            image_sizes_.push_back({url, width, height, failed ? navigation_serial_ : ~0U});
        }
        else
        {
            item->width = width; item->height = height;
            item->failed_serial = failed ? navigation_serial_ : ~0U;
        }
    };
    CachedImage entry;
    entry.url = url;
    entry.serial = navigation_serial_;
    entry.expires = resource_expiration(response);
    entry.cookie_generation = browser_cookie_generation();
    auto previous = std::find_if(image_cache_.begin(), image_cache_.end(), [&](const CachedImage &item) {
        return item.url == url;
    });
    if (!response.ok || response.status_code < 200 || response.status_code >= 300)
    {
        if (previous != image_cache_.end() && !previous->failed &&
            previous->cookie_generation == browser_cookie_generation()) return;
        remember(0, 0, true);
        entry.failed = true;
        if (previous != image_cache_.end()) image_cache_.erase(previous);
        if (image_cache_.size() >= 64) image_cache_.erase(image_cache_.begin());
        image_cache_.push_back(std::move(entry));
        return;
    }
    if (previous != image_cache_.end()) image_cache_.erase(previous);
    if (image_cache_.size() >= 64) image_cache_.erase(image_cache_.begin());
    std::string decode_error;
    if (!browser_decode_image(response.body, response.content_type, entry.image, decode_error)) {
        remember(0, 0, true);
        entry.failed = true;
        image_cache_.push_back(std::move(entry));
        log_line("图片解码失败：" + shorten_url_for_log(url) + " 错误=" + decode_error +
                 " 类型=" + (response.content_type.empty() ? std::string("-") : response.content_type));
        return;
    }

    remember(entry.image.width, entry.image.height, false);
    char info[256];
    std::snprintf(info,
                  sizeof(info),
                  "图片就绪：%s %dx%d 类型=%s",
                  shorten_url_for_log(url).c_str(),
                  entry.image.width,
                  entry.image.height,
                  response.content_type.empty() ? "-" : response.content_type.c_str());
    log_line(info);

    constexpr std::size_t max_pixels = 16 * 1024 * 1024;
    std::size_t cached_pixels = entry.image.pixels.size();
    for (const auto &item : image_cache_) cached_pixels += item.image.pixels.size();
    while (!image_cache_.empty() && cached_pixels > max_pixels)
    {
        cached_pixels -= image_cache_.front().image.pixels.size();
        image_cache_.erase(image_cache_.begin());
    }
    image_cache_.push_back(std::move(entry));

}

void BrowserApp::request_navigation(const std::string &url)
{
    navigate_to(url, BrowserNavReason::Normal);
}

void BrowserApp::navigate_to(const std::string &url, BrowserNavReason reason)
{
    // Navigation/reload is a new user action; DNS and the TLS CA file may be
    // retried after an earlier refusal. Resource fetches do not reset this state.
    xj380_reset_permission_prompts();
    if (url.empty()) {
        return;
    }

    const std::size_t fragment = url.find('#');
    const std::string page = url.substr(0, fragment);
    const std::string current_page = current_url_.substr(0, current_url_.find('#'));
    if (!loading_ && (reason == BrowserNavReason::Normal || reason == BrowserNavReason::Replace) &&
        fragment != std::string::npos && page == current_page)
    {
        current_url_ = url;
        scroll_to_fragment(url);
        record_history(url, reason == BrowserNavReason::Replace);
        std::snprintf(url_input_, sizeof(url_input_), "%s", url.c_str());
        input_len_ = input_cursor_ = input_anchor_ = static_cast<int>(std::strlen(url_input_));
        invalidate_all();
        return;
    }
    input_event_epoch_.fetch_add(1, std::memory_order_release);
    save_history_state();
    close_chrome_popup();
    resource_jobs_.cancel();
    image_decode_queue_.clear();
    failure_details_open_ = false; notice_dismissed_ = false;
    input_mode_ = BrowserInputMode::None;
    update_ime_context();
    ++navigation_serial_;
    cancelled_ = false;
    network_notice_.clear();
    pending_nav_reason_ = reason;
    suppress_history_record_ = reason != BrowserNavReason::Normal;
    pending_url_ = url;
    std::snprintf(url_input_, sizeof(url_input_), "%s", url.c_str());
    input_len_ = static_cast<int>(std::strlen(url_input_));
    input_cursor_ = input_anchor_ = input_len_;
    load_pending_ = true;
    invalidate_header();
}

void BrowserApp::record_history(const std::string &url, bool replace, bool force_entry)
{
    if (url.empty()) {
        return;
    }

    if (replace && history_index_ >= 0 && history_index_ < static_cast<int>(history_.size()))
    {
        history_[history_index_] = url;
        history_states_.resize(history_.size());
        if (!force_entry || history_states_[history_index_].document != document_serial_)
            history_states_[history_index_] = HistoryState {};
        history_states_[history_index_].document = document_serial_;
        current_url_ = url;
        return;
    }
    if (!force_entry && history_index_ >= 0 &&
        history_index_ < static_cast<int>(history_.size()) &&
        history_[history_index_] == url) {
        current_url_ = url;
        return;
    }

    if (history_index_ + 1 < static_cast<int>(history_.size())) {
        history_.erase(history_.begin() + history_index_ + 1, history_.end());
        history_states_.resize(history_.size());
    }

    history_.push_back(url);
    history_states_.resize(history_.size());
    history_index_ = static_cast<int>(history_.size()) - 1;
    history_states_[history_index_].document = document_serial_;
    current_url_ = url;
}

void BrowserApp::go_history(int delta)
{
    const auto next = static_cast<long long>(history_index_) + delta;
    if (next < 0 || next >= static_cast<long long>(history_.size())) {
        return;
    }
    const int next_index = static_cast<int>(next);
    if (!loading_ && !load_pending_ && document_ && script_runtime_ && document_serial_ &&
        next_index < static_cast<int>(history_states_.size()) &&
        history_states_[next_index].document == document_serial_)
    {
        save_history_state();
        history_index_ = next_index;
        current_url_ = history_[next_index];
        if (!base_url_set_) base_url_ = current_url_;
        std::snprintf(url_input_, sizeof(url_input_), "%s", current_url_.c_str());
        input_len_ = input_cursor_ = input_anchor_ = std::strlen(url_input_);
        scroll_y_ = history_states_[next_index].scroll;
        clamp_scroll();
        {
            BrowserScriptScope scope(processing_scripts_);
            script_runtime_->history_changed(current_url_, history_states_[next_index].script_state);
            pump_module_requests();
        }
        sync_script_document();
        flush_script_submission();
        invalidate_all();
        return;
    }
    pending_history_index_ = next_index;
    navigate_to(history_[next_index], BrowserNavReason::History);
}

void BrowserApp::reload_current()
{
    const std::string target = current_url_.empty() ? trim_copy(std::string(url_input_)) : current_url_;
    if (target.empty()) {
        return;
    }
    navigate_to(target, BrowserNavReason::Reload);
}

void BrowserApp::stop_loading()
{
    if (!loading_ && !load_pending_ && !active_fetches_ && !resources_pending()) return;
    cancelled_ = true;
    module_fetch_cancelled_ = true;
    resource_jobs_.cancel();
    image_decode_queue_.clear();
    browser_http_close_idle();
    if (script_runtime_)
    {
        for (const auto &node : dynamic_scripts_) script_runtime_->release_node(node);
        for (auto &program : deferred_scripts_)
            if (!program.applied)
            {
                script_runtime_->release_node(program.node);
                program.applied = true;
            }
    }
    dynamic_scripts_.clear();
    script_navigation_.clear();
    script_submission_.reset();
    load_pending_ = false;
    pending_url_.clear();
    ++navigation_serial_;
    network_notice_.clear();
    invalidate_content();
    set_status(browser_tr("已停止", "Stopped"));
}

int BrowserApp::scroll_content_height() const
{
    if (document_) {
        return static_cast<int>(document_->height());
    }
    return text_content_height_;
}

void BrowserApp::clamp_scroll()
{
    const int max_scroll = std::max(0, scroll_content_height() - content_height());
    scroll_y_ = std::clamp(scroll_y_, 0, max_scroll);
}

void BrowserApp::draw_scrollbar()
{
    if (window_height_ <= content_y() || content_width() >= window_width_) return;
    const auto theme = chrome_theme();
    const browser_ui::Rect area {scrollbar_x(), content_y(), window_width_ - scrollbar_x(), content_height()};
    chrome_canvas_.begin(area, theme.surface);
    const int total = scroll_content_height();
    if (total > content_height() && area.h > 8)
    {
        const int track = area.h - 8;
        const int thumb = std::clamp(static_cast<int>(static_cast<long long>(area.h) * track / total),
                                     std::min(BROWSER_SCROLLBAR_MIN_THUMB, track), track);
        const int y = area.y + 4 + static_cast<int>(static_cast<long long>(track - thumb) * scroll_y_ /
                                                  std::max(1, total - area.h));
        chrome_canvas_.round({area.x + 3, y, std::max(0, area.w - 6), thumb}, 4, theme.secondary);
    }
    chrome_canvas_.present(window_);
}

bool BrowserApp::handle_scrollbar_click(int x, int y)
{
    if (x < scrollbar_x() || x >= scrollbar_x() + BROWSER_SCROLLBAR_WIDTH ||
        y < content_y() || y >= content_y() + content_height()) {
        return false;
    }

    const int total_height = scroll_content_height();
    if (total_height <= content_height()) {
        scroll_y_ = 0;
        refresh_content_view();
        return true;
    }

    const int track_height = std::max(1, content_height() - 8);
    const int thumb_height = std::clamp(
        static_cast<int>(static_cast<long long>(content_height()) * track_height / total_height),
        std::min(BROWSER_SCROLLBAR_MIN_THUMB, track_height), track_height);
    const int max_scroll = total_height - content_height();
    const int travel = std::max(1, track_height - thumb_height);
    const int track_click = std::clamp(y - content_y() - 4 - thumb_height / 2, 0, travel);
    scroll_y_ = static_cast<int>(static_cast<long long>(track_click) * max_scroll / travel);
    clamp_scroll();
    refresh_content_view();
    return true;
}

void BrowserApp::prepare_text_view(const BrowserFetchResult &response)
{
    std::string text = looks_like_html(response) ? html_to_text(response.body) : response.body;
    text = trim_copy(text);
    if (text.empty()) {
        text = browser_tr("页面已加载，但没有生成可见文本内容。",
                          "The page loaded, but did not produce visible text content.");
    }

    page_text_ = text;
    text_lines_ = wrap_text_for_width(text, std::max(120, content_width() - 24));
    text_content_height_ = static_cast<int>(text_lines_.size()) * BROWSER_TEXT_LINE_HEIGHT + 24;
    update_find_matches();
}

bool BrowserApp::hit_content_area(int x, int y) const
{
    return x >= content_x() && x < content_x() + content_width() &&
           y >= content_y() && y < content_y() + content_height();
}

bool BrowserApp::hit_url_input(int x, int y) const
{
    return chrome_layout().address_text.contains(x, y);
}

bool BrowserApp::hit_find_input(int x, int y) const
{
    return find_visible_ && !page_failed_ && chrome_layout().find_text.contains(x, y);
}

std::vector<BrowserButton> BrowserApp::chrome_buttons() const
{
    using browser_ui::Icon;
    const auto l = chrome_layout();
    std::vector<BrowserButton> buttons {
        {BrowserCommand::Back, l.back, Icon::Back},
        {BrowserCommand::Forward, l.forward, Icon::Forward},
        {browser_busy() ? BrowserCommand::Stop : BrowserCommand::Reload, l.reload,
         browser_busy() ? Icon::Stop : Icon::Reload},
        {BrowserCommand::FocusFind, l.search, Icon::Search},
        {BrowserCommand::Menu, l.menu, Icon::Menu},
        {BrowserCommand::SiteInfo, l.site, Icon::Site}
    };
    if (notice_visible())
    {
        const bool partial = resource_failed_ && !browser_busy();
        buttons.push_back({partial ? BrowserCommand::ResourceInfo : BrowserCommand::RetryNow,
                           l.banner_action, partial ? Icon::Info : Icon::Reload,
                           partial ? browser_tr("查看详情", "Details") :
                           browser_busy() ? browser_tr("立即重试", "Retry now") : browser_tr("重新加载", "Reload")});
        buttons.push_back({browser_busy() ? BrowserCommand::Stop : BrowserCommand::DismissNotice,
                           l.banner_close, Icon::Stop});
    }
    if (page_failed_)
    {
        buttons.push_back({BrowserCommand::Reload, l.error_reload, Icon::Reload, browser_tr("重新加载", "Reload")});
        buttons.push_back({BrowserCommand::Back, l.error_back, Icon::Back, browser_tr("返回上一页", "Go back")});
        buttons.push_back({BrowserCommand::ErrorDetails, l.error_details, Icon::Down,
                           browser_tr("查看详细信息", "Show details")});
    }
    else if (!document_ && text_lines_.empty() && !loading_)
    {
        const int width = std::min(180, std::max(0, window_width_ - 48));
        buttons.push_back({BrowserCommand::FocusUrl,
                           {(window_width_ - width) / 2, content_y() + 270, width, 38},
                           Icon::Arrow, browser_tr("输入网址", "Enter an address")});
    }
    if (find_visible_ && !page_failed_)
    {
        buttons.push_back({BrowserCommand::FindPrev, l.find_prev, Icon::Up});
        buttons.push_back({BrowserCommand::FindNext, l.find_next, Icon::Down});
        buttons.push_back({BrowserCommand::CloseFind, l.find_close, Icon::Stop});
    }
    if (popup_ == BrowserPopup::Menu)
    {
        const BrowserCommand commands[] = {BrowserCommand::FocusFind, BrowserCommand::FocusUrl,
            BrowserCommand::Reload, BrowserCommand::SiteInfo, BrowserCommand::ToggleTheme};
        const Icon icons[] = {Icon::Search, Icon::Arrow, Icon::Reload, Icon::Site,
                              dark_theme_ ? Icon::Sun : Icon::Moon};
        const char *labels[] = {browser_tr("在页面中查找   Ctrl+F", "Find in page   Ctrl+F"),
            browser_tr("输入网址       Ctrl+L", "Open address   Ctrl+L"),
            browser_tr("重新加载       F5", "Reload         F5"),
            browser_tr("连接信息", "Connection information"),
            dark_theme_ ? browser_tr("切换浅色外观", "Use light appearance") : browser_tr("切换深色外观", "Use dark appearance")};
        for (int i = 0; i < 5; ++i)
            buttons.push_back({commands[i], {l.popup.x + 6, l.popup.y + 6 + i * 38, l.popup.w - 12, 38},
                               icons[i], labels[i]});
    }
    return buttons;
}

inline browser_ui::Rect BrowserApp::control_rect(BrowserCommand command) const
{
    for (const auto &button : chrome_buttons())
        if (button.command == command) return button.rect.intersect(chrome_layout().bounds);
    return {};
}

BrowserCommand BrowserApp::command_at(int x, int y) const
{
    const auto buttons = chrome_buttons();
    for (auto i = buttons.rbegin(); i != buttons.rend(); ++i)
        if (i->rect.intersect(chrome_layout().bounds).contains(x, y)) return i->command;
    return BrowserCommand::None;
}

bool BrowserApp::command_enabled(BrowserCommand command) const
{
    switch (command)
    {
    case BrowserCommand::Back: return history_index_ > 0;
    case BrowserCommand::Forward:
        return history_index_ >= 0 && history_index_ + 1 < static_cast<int>(history_.size());
    case BrowserCommand::Reload:
    case BrowserCommand::RetryNow:
        return !current_url_.empty() || input_len_ > 0 || !loading_target_.empty();
    case BrowserCommand::Stop: return browser_busy();
    case BrowserCommand::SiteInfo: return !current_url_.empty() || browser_busy();
    case BrowserCommand::FocusFind: return !page_failed_ && (document_ || !page_text_.empty());
    case BrowserCommand::FindPrev:
    case BrowserCommand::FindNext: return find_visible_ && find_len_ > 0 && find_match_count_ > 0;
    default: return true;
    }
}

void BrowserApp::close_chrome_popup()
{
    chrome_backdrop_.restore(window_);
    popup_ = BrowserPopup::None;
    invalidate_header();
}

void BrowserApp::execute_command(BrowserCommand command)
{
    if (!command_enabled(command)) return;
    switch (command)
    {
    case BrowserCommand::Back: close_chrome_popup(); go_history(-1); break;
    case BrowserCommand::Forward: close_chrome_popup(); go_history(1); break;
    case BrowserCommand::Reload: close_chrome_popup(); reload_current(); break;
    case BrowserCommand::RetryNow: {
        const auto target = load_pending_ ? pending_url_ : loading_ ? loading_target_ : current_url_;
        navigate_to(target.empty() ? std::string(url_input_) : target, BrowserNavReason::Reload);
        break;
    }
    case BrowserCommand::Stop: stop_loading(); break;
    case BrowserCommand::FocusUrl: close_chrome_popup(); focus_url(true); break;
    case BrowserCommand::FocusFind: close_chrome_popup(); focus_find(true); break;
    case BrowserCommand::FindPrev: find_next(-1); break;
    case BrowserCommand::FindNext: find_next(1); break;
    case BrowserCommand::CloseFind:
        find_visible_ = false; input_mode_ = BrowserInputMode::None;
        close_chrome_popup(); invalidate_content(); update_ime_context(); break;
    case BrowserCommand::Menu:
    case BrowserCommand::SiteInfo:
    case BrowserCommand::ResourceInfo: {
        const auto next = command == BrowserCommand::Menu ? BrowserPopup::Menu :
                          command == BrowserCommand::SiteInfo ? BrowserPopup::SiteInfo : BrowserPopup::ResourceInfo;
        const bool open = popup_ != next;
        if (find_visible_) invalidate_content();
        close_chrome_popup();
        find_visible_ = false; input_mode_ = BrowserInputMode::None;
        if (open) popup_ = next;
        update_ime_context();
        break;
    }
    case BrowserCommand::DismissNotice:
        notice_dismissed_ = true; invalidate_content(); invalidate_header(); break;
    case BrowserCommand::ToggleTheme:
        dark_theme_ = !dark_theme_; close_chrome_popup();
        // Appearance belongs to browser chrome. Do not clear or redraw a
        // website merely to change the surrounding controls.
        if (!document_ && (page_failed_ || text_lines_.empty())) invalidate_content();
        else draw_scrollbar();
        invalidate_header(); break;
    case BrowserCommand::ErrorDetails:
        failure_details_open_ = !failure_details_open_; invalidate_content(); break;
    default: break;
    }
}

void BrowserApp::update_ime_context()
{
    if (input_mode_ == BrowserInputMode::Page && focused_form_ >= 0 &&
        focused_form_ < static_cast<int>(forms_.controls.size()))
    {
        const auto &control = forms_.controls[focused_form_];
        const auto box = control.element->get_placement();
        XImeContext ime {};
        ime.size = sizeof(ime); ime.version = XIME_ABI_VERSION; ime.window = window_;
        ime.control = form_control_id();
        if (control.editable() && !control.read_only && box.bottom() >= scroll_y_ && box.y < scroll_y_ + content_height())
        {
            const auto style = container_.font_style(control.element->css().get_font());
            std::string caret_text(page_input_, page_len_);
            int cursor = page_cursor_;
            if (control.kind == browser_forms::Kind::Password)
            {
                int masked_cursor = 0;
                std::string masked;
                for (std::size_t offset = 0; offset < caret_text.size();)
                {
                    if (static_cast<int>(offset) < cursor) ++masked_cursor;
                    masked += '*';
                    offset += std::max<std::size_t>(1, ime_utf8_scalar_bytes(caret_text.data() + offset,
                                                                            caret_text.size() - offset));
                }
                caret_text = std::move(masked);
                cursor = masked_cursor;
            }
            int caret_y = static_cast<int>(box.bottom());
            int caret_x = 0;
            if (control.kind == browser_forms::Kind::Textarea)
            {
                const auto start = cursor > 0 ? caret_text.rfind('\n', cursor - 1) : std::string::npos;
                const auto first = start == std::string::npos ? 0 : start + 1;
                const auto end = caret_text.find('\n', first);
                const auto line = caret_text.substr(first, end - first);
                caret_x = browser_ui::text_position(line, cursor - first, style);
                XTextMetrics metrics {};
                xapi_GetTextMetrics(&style, nullptr, &metrics);
                const int row = std::count(caret_text.begin(), caret_text.begin() + cursor, '\n');
                caret_y = std::min(caret_y, static_cast<int>(box.y) + 3 + (row + 1) * metrics.line_height);
                caret_x = std::min(caret_x, std::max(0, static_cast<int>(box.width) - 9));
            }
            else
            {
                const auto view = browser_ui::input_view(caret_text.c_str(), caret_text.size(), cursor,
                    std::max(0, static_cast<int>(box.width) - 8), style);
                caret_x = view.caret;
            }
            ime.flags = XIME_CONTEXT_ENABLED | (control.kind == browser_forms::Kind::Password ? XIME_CONTEXT_PASSWORD : 0);
            ime.caret_x = std::clamp(static_cast<int>(box.x) + 4 + caret_x, 0, std::max(0, window_width_ - 1));
            ime.caret_y = std::clamp(caret_y + content_y() - scroll_y_, 0, std::max(0, window_height_ - 1));
        }
        xapi_ImeSetContext(&ime);
        return;
    }
    const bool url = input_mode_ == BrowserInputMode::Url;
    const auto field = url ? chrome_layout().address_text : chrome_layout().find_text;
    XImeContext ime = {};
    ime.size = sizeof(ime); ime.version = XIME_ABI_VERSION; ime.window = window_;
    ime.control = url ? 1 : 2;
    if (input_mode_ != BrowserInputMode::None && (url || find_visible_) && !field.intersect(chrome_layout().bounds).empty())
    {
        const auto view = browser_ui::input_view(active_input(),
                                               active_length(),
                                               active_cursor(), std::max(0, field.w - 6));
        ime.flags = XIME_CONTEXT_ENABLED;
        ime.caret_x = std::clamp(field.x + 3 + view.caret, 0, std::max(0, window_width_ - 1));
        ime.caret_y = std::clamp(field.bottom(), 0, std::max(0, window_height_ - 1));
    }
    xapi_ImeSetContext(&ime);
}

void BrowserApp::focus_url(bool select_all)
{
    input_mode_ = BrowserInputMode::Url;
    input_cursor_ = input_len_; input_anchor_ = select_all ? 0 : input_cursor_;
    startup_guard_ = false; popup_ = BrowserPopup::None;
    invalidate_header(); update_ime_context();
}

void BrowserApp::focus_find(bool select_all)
{
    if (page_failed_) return;
    input_mode_ = BrowserInputMode::Find;
    find_visible_ = true;
    find_cursor_ = find_len_; find_anchor_ = select_all ? 0 : find_cursor_;
    startup_guard_ = false;
    close_chrome_popup(); invalidate_content(); update_ime_context();
}

bool BrowserApp::replace_input(const char *text, size_t bytes)
{
    if (input_mode_ == BrowserInputMode::Page &&
        (focused_form_ < 0 || focused_form_ >= static_cast<int>(forms_.controls.size()) ||
         !forms_.controls[focused_form_].editable() || forms_.controls[focused_form_].read_only)) return false;
    const bool url      = input_mode_ == BrowserInputMode::Url;
    char      *buffer   = active_input();
    int       &length   = active_length();
    int       &cursor   = active_cursor();
    int       &anchor   = active_anchor();
    size_t     capacity = input_mode_ == BrowserInputMode::Page ? sizeof(page_input_) : url ? sizeof(url_input_) : sizeof(find_input_);
    int        start = std::min(anchor, cursor), end = std::max(anchor, cursor);
    if (length - (end - start) + bytes >= capacity) return false;
    if (input_mode_ == BrowserInputMode::Page)
    {
        std::string proposed(buffer, start);
        if (bytes) proposed.append(text, bytes);
        proposed += buffer + end;
        if (!ime_utf8_valid(proposed.c_str(), proposed.size() + 1)) return false;
        if (!forms_.set_value(focused_form_, proposed)) return false;
    }
    memmove(buffer + start + bytes, buffer + end, length - end + 1);
    if (bytes) memcpy(buffer + start, text, bytes);
    length += static_cast<int>(bytes) - (end - start);
    cursor = anchor = start + static_cast<int>(bytes);
    if (input_mode_ == BrowserInputMode::Page)
    {
        if (focused_form_ < 0 || !forms_.set_value(focused_form_, buffer)) return false;
        const auto target = forms_.controls[focused_form_].element;
        dispatch_script(target, "input");
        invalidate_content();
    }
    else if (!url) update_find_matches();
    invalidate_header();
    return true;
}

void BrowserApp::insert_char(uint64_t packed)
{
    char bytes[8] = {}; memcpy(bytes, &packed, sizeof(packed));
    size_t count = ime_utf8_scalar_bytes(bytes, 4);
    if (count && static_cast<unsigned char>(bytes[0]) >= ' ') replace_input(bytes, count);
}

void BrowserApp::backspace_input()
{
    char      *buffer = active_input();
    int       &cursor = active_cursor();
    int       &anchor = active_anchor();
    if (anchor == cursor && cursor > 0) anchor = static_cast<int>(ime_utf8_previous(buffer, cursor));
    if (anchor != cursor) replace_input(nullptr, 0);
}

void BrowserApp::collect_text(const litehtml::element::ptr &element)
{
    if (!element || element->css().get_display() == litehtml::display_none ||
        element->css().get_visibility() != litehtml::visibility_visible) return;
    if (element->is_text())
    {
        std::string text;
        element->get_text(text);
        const auto box = element->get_placement();
        if (box.width > 0 && box.height > 0 && !text.empty())
        {
            auto style = container_.font_style(element->css().get_font());
            style.anchor = XTEXT_ANCHOR_BASELINE;
            text_runs_.push_back({page_text_.size(), text.size(), box, style});
            page_text_ += text;
        }
    }
    else
    {
        for (const auto &child : element->children()) collect_text(child);
        if (element->is_block_box() && !page_text_.empty() && page_text_.back() != '\n') page_text_ += '\n';
    }
}

void BrowserApp::scroll_to_fragment(const std::string &url)
{
    const auto hash = url.find('#');
    if (hash == std::string::npos) return;
    if (hash + 1 == url.size()) { scroll_y_ = 0; return; }
    std::string id;
    if (!percent_decode(url.substr(hash + 1), id) || !document_) return;
    auto visit = [&](auto &&self, const litehtml::element::ptr &node) -> bool {
        if (!node) return false;
        const char *node_id = node->get_attr("id");
        const char *name = node->get_attr("name");
        if ((node_id && id == node_id) || (name && id == name))
        {
            scroll_y_ = std::max(0, static_cast<int>(node->get_placement().y));
            clamp_scroll();
            return true;
        }
        for (const auto &child : node->children()) if (self(self, child)) return true;
        return false;
    };
    visit(visit, document_->root());
}

void BrowserApp::update_find_matches()
{
    find_matches_.clear();
    find_match_index_ = -1;
    const std::string needle = trim_copy(std::string(find_input_));
    std::size_t pos = 0;
    while (!needle.empty() && pos < page_text_.size())
    {
        pos = find_case_insensitive(page_text_, needle, pos);
        if (pos == std::string::npos) break;
        find_matches_.push_back(pos);
        pos += needle.size();
    }
    find_match_count_ = static_cast<int>(find_matches_.size());
}

void BrowserApp::find_next(int direction)
{
    if (!find_match_count_)
    {
        set_status(browser_tr("查找：无匹配", "Find: no matches"));
        return;
    }
    if (find_match_index_ < 0) find_match_index_ = direction < 0 ? find_match_count_ - 1 : 0;
    else find_match_index_ = (find_match_index_ + direction + find_match_count_) % find_match_count_;
    const auto offset = find_matches_[find_match_index_];
    if (document_)
    {
        for (const auto &run : text_runs_)
        {
            if (offset >= run.offset && offset < run.offset + run.bytes)
            {
                scroll_y_ = std::max(0, static_cast<int>(run.box.y) - 24);
                break;
            }
        }
    } else {
        // Count all matches in a line, rather than treating each line as one hit.
        const std::string needle = trim_copy(std::string(find_input_));
        int seen = 0;
        for (std::size_t i = 0; i < text_lines_.size(); ++i)
        {
            std::size_t pos = 0;
            while ((pos = find_case_insensitive(text_lines_[i], needle, pos)) != std::string::npos)
            {
                if (seen++ == find_match_index_)
                {
                    scroll_y_ = std::max(0, static_cast<int>(i) * BROWSER_TEXT_LINE_HEIGHT - 24);
                    break;
                }
                pos += needle.size();
            }
            if (seen > find_match_index_) break;
        }
    }
    clamp_scroll();
    char status[64];
    std::snprintf(status, sizeof(status), browser_tr("查找 %d/%d", "Find %d/%d"),
                  find_match_index_ + 1, find_match_count_);
    set_status(status);
    invalidate_content();
}

void BrowserApp::draw_find_highlights()
{
    if (!find_visible_ || find_match_index_ < 0 || find_match_index_ >= static_cast<int>(find_matches_.size())) return;
    const auto start = find_matches_[find_match_index_];
    const auto end = start + trim_copy(std::string(find_input_)).size();
    for (const auto &run : text_runs_)
    {
        if (start >= run.offset + run.bytes || end <= run.offset) continue;
        // Underline only the matched scalar range, including multiple hits
        // within one text run, without painting over the glyphs.
        const auto first = std::max(start, run.offset);
        const auto last = std::min(end, run.offset + run.bytes);
        const auto value = page_text_.substr(run.offset, run.bytes);
        const int prefix = browser_ui::text_position(value, first - run.offset, run.style);
        const int extent = browser_ui::text_position(value, last - run.offset, run.style) - prefix;
        const int x = std::max(content_x(), content_x() + static_cast<int>(run.box.x) + prefix);
        const int y = content_y() + static_cast<int>(run.box.bottom()) - scroll_y_;
        const int right = std::min(scrollbar_x() - 1, content_x() + static_cast<int>(run.box.x) + prefix + extent - 1);
        if (x <= right && y >= content_y() && y + 1 < content_y() + content_height())
        {
            xapi_DrawLine(window_, x, y, right, y, COLOR_LINK);
            xapi_DrawLine(window_, x, y + 1, right, y + 1, COLOR_LINK);
        }
    }
}

int BrowserApp::text_width_px(const std::string &text, unsigned int size) const
{
    return browser_ui::text_width(text, size);
}

std::string BrowserApp::href_from_element(litehtml::element::const_ptr element) const
{
    while (element) {
        const char *href = element->get_attr("href");
        if (href == nullptr || *href == '\0') {
            element = element->parent();
            continue;
        }
        return resolve_url(href, base_url_);
    }

    return std::string();
}

void BrowserApp::process_pointer_move(int x, int y)
{
    pointer_x_ = x; pointer_y_ = y;
    const auto command = command_at(x, y);
    if (command != hovered_command_)
    {
        hovered_command_ = command;
        invalidate_header();
    }
    const auto l = chrome_layout();
    const bool floating = popup_ != BrowserPopup::None && l.popup.contains(x, y);
    if (command != BrowserCommand::None || floating || (find_visible_ && l.find.contains(x, y)) ||
        (notice_visible() && l.banner.contains(x, y)) || page_failed_ || !document_ || !hit_content_area(x, y))
    {
        if (!hovered_url_.empty()) { hovered_url_.clear(); invalidate_header(); }
        return;
    }
    litehtml::position::vector redraw;
    const int client_x = x - content_x(), client_y = y - content_y();
    document_->on_mouse_over(client_x, client_y + scroll_y_, client_x, client_y, redraw);
    const auto href = href_from_element(document_->get_over_element());
    if (href != hovered_url_) { hovered_url_ = href; invalidate_header(); }
    if (!redraw.empty()) invalidate_content();
}

void BrowserApp::process_pointer_click(int x, int y)
{
    ctrl_down_ = alt_down_ = false;
    const auto command = command_at(x, y);
    if (command != BrowserCommand::None) { execute_command(command); return; }
    if (popup_ != BrowserPopup::None)
    {
        const bool inside = chrome_layout().popup.contains(x, y);
        close_chrome_popup();
        if (inside) return;
    }
    if (hit_url_input(x, y)) { focus_url(true); return; }
    if (hit_find_input(x, y)) { focus_find(false); return; }
    const auto l = chrome_layout();
    if ((find_visible_ && l.find.contains(x, y)) || (notice_visible() && l.banner.contains(x, y))) return;
    if (handle_scrollbar_click(x, y)) return;
    if (!hit_content_area(x, y)) return;
    if (!document_ || page_failed_)
    {
        input_mode_ = BrowserInputMode::None;
        update_ime_context(); invalidate_header();
        return;
    }
    litehtml::position::vector redraw;
    const int client_x = x - content_x(), client_y = y - content_y();
    auto pointer_target = [&]() {
        document_->on_mouse_over(client_x, client_y + scroll_y_, client_x, client_y, redraw);
        auto target = std::const_pointer_cast<litehtml::element>(document_->get_over_element());
        for (auto ancestor = target; ancestor; ancestor = ancestor->parent())
            if (ancestor->tag() == litehtml::__tag_before_ || ancestor->tag() == litehtml::__tag_after_)
                target = ancestor->parent();
        while (target && target->is_text()) target = target->parent();
        return target;
    };
    auto target = pointer_target();
    if (!target) return;
    const auto gesture_serial = navigation_serial_;
    // The native input message represents a complete left-button gesture.
    // Page handlers can update a link on mousedown before its click action.
    dispatch_script(target, "mousedown");
    if (load_pending_ || navigation_serial_ != gesture_serial || pointer_target() != target) return;
    dispatch_script(target, "mouseup");
    if (load_pending_ || navigation_serial_ != gesture_serial || pointer_target() != target) return;
    auto control_index = forms_.index_of(target);
    if (control_index < forms_.controls.size()) { activate_form(control_index); return; }
    input_mode_ = BrowserInputMode::None;
    update_ime_context(); invalidate_header();
    if (dispatch_script(target, "click")) return;
    const unsigned int before = navigation_serial_;
    document_->on_lbutton_down(client_x, client_y + scroll_y_, client_x, client_y, redraw);
    document_->on_lbutton_up(client_x, client_y + scroll_y_, client_x, client_y, redraw);
    if (before == navigation_serial_)
    {
        const auto href = href_from_element(document_->get_over_element());
        if (!href.empty()) { request_navigation(href); return; }
    }
    invalidate_content();
}

void BrowserApp::handle_message(unsigned long long type, unsigned long long hData, unsigned long long lData)
{
    if (input_mode_ == BrowserInputMode::Page &&
        (focused_form_ < 0 || focused_form_ >= static_cast<int>(forms_.controls.size())))
    {
        focused_form_ = -1;
        input_mode_ = BrowserInputMode::None;
        update_ime_context();
    }
    switch (type) {
    case MSG_PASTE: {
        char              text[sizeof(url_input_)];
        const XClipResult bytes = xapi_ReadPaste(hData, XCLIP_TEXT, text, sizeof(text) - 1);
        XClipInfo         info;
        if (bytes >= 0 && xapi_GetPasteInfo(hData, &info) == 0 && info.window == window_ &&
            (info.control == 1 || info.control == 2 ||
             (input_mode_ == BrowserInputMode::Page && info.control == form_control_id())))
        {
            bool   valid = true;
            const bool multiline = info.control == form_control_id() && focused_form_ >= 0 &&
                forms_.controls[focused_form_].kind == browser_forms::Kind::Textarea;
            size_t used  = 0;
            for (XClipResult i = 0; i < bytes; ++i)
            {
                char ch = text[i];
                if (ch == '\r' && i + 1 < bytes && text[i + 1] == '\n') ++i;
                if (ch == '\r' || ch == '\n') ch = multiline ? '\n' : ' ';
                else if (ch == '\t' && !multiline) ch = ' ';
                else if (ch == '\t' && multiline) {}
                else if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f)
                    valid = false;
                text[used++] = ch;
            }
            BrowserInputMode saved = input_mode_;
            input_mode_ = info.control == 1 ? BrowserInputMode::Url : info.control == 2 ? BrowserInputMode::Find : BrowserInputMode::Page;
            if (valid) replace_input(text, used);
            input_mode_    = saved;
            startup_guard_ = false;
        }
        xapi_FinishPaste(hData, 0);
        break;
    }
    case MSG_EDIT: {
        XClipInfo request;
        if (xapi_GetPasteInfo(lData, &request) < 0 || request.window != window_ ||
            (request.control != 1 && request.control != 2 &&
             (input_mode_ != BrowserInputMode::Page || request.control != form_control_id())))
        {
            xapi_FinishPaste(lData, 0);
            break;
        }
        BrowserInputMode saved = input_mode_;
        input_mode_ = request.control == 1 ? BrowserInputMode::Url : request.control == 2 ? BrowserInputMode::Find : BrowserInputMode::Page;
        char      *buffer      = active_input();
        int       &cursor      = active_cursor();
        int       &anchor      = active_anchor();
        const int  length      = active_length();
        if (hData == XCLIP_EDIT_SELECT_ALL)
        {
            anchor = 0;
            cursor = length;
        }
        else if (anchor != cursor && !(input_mode_ == BrowserInputMode::Page &&
                 forms_.controls[focused_form_].kind == browser_forms::Kind::Password))
        {
            int start = std::min(anchor, cursor), end = std::max(anchor, cursor);
            if (xapi_ClipboardSetText(buffer + start, end - start) >= 0 && hData == XCLIP_EDIT_CUT)
                replace_input(nullptr, 0);
        }
        invalidate_header();
        if (input_mode_ == BrowserInputMode::Page) invalidate_content();
        input_mode_ = saved;
        xapi_FinishPaste(lData, 0);
        break;
    }
    case MSG_KEYDOWN:
    case MSG_KEYUP: {

        if (lData == BROWSER_KEY_SHIFT) shift_down_ = type == MSG_KEYDOWN;
        if (lData == 134) ctrl_down_ = type == MSG_KEYDOWN;
        if (lData == 135) alt_down_ = type == MSG_KEYDOWN;
        if (type != MSG_KEYDOWN) break;
        if (ctrl_down_ && (lData == 'l' || lData == 'L')) { execute_command(BrowserCommand::FocusUrl); break; }
        if (ctrl_down_ && (lData == 'f' || lData == 'F')) { execute_command(BrowserCommand::FocusFind); break; }
        if (alt_down_ && lData == BROWSER_KEY_LEFT) { execute_command(BrowserCommand::Back); break; }
        if (alt_down_ && lData == BROWSER_KEY_RIGHT) { execute_command(BrowserCommand::Forward); break; }
        if (lData == 141) { execute_command(BrowserCommand::Reload); break; }
        if (input_mode_ == BrowserInputMode::Page && lData == 160)
        {
            if (page_cursor_ == page_anchor_ && page_cursor_ < page_len_)
                page_anchor_ += ime_utf8_scalar_bytes(page_input_ + page_cursor_, page_len_ - page_cursor_);
            replace_input(nullptr, 0);
            break;
        }
        if (input_mode_ == BrowserInputMode::None ||
            (lData != BROWSER_KEY_LEFT && lData != BROWSER_KEY_RIGHT &&
             lData != BROWSER_KEY_HOME && lData != BROWSER_KEY_END)) break;
        char      *buffer = active_input();
        int       &cursor = active_cursor();
        int       &anchor = active_anchor();
        const int  length = active_length();
        if (lData == BROWSER_KEY_HOME) cursor = 0;
        if (lData == BROWSER_KEY_END) cursor = length;
        if (lData == BROWSER_KEY_LEFT)
            cursor = !shift_down_ && anchor != cursor ? std::min(anchor, cursor)
                                                      : static_cast<int>(ime_utf8_previous(buffer, cursor));
        if (lData == BROWSER_KEY_RIGHT)
            cursor = !shift_down_ && anchor != cursor
                         ? std::max(anchor, cursor)
                         : cursor + static_cast<int>(ime_utf8_scalar_bytes(buffer + cursor, length - cursor));
        if (!shift_down_) anchor = cursor;
        invalidate_header();
        if (input_mode_ == BrowserInputMode::Page) invalidate_content();
        break;
    }
    case MSG_CHAR:
        if (input_mode_ == BrowserInputMode::Page && focused_form_ >= 0 &&
            !forms_.controls[focused_form_].editable())
        {
            if (lData == ' ') activate_form(focused_form_);
            break;
        }
        if (!ctrl_down_ && !alt_down_ && input_mode_ != BrowserInputMode::None &&
            static_cast<unsigned char>(lData) >= ' ') {
            startup_guard_ = false;
            insert_char(lData);
        }
        break;
    case MSG_SPCHAR:
        if (script_runtime_ && (input_mode_ == BrowserInputMode::Page || input_mode_ == BrowserInputMode::None))
        {
            const auto target = input_mode_ == BrowserInputMode::Page && focused_form_ >= 0 ?
                forms_.controls[focused_form_].element : nullptr;
            const int raw = static_cast<unsigned char>(lData);
            const int code = raw == 128 ? 27 : static_cast<char>(lData) == '\n' ? 13 : raw;
            const std::string key = code == 27 ? "Escape" : code == 13 ? "Enter" :
                code == 8 ? "Backspace" : code == 9 ? "Tab" : "";
            const bool prevented = dispatch_script(target, "keydown", key, code);
            dispatch_script(target, "keyup", key, code);
            if (prevented) break;
        }
        if (static_cast<unsigned char>(lData) == 128) {
            if (popup_ != BrowserPopup::None) close_chrome_popup();
            else if (find_visible_) execute_command(BrowserCommand::CloseFind);
            else if (loading_ || load_pending_) stop_loading();
        } else if (static_cast<char>(lData) == '\t') {
            tab_form(shift_down_ ? -1 : 1);
        } else if (input_mode_ == BrowserInputMode::None && static_cast<unsigned char>(lData) != BROWSER_KEY_F3) {
            break;
        } else if (static_cast<char>(lData) == '\b') {
            startup_guard_ = false;
            backspace_input();
        } else if (static_cast<char>(lData) == '\n') {
            if (input_mode_ == BrowserInputMode::Find) {
                find_next(1);
            } else if (input_mode_ == BrowserInputMode::Page) {
                if (focused_form_ >= 0 && forms_.controls[focused_form_].kind == browser_forms::Kind::Textarea) replace_input("\n", 1);
                else if (focused_form_ >= 0 && forms_.controls[focused_form_].editable()) submit_form(focused_form_);
                else activate_form(focused_form_);
            } else if (!startup_guard_) {
                navigate_to(url_input_, BrowserNavReason::Normal);
            } else {
                startup_guard_ = false;
            }
        } else if (static_cast<unsigned char>(lData) == BROWSER_KEY_F3) {
            if (!find_visible_) focus_find(false);
            find_next(shift_down_ ? -1 : 1);
        }
        break;
    case MSG_MOVE:
        process_pointer_move(static_cast<int>(hData), static_cast<int>(lData));
        break;
    case MSG_LBUTTON:
        process_pointer_click(static_cast<int>(hData), static_cast<int>(lData));
        break;
    case MSG_ROLLER:
        {
            const int old_scroll = scroll_y_;
            scroll_y_ -= static_cast<int>(hData) * BROWSER_SCROLL_STEP;
            clamp_scroll();
            if (scroll_y_ != old_scroll) {
                refresh_content_view();
                if (script_runtime_ && !processing_scripts_)
                {
                    BrowserScriptScope scope(processing_scripts_);
                    script_runtime_->view_changed("scroll");
                }
            }
        }
        break;
    case MSG_RESIZE:
        window_width_ = std::max(1, static_cast<int>(hData));
        window_height_ = std::max(1, static_cast<int>(lData));
        if (document_) {
            container_.set_viewport(content_width(), content_height());
            // Viewport lengths and math expressions were resolved when styles
            // were computed. Rebuild the render tree from the same DOM so they
            // update even if no media-query breakpoint changes.
            if (!prepared_html_.empty()) rebuild_resource_document(true);
            else
            {
                document_->media_changed();
                document_->render(static_cast<float>(content_width()));
                page_text_.clear(); text_runs_.clear();
                collect_text(document_->root()); update_find_matches();
            }
        } else if (!page_text_.empty()) {
            text_lines_ = wrap_text_for_width(page_text_, std::max(120, content_width() - 24));
            text_content_height_ = static_cast<int>(text_lines_.size()) * BROWSER_TEXT_LINE_HEIGHT + 24;
        }
        clamp_scroll();
        if (script_runtime_ && !processing_scripts_)
        {
            BrowserScriptScope scope(processing_scripts_);
            script_runtime_->view_changed("resize");
        }
        invalidate_all();
        break;
    default:
        break;
    }
    if (type != MSG_MOVE) update_ime_context();
}

void BrowserApp::load_current_url()
{
    load_pending_ = false;
    loading_ = true;
    cancelled_ = false;
    loading_serial_ = navigation_serial_;
    const unsigned int serial = loading_serial_;
    const BrowserNavReason reason = pending_nav_reason_;
    const int history_target = pending_history_index_;
    // Build a new page transactionally. Input can supersede this navigation
    // while resource fetches pump events, so keep the committed page alive.
    struct LoadScope
    {
        BrowserApp &app;
        litehtml::document::ptr document;
        browser_forms::Model forms;
        std::unique_ptr<BrowserScriptRuntime> runtime;
        std::vector<litehtml::element::ptr> dynamic_scripts;
        std::vector<ModuleTransfer> module_transfers;
        std::vector<ScriptLink> script_links;
        std::vector<std::weak_ptr<litehtml::element>> started_scripts;
        std::vector<ScriptHistoryUpdate> history_updates;
        std::string history_base, history_state;
        int history_delta;
        bool history_go_pending, scroll_pending, module_fetch_cancelled;
        double scroll_x, scroll_y;
        litehtml::element::ptr script_submission;
        std::string current, base, title, text, html;
        unsigned int document_serial;
        std::vector<std::string> lines;
        std::vector<TextRun> runs;
        std::vector<std::size_t> matches;
        std::vector<DeferredScript> scripts;
        std::string script_navigation;
        bool dom_ready, load_ready, script_replace;
        unsigned executed;
        std::size_t script_bytes;
        int scroll, height, match_index, match_count;
        bool old_base_set;
        bool committed = false;
        explicit LoadScope(BrowserApp &owner) : app(owner), document(std::move(owner.document_)), forms(std::move(owner.forms_)),
            runtime(std::move(owner.script_runtime_)), dynamic_scripts(std::move(owner.dynamic_scripts_)), script_submission(std::move(owner.script_submission_)),
            current(owner.current_url_), base(std::move(owner.base_url_)), title(std::move(owner.page_title_)),
            text(std::move(owner.page_text_)), html(std::move(owner.prepared_html_)),
            document_serial(owner.document_serial_),
            lines(std::move(owner.text_lines_)), runs(std::move(owner.text_runs_)),
            matches(std::move(owner.find_matches_)),
            scripts(std::move(owner.deferred_scripts_)),
            script_navigation(std::move(owner.script_navigation_)), dom_ready(owner.script_dom_ready_),
            load_ready(owner.script_load_ready_), script_replace(owner.script_replace_),
            executed(owner.executed_scripts_), script_bytes(owner.script_bytes_),
            scroll(owner.scroll_y_), height(owner.text_content_height_), match_index(owner.find_match_index_),
            match_count(owner.find_match_count_), old_base_set(owner.base_url_set_)
        {
            module_transfers = std::move(owner.module_transfers_);
            script_links = std::move(owner.script_links_);
            started_scripts = std::move(owner.started_script_nodes_);
            history_updates = std::move(owner.script_history_updates_);
            history_base = std::move(owner.script_history_base_);
            history_state = std::move(owner.loading_history_state_);
            history_delta = owner.script_history_delta_;
            history_go_pending = owner.script_history_go_pending_;
            scroll_pending = owner.script_scroll_pending_;
            scroll_x = owner.script_scroll_x_; scroll_y = owner.script_scroll_y_;
            module_fetch_cancelled = owner.module_fetch_cancelled_;
        }
        ~LoadScope()
        {
            app.loading_ = false;
            app.cancelled_ = false;
            if (!committed)
            {
                app.current_url_ = std::move(current);
                app.document_ = std::move(document);
                app.forms_ = std::move(forms);
                app.script_runtime_ = std::move(runtime);
                app.dynamic_scripts_ = std::move(dynamic_scripts);
                app.module_transfers_ = std::move(module_transfers);
                app.script_links_ = std::move(script_links);
                app.started_script_nodes_ = std::move(started_scripts);
                app.script_history_updates_ = std::move(history_updates);
                app.script_history_base_ = std::move(history_base);
                app.loading_history_state_ = std::move(history_state);
                app.script_history_delta_ = history_delta;
                app.script_history_go_pending_ = history_go_pending;
                app.script_scroll_pending_ = scroll_pending;
                app.script_scroll_x_ = scroll_x; app.script_scroll_y_ = scroll_y;
                app.module_fetch_cancelled_ = module_fetch_cancelled;
                app.script_submission_ = std::move(script_submission);
                app.script_navigation_ = std::move(script_navigation);
                app.script_dom_ready_ = dom_ready; app.script_load_ready_ = load_ready;
                app.script_replace_ = script_replace; app.executed_scripts_ = executed;
                app.script_bytes_ = script_bytes;
                app.base_url_set_ = old_base_set;
                app.base_url_ = std::move(base);
                app.page_title_ = std::move(title);
                app.page_text_ = std::move(text);
                app.text_lines_ = std::move(lines);
                app.text_runs_ = std::move(runs);
                app.find_matches_ = std::move(matches);
                app.prepared_html_ = std::move(html); app.deferred_scripts_ = std::move(scripts);
                app.document_serial_ = document_serial;
                app.scroll_y_ = scroll; app.text_content_height_ = height;
                app.find_match_index_ = match_index; app.find_match_count_ = match_count;
                app.update_find_matches();
                if (!app.load_pending_)
                {
                    std::snprintf(app.url_input_, sizeof(app.url_input_), "%s", app.current_url_.c_str());
                    app.input_len_ = app.input_cursor_ = app.input_anchor_ =
                        static_cast<int>(std::strlen(app.url_input_));
                }
            }
            app.update_window_title();
            app.update_ime_context();
            app.invalidate_all();
        }
    } load_scope {*this};
    script_navigation_.clear();
    dynamic_scripts_.clear();
    module_transfers_.clear(); script_links_.clear(); started_script_nodes_.clear();
    script_history_updates_.clear(); script_history_base_.clear();
    script_history_go_pending_ = script_scroll_pending_ = false;
    module_fetch_cancelled_ = false;
    loading_history_state_ = reason == BrowserNavReason::History && history_target >= 0 &&
        history_target < static_cast<int>(history_states_.size()) ?
        history_states_[history_target].script_state : "null";
    script_dom_ready_ = script_load_ready_ = false;
    executed_scripts_ = 0;
    script_bytes_ = 0;
    force_reload_ = reason == BrowserNavReason::Reload;
    if (force_reload_) browser_http_close_idle();
    resource_failed_ = false;
    reset_resource_jobs();
    network_notice_.clear();
    set_status(browser_tr("正在加载...", "Loading..."));
    render_header();
    xapi_RefreshWindow(window_);
    std::string candidate_url;

    std::string target = pending_url_.empty() ? std::string(url_input_) : pending_url_;
    loading_target_ = target;
    try {
        BrowserFetchResult response;
        BrowserPreparedDocument prepared;

        {
            log_line(std::string(browser_tr("开始获取 URL=", "Fetching URL=")) + target);
            response = fetch_resource(target, true);
            if (serial != navigation_serial_ || cancelled_ || response.cancelled) return;
            if (!response.ok) {
                text_lines_.clear();
                text_runs_.clear();
                page_text_.clear();
                page_title_.clear();
                text_content_height_ = 0;
                update_find_matches();
                log_line(std::string(browser_tr("加载失败：", "Load failed: ")) + response.error);
                document_.reset();
                base_url_ = target;
                scroll_y_ = 0;
                current_url_ = target;
                const std::string detail = browser_fetch_error_message(response.error_code, response.tls_failure,
                                                                        g_browser_language == XJ380_LANGUAGE_EN_US);
                char attempts[128];
                std::snprintf(attempts, sizeof(attempts), browser_tr("尝试次数：%u", "Attempts: %u"),
                              response.attempts);
                network_notice_ = detail + " (" + attempts + ")";
                page_failed_ = true; page_secure_ = false; failure_details_open_ = false;
                failure_url_ = target; failure_code_ = response.error_code; failure_tls_ = response.tls_failure;
                failure_attempts_ = response.attempts; failure_detail_ = response.error;
                input_mode_ = BrowserInputMode::None;
                if (reason == BrowserNavReason::Normal || reason == BrowserNavReason::Replace)
                    record_history(target, reason == BrowserNavReason::Replace);
                else if (reason == BrowserNavReason::History && history_target >= 0 &&
                         history_target < static_cast<int>(history_.size())) history_index_ = history_target;
                BrowserFetchResult error_page;
                error_page.content_type = "text/plain";
                error_page.body = std::string(browser_tr("无法打开网页", "Unable to open page")) + "\n\n" +
                    target + "\n\n" + detail + "\n" + attempts + "\n\n" +
                    browser_tr("点击刷新可以重新尝试。网络异常时请检查网络或 VPN 设置。",
                               "Click Reload to try again. For connection problems, check your network or VPN.");
                prepare_text_view(error_page);
                load_scope.committed = true;
                set_status(browser_tr("加载失败", "Load failed"));
                invalidate_content();
                return;
            }

            base_url_set_ = false;
            base_url_ = response.final_url.empty() ? target : response.final_url;
            candidate_url = base_url_;
            loading_document_url_ = candidate_url;
            reset_resource_jobs(); // Resource cookies use the newly loaded document site.
            if (target.find('#') != std::string::npos && candidate_url.find('#') == std::string::npos)
                candidate_url += target.substr(target.find('#'));

            char info[256];
            std::snprintf(info,
                          sizeof(info),
                          browser_tr("获取成功 状态=%d 字节=%llu 类型=%s 字符集=%s",
                                     "Fetch ok status=%d bytes=%llu type=%s charset=%s"),
                          response.status_code,
                          static_cast<unsigned long long>(response.body.size()),
                          response.content_type.empty() ? "-" : response.content_type.c_str(),
                          response.charset.empty() ? "-" : response.charset.c_str());
            log_line(info);

            if (looks_like_html(response))
            {
                litehtml::estring bytes(response.body);
                bytes.encoding = litehtml::get_encoding(lower_copy(response.charset));
                bytes.confidence = bytes.encoding == litehtml::encoding::null ? litehtml::confidence::tentative
                                                                            : litehtml::confidence::certain;
                litehtml::encoding_sniffing_algorithm(bytes);
                response.body = litehtml::decode(bytes, bytes.encoding);
                response.charset = "utf-8";
            }
            prepared = BrowserPreparedDocument();
            if (BROWSER_ENABLE_LITEHTML) {
                prepared = prepare_document_html(response);
                if (prepared.script_blocks > 0)
                    log_line("JavaScript programs discovered: " + std::to_string(prepared.script_blocks));
            }
        }

        if (serial != navigation_serial_ || cancelled_) return;
        page_title_.clear();
        scroll_y_ = 0;
        document_.reset();
        text_lines_.clear();
        page_text_.clear();
        text_runs_.clear();
        text_content_height_ = 0;

        prepared_html_ = prepared.html;
        const auto dependencies = browser_html_scripts(prepared_html_);
        for (const auto &href : dependencies.stylesheets)
        {
            const auto url = resolve_url(href, base_url_);
            if (!resource_response(url)) enqueue_resource(url, BrowserStylesheet, 0);
        }
        if (!wait_stylesheets(serial)) return;
        bool prepared_fallback = !BROWSER_ENABLE_LITEHTML || prepared.fallback_used;
        if (!prepared_fallback) {
            litehtml::estring html(prepared.html);
            html.encoding = litehtml::encoding::utf_8;
            html.confidence = litehtml::confidence::certain;
            if (!response.charset.empty()) {
                litehtml::encoding declared_encoding = litehtml::get_encoding(lower_copy(response.charset));
                if (declared_encoding != litehtml::encoding::null) {
                    html.encoding = declared_encoding;
                }
            }

            container_.set_viewport(content_width(), content_height());
            log_line(browser_tr("开始创建文档", "Creating document"));
            BrowserParserScope parser_scope;
            for (unsigned int pass = 0; pass < 8; ++pass)
            {
                css_import_count_ = 0;
                document_ = litehtml::document::createFromString(html, &container_, browser_master_css(), BROWSER_USER_CSS);
                if (!resource_jobs_.pending(BrowserStylesheet)) break;
                document_.reset();
                if (!wait_stylesheets(serial)) return;
            }
            if (document_) {
                log_line(browser_tr("开始渲染文档", "Rendering document"));
                document_->render(static_cast<float>(content_width()));
                page_text_.clear();
                if (document_->root()) {
                    collect_text(document_->root());
                    update_find_matches();
                }
                log_line(browser_tr("文档渲染完成", "Document rendered"));
            } else {
                prepared_fallback = true;
            }
        }

        if (prepared_fallback) {
            if (!BROWSER_ENABLE_LITEHTML) {
                log_line(browser_tr("litehtml 已禁用：启用安全文本模式",
                                    "litehtml is disabled: using safe text mode"));
            }
            prepare_text_view(response);
            log_line(browser_tr("安全文本渲染就绪", "Safe text rendering is ready"));
        }
        if (serial != navigation_serial_ || cancelled_) { document_.reset(); return; }
        forms_.collect(document_ ? document_->root() : nullptr);
        ++form_generation_;
        focused_form_ = -1;
        if (reason == BrowserNavReason::History && history_target >= 0 &&
            history_target < static_cast<int>(history_states_.size()))
        {
            forms_.restore_state(history_states_[history_target].forms);
            scroll_y_ = history_states_[history_target].scroll;
        }
        current_url_ = candidate_url;
        input_mode_ = BrowserInputMode::None;
        initialize_scripts(current_url_);
        if (serial != navigation_serial_ || cancelled_) return;
        page_failed_ = false;
        page_secure_ = lower_copy(candidate_url).rfind("https://", 0) == 0;
        failure_detail_.clear();
        document_serial_ = serial;
        scroll_to_fragment(target);
        clamp_scroll();
        std::snprintf(url_input_, sizeof(url_input_), "%s", current_url_.c_str());
        input_len_ = input_cursor_ = input_anchor_ = static_cast<int>(std::strlen(url_input_));
        prioritize_visible_images();
        set_status(resources_pending() ? browser_tr("正在加载资源", "Loading resources") :
                   resource_failed_ ? browser_tr("部分资源失败", "Partial load") : browser_tr("已加载", "Loaded"));
        if (reason == BrowserNavReason::Normal || reason == BrowserNavReason::Replace)
            record_history(script_history_base_.empty() ? current_url_ : script_history_base_,
                           reason == BrowserNavReason::Replace);
        else if (reason == BrowserNavReason::History && history_target >= 0 &&
                 history_target < static_cast<int>(history_.size())) history_index_ = history_target;
        if (history_index_ >= 0 && history_index_ < static_cast<int>(history_states_.size()))
            history_states_[history_index_].document = document_serial_;
        flush_script_history();
        suppress_history_record_ = false;
        pending_history_index_ = -1;
        load_scope.committed = true;
        invalidate_content();
    } catch (const std::exception &ex) {
        log_line(std::string(browser_tr("异常：", "Exception: ")) + ex.what());
        document_.reset();
        set_status(browser_tr("异常", "Exception"));
        invalidate_content();
    } catch (...) {
        log_line(browser_tr("未知异常", "Unknown exception"));
        document_.reset();
        set_status(browser_tr("异常", "Exception"));
        invalidate_content();
    }
}


void BrowserApp::update_window_title()
{
    std::string title = page_title_;
    while (title.size() > 160) title.resize(ime_utf8_previous(title.c_str(), title.size()));
    if (!title.empty()) title += " · ";
    title += browser_tr("XJ380 浏览器", "XJ380 Browser");
    if (window_) xapi_SetWindowTitle(window_, const_cast<char *>(title.c_str()));
}

std::string BrowserApp::page_host() const
{
    std::string value = load_pending_ ? pending_url_ : loading_ ? loading_target_ :
                        current_url_.empty() ? std::string(url_input_) : current_url_;
    const auto scheme = value.find("://");
    if (scheme != std::string::npos) value.erase(0, scheme + 3);
    value.resize(value.find_first_of("/?#") == std::string::npos ? value.size() : value.find_first_of("/?#"));
    return value;
}

void BrowserApp::draw_chrome_button(const BrowserButton &button)
{
    using browser_ui::Icon;
    const auto theme = chrome_theme();
    const bool enabled = command_enabled(button.command);
    const bool active = (button.command == BrowserCommand::Menu && popup_ == BrowserPopup::Menu) ||
                        (button.command == BrowserCommand::SiteInfo && popup_ == BrowserPopup::SiteInfo) ||
                        (button.command == BrowserCommand::FocusFind && find_visible_);
    const bool hover = button.rect.contains(pointer_x_, pointer_y_);
    if (enabled && (active || hover)) chrome_canvas_.round(button.rect, 6, theme.hover);
    const auto color = enabled ? theme.text : theme.disabled;
    if (button.label)
    {
        const browser_ui::Rect image {button.rect.x + 6, button.rect.y, 22, button.rect.h};
        chrome_canvas_.icon(button.icon, image, color, 16);
        chrome_canvas_.text({button.rect.x + 34, button.rect.y + (button.rect.h - 16) / 2,
                             std::max(0, button.rect.w - 42), 20}, button.label, color);
    }
    else chrome_canvas_.icon(button.icon, button.rect, color);
}

void BrowserApp::draw_chrome_input(browser_ui::Rect field, bool url)
{
    const auto theme = chrome_theme();
    const char *buffer = url ? url_input_ : find_input_;
    const int length = url ? input_len_ : find_len_;
    const bool editing = input_mode_ == (url ? BrowserInputMode::Url : BrowserInputMode::Find);
    const int cursor = url ? input_cursor_ : find_cursor_;
    const int anchor = url ? input_anchor_ : find_anchor_;
    field = field.inset(3);
    if (field.empty()) return;
    if (url && browser_busy() && !editing && field.w > 440)
    {
        const int width = std::min(180, text_width_px(status_, 10));
        chrome_canvas_.text({field.right() - width, field.y + 4, width, 20}, status_, theme.secondary);
        field.w -= width + 16;
    }
    const auto view = browser_ui::input_view(buffer, length, editing ? cursor : 0, field.w);
    if (editing && cursor != anchor)
    {
        const int start = std::max(view.first, std::min(anchor, cursor));
        const int end = std::min(view.first + static_cast<int>(view.text.size()), std::max(anchor, cursor));
        if (end > start)
        {
            const int x1 = field.x + browser_ui::text_position(view.text, start - view.first, 10);
            const int x2 = field.x + browser_ui::text_position(view.text, end - view.first, 10);
            chrome_canvas_.fill({x1, field.y + 2, x2 - x1, 20}, theme.selection);
        }
    }
    if (length) chrome_canvas_.text({field.x, field.y + 4, field.w, 22}, view.text, theme.text);
    else chrome_canvas_.text({field.x, field.y + 4, field.w, 22},
                             url ? browser_tr("输入网址", "Enter an address") :
                                   browser_tr("在页面中查找", "Find in page"), theme.secondary);
    if (editing)
        chrome_canvas_.fill({field.x + std::min(field.w - 1, view.caret), field.y + 4, 1, 17}, theme.accent);
}

void BrowserApp::draw_chrome_paragraph(browser_ui::Rect rect, const std::string &text, uint32_t color)
{
    if (rect.w <= 0) return;
    int y = rect.y;
    for (const auto &line : wrap_text_for_width(text, rect.w))
    {
        if (y + 16 > rect.bottom()) break;
        chrome_canvas_.text({rect.x, y, rect.w, 18}, line, color);
        y += 18;
    }
}

void BrowserApp::render_frame()
{
    if (window_width_ > 0 && window_height_ > 0)
        xapi_DrawRect(window_, 0, 0, window_width_ - 1, window_height_ - 1, COLOR_PANEL, true);
}

void BrowserApp::render_header()
{
    const auto l = chrome_layout();
    const auto theme = chrome_theme();
    chrome_canvas_.begin(l.toolbar.intersect(l.bounds), theme.toolbar);
    if (input_mode_ == BrowserInputMode::Url)
    {
        chrome_canvas_.round(l.address, 9, theme.accent);
        chrome_canvas_.round(l.address.inset(1), 8, theme.surface);
    }
    else chrome_canvas_.round(l.address, 9, theme.field);
    for (const auto &button : chrome_buttons())
        if (button.rect.y < l.content_top) draw_chrome_button(button);
    draw_chrome_input(l.address_text, true);
    chrome_canvas_.fill({0, l.content_top - 1, window_width_, 1}, theme.line);
    if (browser_busy()) chrome_canvas_.fill({0, l.content_top - 2, 100, 2}, theme.accent);
    chrome_canvas_.present(window_);
}

void BrowserApp::render_native_page()
{
    using browser_ui::Icon;
    const auto l = chrome_layout();
    const auto theme = chrome_theme();
    const browser_ui::Rect viewport {0, content_y(), window_width_, content_height()};
    chrome_canvas_.begin(viewport.intersect(l.bounds), theme.surface);
    if (page_failed_)
    {
        chrome_canvas_.icon(failure_tls_ == 2 ? Icon::Alert : Icon::Globe,
                            {l.error_title.x, l.error_title.y - 76, 48, 48}, theme.secondary, 44);
        chrome_canvas_.text({l.error_title.x, l.error_title.y, l.error_title.w, 38},
                            failure_tls_ == 2 ? browser_tr("无法验证此网站的身份", "Unable to verify this website") :
                                               browser_tr("暂时无法打开此网页", "This page could not be opened"),
                            theme.text, window_width_ < 600 ? 15 : 18);
        chrome_canvas_.text({l.error_title.x, l.error_title.y + 43, l.error_title.w, 22}, page_host(), theme.accent);
        std::string message = browser_fetch_error_message(failure_code_, failure_tls_,
                                                          g_browser_language == XJ380_LANGUAGE_EN_US);
        if (failure_attempts_ > 1)
            message += browser_tr("。已尝试 ", ". Attempted ") + std::to_string(failure_attempts_) +
                       browser_tr(" 次，仍未成功。", " times without success.");
        else message += browser_tr("。", ".");
        draw_chrome_paragraph(l.error_body, message, theme.secondary);
        const std::string help = failure_tls_ == 2 || failure_tls_ == 4
            ? browser_tr("请检查系统日期与时间，或联系网站管理员。", "Check the system date and time, or contact the website owner.")
            : browser_tr("请检查网址与网络连接，或稍后再试。", "Check the address and network connection, or try again later.");
        draw_chrome_paragraph({l.error_body.x, l.error_body.y + 58, l.error_body.w, 50}, help, theme.secondary);
        for (const auto &button : chrome_buttons())
        {
            if (button.rect == l.error_reload || button.rect == l.error_back)
            {
                const bool primary = button.rect == l.error_reload;
                chrome_canvas_.round(button.rect, 7, primary ? theme.accent : theme.line);
                if (!primary) chrome_canvas_.round(button.rect.inset(1), 6, theme.surface);
                const auto color = primary ? theme.on_accent :
                                   command_enabled(button.command) ? theme.text : theme.disabled;
                chrome_canvas_.icon(button.icon, {button.rect.x + 8, button.rect.y, 22, button.rect.h}, color, 16);
                chrome_canvas_.text({button.rect.x + 36, button.rect.y + 10, button.rect.w - 40, 22},
                                    button.label, color);
            }
        }
        chrome_canvas_.fill({l.error_details.x, l.error_details.y - 12, l.error_details.w, 1}, theme.line);
        chrome_canvas_.icon(failure_details_open_ ? Icon::Up : Icon::Down,
                            {l.error_details.x, l.error_details.y, 18, 28}, theme.secondary, 13);
        chrome_canvas_.text({l.error_details.x + 24, l.error_details.y + 5, l.error_details.w - 24, 22},
                            browser_tr("查看详细信息", "Show details"), theme.secondary);
        if (failure_details_open_)
        {
            const std::string code = "errno " + std::to_string(failure_code_) + " / TLS " + std::to_string(failure_tls_);
            draw_chrome_paragraph({l.error_details.x, l.error_details.bottom() + 6, l.error_details.w, 80},
                                  code + "\n" + failure_detail_, theme.secondary);
        }
    }
    else
    {
        const int width = std::max(0, std::min(540, window_width_ - 48));
        const int left = std::max(0, (window_width_ - width) / 2);
        const int top = content_y() + 68;
        chrome_canvas_.round({window_width_ / 2 - 28, top, 56, 56}, 15, theme.field);
        chrome_canvas_.icon(Icon::Globe, {window_width_ / 2 - 28, top, 56, 56}, theme.accent, 32);
        const std::string title = browser_tr("从一个网址开始。", "Start with an address.");
        const unsigned size = window_width_ < 520 ? 17 : 21;
        const int text_width = text_width_px(title, size);
        chrome_canvas_.text({std::max(left, (window_width_ - text_width) / 2), top + 85, width, 42}, title, theme.text, size);
        const std::string description = browser_tr("在地址栏输入网址，按 Enter 打开。", "Enter a website address above and press Enter.");
        draw_chrome_paragraph({left + 12, top + 143, std::max(0, width - 24), 56}, description, theme.secondary);
        for (const auto &button : chrome_buttons())
            if (button.command == BrowserCommand::FocusUrl)
            {
                chrome_canvas_.round(button.rect, 8, theme.accent);
                chrome_canvas_.icon(Icon::Arrow, {button.rect.x + 12, button.rect.y, 22, button.rect.h}, theme.on_accent);
                chrome_canvas_.text({button.rect.x + 44, button.rect.y + 10, button.rect.w - 48, 22},
                                    button.label, theme.on_accent);
            }
        chrome_canvas_.text({left + 12, content_y() + 336, std::max(0, width - 24), 24},
                            browser_tr("Ctrl+L  输入网址       Ctrl+F  页面查找",
                                       "Ctrl+L  Address       Ctrl+F  Find in page"), theme.secondary);
    }
    chrome_canvas_.present(window_);
}

void BrowserApp::render_content()
{
    if (window_height_ <= content_y() || window_width_ <= 0) return;
    if (page_failed_ || (!document_ && text_lines_.empty())) { render_native_page(); return; }
    xapi_DrawRect(window_, 0, content_y(), window_width_ - 1, window_height_ - 1, COLOR_PANEL, true);
    if (document_)
    {
        try
        {
            container_.set_viewport(content_width(), content_height());
            litehtml::position clip(content_x(), content_y(), content_width(), content_height());
            container_.begin_draw(clip);
            struct Scope { BrowserContainer &container; ~Scope() { container.end_draw(); } } scope {container_};
            // Fixed boxes share the page's screen origin, without its scroll offset.
            document_->draw(window_, content_x(), content_y() - scroll_y_, &clip, content_x(), content_y());
            draw_find_highlights();
        }
        catch (const std::exception &error) { log_line(error.what()); set_status(browser_tr("绘制异常", "Draw exception")); }
        catch (...) { log_line("Browser draw exception"); set_status(browser_tr("绘制异常", "Draw exception")); }
    }
    else
    {
        const int start = std::max(0, scroll_y_ / BROWSER_TEXT_LINE_HEIGHT);
        for (int i = start; i < static_cast<int>(text_lines_.size()); ++i)
        {
            const int y = content_y() + 12 + i * BROWSER_TEXT_LINE_HEIGHT - scroll_y_;
            if (y + BROWSER_TEXT_LINE_HEIGHT > window_height_) break;
            if (y < content_y()) continue;
            xapi_DrawMonoTextTop(window_, 12, y, const_cast<char *>(text_lines_[i].c_str()), COLOR_TEXT);
        }
    }
    draw_scrollbar();
}

void BrowserApp::render_overlays(bool content_repainted)
{
    using browser_ui::Icon;
    const auto l = chrome_layout();
    const auto theme = chrome_theme();
    if (content_repainted)
    {
        chrome_backdrop_.reset();
        hint_backdrop_.reset();
        notice_backdrop_.reset();
    }
    else
    {
        // Restore overlays in reverse painting order. Their geometry can change
        // when a retry banner appears, including inside a fetch callback where
        // entering the document renderer would be unsafe.
        chrome_backdrop_.restore(window_);
        hint_backdrop_.restore(window_);
        notice_backdrop_.restore(window_);
    }
    notice_painted_ = false;
    if (notice_visible())
    {
        const auto rect = l.banner.intersect(l.bounds);
        notice_backdrop_.capture(window_, rect, true);
        chrome_canvas_.begin(rect, theme.warning);
        chrome_canvas_.icon(Icon::Alert, {12, l.banner.y + 9, 18, 22}, theme.warning_text, 16);
        auto text = l.banner_text; text.h = 36;
        draw_chrome_paragraph(text, network_notice_, theme.warning_text);
        for (const auto &button : chrome_buttons())
            if (button.rect == l.banner_action || button.rect == l.banner_close) draw_chrome_button(button);
        chrome_canvas_.fill({0, l.banner.bottom() - 1, window_width_, 1}, theme.line);
        chrome_canvas_.present(window_);
        notice_painted_ = !rect.empty();
    }
    std::string hint = hovered_url_;
    if (hint.empty())
    {
        switch (hovered_command_)
        {
        case BrowserCommand::Back: hint = browser_tr("后退  Alt+Left", "Back  Alt+Left"); break;
        case BrowserCommand::Forward: hint = browser_tr("前进  Alt+Right", "Forward  Alt+Right"); break;
        case BrowserCommand::Reload: hint = browser_tr("重新加载  F5", "Reload  F5"); break;
        case BrowserCommand::Stop: hint = browser_tr("停止加载  Esc", "Stop loading  Esc"); break;
        case BrowserCommand::FocusFind: hint = browser_tr("在页面中查找  Ctrl+F", "Find in page  Ctrl+F"); break;
        default: break;
        }
    }
    if (!hint.empty() && window_height_ > content_y() + 100)
    {
        const int width = std::min(window_width_, text_width_px(hint, 9) + 22);
        const browser_ui::Rect rect {0, window_height_ - 26, width, 26};
        hint_backdrop_.capture(window_, rect, true);
        chrome_canvas_.begin(rect, theme.toolbar);
        chrome_canvas_.fill({0, rect.y, rect.w, 1}, theme.line);
        chrome_canvas_.text({8, rect.y + 5, std::max(0, width - 16), 20}, hint, theme.secondary, 9);
        chrome_canvas_.present(window_);
    }
    const bool find = find_visible_ && !page_failed_ && popup_ == BrowserPopup::None;
    if (!find && popup_ == BrowserPopup::None) return;
    auto rect = (find ? l.find : l.popup).intersect(l.bounds);
    if (rect.empty()) return;
    chrome_backdrop_.capture(window_, rect, content_repainted);
    chrome_canvas_.begin(rect, theme.surface, &chrome_backdrop_.pixels);
    chrome_canvas_.round(rect, 9, theme.line);
    chrome_canvas_.round(rect.inset(1), 8, theme.surface);
    if (find)
    {
        draw_chrome_input(l.find_text, false);
        char count[40];
        if (!find_len_) count[0] = 0;
        else if (find_match_count_ > 0)
            std::snprintf(count, sizeof(count), "%d/%d", std::max(1, find_match_index_ + 1), find_match_count_);
        else std::snprintf(count, sizeof(count), "0/0");
        chrome_canvas_.text({l.find_count.x, l.find_count.y + 7, l.find_count.w, 22}, count, theme.secondary, 9);
        for (const auto &button : chrome_buttons())
            if (button.command == BrowserCommand::FindPrev || button.command == BrowserCommand::FindNext ||
                button.command == BrowserCommand::CloseFind) draw_chrome_button(button);
    }
    else if (popup_ == BrowserPopup::Menu)
    {
        for (const auto &button : chrome_buttons())
            if (l.popup.contains(button.rect.x, button.rect.y)) draw_chrome_button(button);
    }
    else if (popup_ == BrowserPopup::ResourceInfo)
    {
        chrome_canvas_.icon(Icon::Alert, {rect.x + 14, rect.y + 16, 24, 28}, theme.warning_text);
        chrome_canvas_.text({rect.x + 46, rect.y + 21, rect.w - 60, 26},
                            browser_tr("部分资源未能加载", "Some resources did not load"), theme.text, 11);
        draw_chrome_paragraph({rect.x + 18, rect.y + 63, rect.w - 36, 70}, resource_error_, theme.secondary);
        draw_chrome_paragraph({rect.x + 18, rect.y + 140, rect.w - 36, 60},
            browser_tr("正文仍可查看。重新加载网页可以再次尝试。", "The page remains usable. Reload it to try again."), theme.secondary);
    }
    else
    {
        const bool secure = page_secure_ && !page_failed_ && !loading_ && !load_pending_;
        const std::string title = secure ? browser_tr("连接已加密", "Connection is encrypted") :
            page_failed_ || browser_busy() ? browser_tr("尚未建立可靠连接", "Connection is not ready") :
            browser_tr("连接未加密", "Connection is not encrypted");
        chrome_canvas_.icon(secure ? Icon::Shield : Icon::Info, {rect.x + 16, rect.y + 16, 24, 28}, theme.accent);
        chrome_canvas_.text({rect.x + 48, rect.y + 20, rect.w - 64, 28}, title, theme.text, 11);
        chrome_canvas_.text({rect.x + 18, rect.y + 62, rect.w - 36, 22}, page_host(), theme.secondary);
        const std::string message = secure
            ? browser_tr("此网站的证书已通过验证，数据通过 HTTPS 传输。", "The certificate was verified. Data is transferred over HTTPS.")
            : browser_busy() || page_failed_
                ? browser_tr("连接完成前，无法确认此网站的安全状态。", "The website cannot be verified before its connection completes.")
                : browser_tr("此页面使用 HTTP，传输内容未经加密。", "This page uses HTTP. Its content is not encrypted.");
        draw_chrome_paragraph({rect.x + 18, rect.y + 98, rect.w - 36, 90}, message, theme.secondary);
    }
    chrome_canvas_.present(window_);

}

void BrowserApp::refresh_content_view()
{
    render_content();
    render_overlays(true);
    xapi_RefreshWindow(window_);
    content_dirty_ = false;
    need_redraw_ = header_dirty_ || frame_dirty_;
}

void BrowserApp::render()
{
    const bool frame = frame_dirty_, header = header_dirty_ || frame;
    const bool content = content_dirty_ || frame || notice_painted_ != notice_visible();
    // Clear entry flags before painting so invalidations raised by drawing
    // are retained for the next turn.
    frame_dirty_ = header_dirty_ = content_dirty_ = false;
    if (frame) render_frame();
    if (header) render_header();
    if (content) render_content();
    if (header || content) { render_overlays(content); xapi_RefreshWindow(window_); }
    need_redraw_ = frame_dirty_ || header_dirty_ || content_dirty_;
}

void BrowserApp::initialize_window()
{
    g_app = this;

    XWINDOW window {};
    window.title = browser_tr("XJ380 浏览器", "XJ380 Browser");
    window.width = BROWSER_WIDTH;
    window.height = BROWSER_HEIGHT;
    window.sets = XWIN_NORMAL | XWIN_SUPPORT_RESIZEABLE;

    xapi_CreateWindow(&window_, &window);
    xapi_SetIcon(window_, const_cast<char *>("/system/resources/icon/browser.png"));
    SetMsgPrcor(window_, [](unsigned long long type, unsigned long long hData, unsigned long long lData) {
        if (g_app != nullptr) {
            g_app->enqueue_message(type, hData, lData);
        }
    });

    xapi_ClipboardAttach(window_, XCLIP_STANDARD, XCLIP_FORMAT_BIT(XCLIP_TEXT));
    update_ime_context();
    render();
}

int BrowserApp::run()
{
    initialize_window();
    while (true) {
        drain_messages();
        if (load_pending_) {
            load_current_url();
        }
        if (need_redraw_) {
            render();
        }
        try { pump_resources(); }
        catch (const std::exception &error)
        {
            resource_jobs_.cancel();
            image_decode_queue_.clear();
            log_line(error.what());
            set_status(browser_tr("资源加载异常", "Resource error"));
        }
        xapi_Sleep(resources_pending() ? 1 : BROWSER_LOOP_SLEEP_MS);
    }

    return 0;
}

} // namespace

#ifndef BROWSER_APP_TESTING
static int browser_main_impl(int argc, char *argv[], char *envp[])
{
    (void)envp;

    BrowserApp app;
    if (argc > 1) app.open_initial_url(argv[1]);
    return app.run();
}

extern "C" int browser_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int browser_main_cpp(int argc, char *argv[], char *envp[])
{
    return browser_main_impl(argc, argv, envp);
}

#endif // BROWSER_APP_TESTING
