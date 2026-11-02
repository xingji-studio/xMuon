#pragma once

#include "browser_parser_memory.h"

#include <gumbo.h>
#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

struct BrowserHtmlScript
{
    // Byte offsets into the caller's UTF-8 HTML. All attribute strings are
    // copied so nothing in this result borrows the temporary Gumbo tree.
    std::size_t begin, end, content_begin, content_end;
    std::string type, src;
    // Boolean attributes are true whenever present, even async="false".
    bool async = false, defer = false;
    bool nomodule = false;
    std::string integrity;
};

struct BrowserHtmlScripts
{
    std::vector<BrowserHtmlScript> scripts;
    std::vector<std::string> stylesheets;
    bool has_base = false;
    std::size_t base_begin = 0;
    std::string base_href;
};

inline bool browser_html_has_stylesheet_rel(const char *rel)
{
    auto is_space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; };
    static const char stylesheet[] = "stylesheet";
    while (*rel)
    {
        while (is_space(*rel)) ++rel;
        const char *start = rel;
        while (*rel && !is_space(*rel)) ++rel;
        if (static_cast<std::size_t>(rel - start) != sizeof(stylesheet) - 1) continue;
        bool match = true;
        for (std::size_t i = 0; i < sizeof(stylesheet) - 1; ++i)
        {
            const char ch = start[i] >= 'A' && start[i] <= 'Z' ? start[i] + ('a' - 'A') : start[i];
            if (ch != stylesheet[i]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Discover script and stylesheet metadata using HTML parsing rules, rather than
// searching comments, attributes, textarea/title contents or inert templates.
// The browser decides which script MIME types its JavaScript runtime supports.
// Call this before removing any <noscript> markers.
inline BrowserHtmlScripts browser_html_scripts(const std::string &html)
{
    BrowserHtmlScripts result;
    BrowserParserScope parser;
    GumboOptions options = kGumboDefaultOptions;
    options.max_errors = 0;
    struct OutputScope
    {
        const GumboOptions *options;
        GumboOutput *output;
        ~OutputScope() { if (output != nullptr) gumbo_destroy_output(options, output); }
    } output {&options, gumbo_parse_with_options(&options, html.data(), html.size())};
    if (output.output == nullptr || output.output->root == nullptr) return result;

    // Iterative traversal also avoids adding a C++ stack frame per HTML node.
    std::vector<const GumboNode *> pending {output.output->root};
    std::vector<std::pair<std::size_t, std::string>> stylesheets;
    while (!pending.empty())
    {
        const GumboNode *node = pending.back();
        pending.pop_back();
        if (node->type != GUMBO_NODE_ELEMENT) continue;
        const GumboElement &element = node->v.element;
        const bool html_element = element.tag_namespace == GUMBO_NAMESPACE_HTML;
        if (html_element && (element.tag == GUMBO_TAG_TEMPLATE || element.tag == GUMBO_TAG_NOSCRIPT)) continue;

        if (html_element && element.tag == GUMBO_TAG_BASE && !result.has_base)
        {
            const GumboAttribute *href = gumbo_get_attribute(&element.attributes, "href");
            if (href != nullptr)
            {
                result.has_base = true;
                result.base_begin = element.start_pos.offset;
                result.base_href = href->value;
            }
        }
        if (html_element && element.tag == GUMBO_TAG_SCRIPT)
        {
            const std::size_t begin = element.start_pos.offset;
            const std::size_t content_end = element.end_pos.offset;
            // Synthesized/unclosed tags are not complete executable programs.
            if (element.original_tag.length == 0 || element.original_end_tag.length == 0 || begin > html.size() ||
                element.original_tag.length > html.size() - begin || content_end > html.size() ||
                element.original_end_tag.length > html.size() - content_end)
                continue;
            const std::size_t content_begin = begin + element.original_tag.length;
            if (content_end < content_begin) continue;
            const GumboAttribute *type = gumbo_get_attribute(&element.attributes, "type");
            const GumboAttribute *src = gumbo_get_attribute(&element.attributes, "src");
            result.scripts.push_back({begin, content_end + element.original_end_tag.length, content_begin, content_end,
                                      type == nullptr ? std::string() : std::string(type->value),
                                      src == nullptr ? std::string() : std::string(src->value),
                                      gumbo_get_attribute(&element.attributes, "async") != nullptr,
                                      gumbo_get_attribute(&element.attributes, "defer") != nullptr,
                                      gumbo_get_attribute(&element.attributes, "nomodule") != nullptr,
                                      gumbo_get_attribute(&element.attributes, "integrity") == nullptr ? std::string() :
                                          std::string(gumbo_get_attribute(&element.attributes, "integrity")->value)});
            continue;
        }
        if (html_element && element.tag == GUMBO_TAG_LINK &&
            gumbo_get_attribute(&element.attributes, "disabled") == nullptr)
        {
            const GumboAttribute *rel = gumbo_get_attribute(&element.attributes, "rel");
            const GumboAttribute *href = gumbo_get_attribute(&element.attributes, "href");
            if (rel != nullptr && href != nullptr && browser_html_has_stylesheet_rel(rel->value))
                stylesheets.emplace_back(element.start_pos.offset, href->value);
        }
        for (std::size_t i = element.children.length; i > 0; --i)
            pending.push_back(static_cast<const GumboNode *>(element.children.data[i - 1]));
    }

    // HTML error recovery can move nodes out of source order (foster parenting).
    std::sort(result.scripts.begin(), result.scripts.end(),
              [](const BrowserHtmlScript &left, const BrowserHtmlScript &right) { return left.begin < right.begin; });
    std::sort(stylesheets.begin(), stylesheets.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    for (auto &stylesheet : stylesheets) result.stylesheets.push_back(std::move(stylesheet.second));
    return result;
}
