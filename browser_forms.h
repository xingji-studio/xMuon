#pragma once

#include "browser_url.h"

#include <litehtml.h>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Native HTML form state belongs to the browser, independently of litehtml's
// layout tree. Snapshots contain no DOM pointers and survive a document rebuild.
namespace browser_forms
{

enum class Kind { Text, Password, Hidden, Submit, Button, Textarea, Checkbox, Radio, Select, Reset, File };

struct Option
{
    litehtml::element::ptr element;
    std::string value, label;
    bool selected = false, disabled = false, default_selected = false;
};

struct Control
{
    litehtml::element::ptr element, form;
    Kind kind = Kind::Text;
    std::string type, name, value, key, default_value;
    bool disabled = false, read_only = false, checked = false, multiple = false, default_checked = false;
    std::size_t max_length = std::numeric_limits<std::size_t>::max();
    int tab_index = 0;
    std::vector<Option> options;

    bool editable() const { return kind == Kind::Text || kind == Kind::Password || kind == Kind::Textarea; }
    bool focusable() const { return !disabled && kind != Kind::Hidden && kind != Kind::File; }
};

struct State
{
    Kind kind = Kind::Text;
    std::string key, value;
    bool checked = false;
    std::vector<bool> selected;
    std::vector<std::string> option_values;
};

struct Submission
{
    bool ok = false;
    std::string url, error;
};

namespace detail
{

inline std::string lower(std::string text)
{
    for (char &ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return text;
}

inline std::string attr(const litehtml::element::const_ptr &element, const char *name)
{
    const char *value = element ? element->get_attr(name) : nullptr;
    return value ? value : "";
}

inline bool has(const litehtml::element::const_ptr &element, const char *name)
{
    return element && element->get_attr(name) != nullptr;
}

inline bool tag(const litehtml::element::const_ptr &element, const char *name)
{
    const char *value = element ? element->get_tagName() : nullptr;
    return value && lower(value) == name;
}

inline bool inside(litehtml::element::const_ptr element, const litehtml::element::const_ptr &parent)
{
    while (element)
    {
        if (element == parent) return true;
        element = element->parent();
    }
    return false;
}

inline bool disabled(const litehtml::element::const_ptr &element)
{
    if (has(element, "disabled")) return true;
    for (auto parent = element->parent(); parent; parent = parent->parent())
    {
        if (!tag(parent, "fieldset") || !has(parent, "disabled")) continue;
        litehtml::element::ptr first_legend;
        for (const auto &child : parent->children())
            if (tag(child, "legend")) { first_legend = child; break; }
        // The first legend's descendants remain enabled in a disabled fieldset.
        if (!first_legend || !inside(element, first_legend)) return true;
    }
    return false;
}

inline std::string text(const litehtml::element::const_ptr &element)
{
    std::string result;
    if (element) element->get_text(result);
    return result;
}

inline bool space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

inline std::string collapse_space(const std::string &text)
{
    std::string result;
    bool pending = false;
    for (char ch : text)
    {
        if (space(ch)) { pending = !result.empty(); continue; }
        if (pending) result += ' ';
        pending = false;
        result += ch;
    }
    return result;
}

inline std::string line_endings(const std::string &text, bool multiline)
{
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        char ch = text[i];
        if (ch == '\r')
        {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            ch = '\n';
        }
        if (ch != '\n' || multiline) result += ch;
    }
    return result;
}

// maxlength counts UTF-16 code units. A supplementary UTF-8 scalar therefore
// consumes two units; malformed byte sequences are rejected rather than sent.
inline bool valid_length(const std::string &text, std::size_t maximum)
{
    std::size_t units = 0;
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        unsigned int scalar = lead;
        std::size_t bytes = 1;
        if (lead >= 0xc2 && lead <= 0xdf) { scalar = lead & 0x1f; bytes = 2; }
        else if (lead >= 0xe0 && lead <= 0xef) { scalar = lead & 0x0f; bytes = 3; }
        else if (lead >= 0xf0 && lead <= 0xf4) { scalar = lead & 7; bytes = 4; }
        else if (lead >= 0x80 || lead == 0) return false;
        if (bytes > text.size() - i) return false;
        for (std::size_t j = 1; j < bytes; ++j)
        {
            const unsigned char next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if ((bytes == 2 && scalar < 0x80) || (bytes == 3 && scalar < 0x800) ||
            (bytes == 4 && scalar < 0x10000) || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
        const std::size_t charge = scalar >= 0x10000 ? 2 : 1;
        if (charge > maximum - std::min(maximum, units)) return false;
        units += charge;
        i += bytes;
    }
    return true;
}

inline std::size_t unsigned_attribute(const std::string &text, std::size_t fallback)
{
    if (text.empty()) return fallback;
    std::size_t value = 0;
    for (char ch : text)
    {
        if (ch < '0' || ch > '9') return fallback;
        const std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) return fallback;
        value = value * 10 + digit;
    }
    return value;
}

inline int tab_index(const std::string &text)
{
    if (text.empty()) return 0;
    const bool negative = text[0] == '-';
    const std::size_t number = unsigned_attribute(text.substr(negative ? 1 : text[0] == '+' ? 1 : 0), 0);
    const int bounded = static_cast<int>(std::min<std::size_t>(number, 32767));
    return negative ? -bounded : bounded;
}

inline std::string encoded(const std::string &text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string output;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == '\r' || ch == '\n')
        {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            output += "%0D%0A";
        }
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '*' || ch == '-' || ch == '.' || ch == '_') output += ch;
        else if (ch == ' ') output += '+';
        else
        {
            output += '%';
            output += hex[ch >> 4];
            output += hex[ch & 15];
        }
    }
    return output;
}

} // namespace detail

class Model
{
public:
    std::vector<Control> controls;
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    void collect(const litehtml::element::ptr &root)
    {
        controls.clear();
        if (!root) return;
        std::vector<litehtml::element::ptr> nodes, pending {root};
        while (!pending.empty())
        {
            auto node = pending.back(); pending.pop_back();
            if (detail::tag(node, "template") || detail::tag(node, "datalist")) continue;
            nodes.push_back(node);
            const auto &children = node->children();
            for (auto it = children.rbegin(); it != children.rend(); ++it) pending.push_back(*it);
        }
        std::map<std::string, litehtml::element::ptr> ids;
        std::map<const litehtml::element *, std::string> form_keys;
        std::size_t form_number = 0;
        for (const auto &node : nodes)
        {
            const auto id = detail::attr(node, "id");
            if (!id.empty()) ids.emplace(id, node); // The first matching ID wins.
            if (detail::tag(node, "form"))
                form_keys[node.get()] = id.empty() ? "form:" + std::to_string(form_number++) : "form-id:" + id;
        }
        std::map<std::string, std::size_t> occurrences;
        for (const auto &node : nodes)
        {
            const bool input = detail::tag(node, "input"), button = detail::tag(node, "button");
            if (!input && !button && !detail::tag(node, "textarea") && !detail::tag(node, "select")) continue;
            Control control;
            control.element = node;
            control.type = detail::lower(detail::attr(node, "type"));
            if (detail::tag(node, "textarea")) control.kind = Kind::Textarea;
            else if (detail::tag(node, "select")) control.kind = Kind::Select;
            else if (button)
                control.kind = control.type == "button" ? Kind::Button : control.type == "reset" ? Kind::Reset : Kind::Submit;
            else if (control.type == "password") control.kind = Kind::Password;
            else if (control.type == "hidden") control.kind = Kind::Hidden;
            else if (control.type == "submit" || control.type == "image") control.kind = Kind::Submit;
            else if (control.type == "button") control.kind = Kind::Button;
            else if (control.type == "reset") control.kind = Kind::Reset;
            else if (control.type == "checkbox") control.kind = Kind::Checkbox;
            else if (control.type == "radio") control.kind = Kind::Radio;
            else if (control.type == "file") control.kind = Kind::File;

            if (detail::has(node, "form"))
            {
                auto found = ids.find(detail::attr(node, "form"));
                if (found != ids.end() && detail::tag(found->second, "form")) control.form = found->second;
            }
            else
            {
                for (auto parent = node->parent(); parent; parent = parent->parent())
                    if (detail::tag(parent, "form")) { control.form = parent; break; }
            }
            control.name = detail::attr(node, "name");
            control.disabled = detail::disabled(node);
            control.read_only = control.editable() && detail::has(node, "readonly");
            control.multiple = detail::has(node, "multiple");
            control.checked = control.default_checked = detail::has(node, "checked");
            if (detail::has(node, "data-xj380-checked"))
                control.checked = detail::attr(node, "data-xj380-checked") == "true";
            control.tab_index = detail::tab_index(detail::attr(node, "tabindex"));
            control.max_length = detail::unsigned_attribute(detail::attr(node, "maxlength"), control.max_length);
            if (control.kind == Kind::Textarea)
                control.value = detail::line_endings(node->get_attr("data-xj380-value",
                    detail::text(node).c_str()), true);
            else if (control.kind == Kind::Checkbox || control.kind == Kind::Radio)
                control.value = detail::has(node, "value") ? detail::attr(node, "value") : "on";
            else control.value = detail::line_endings(detail::attr(node, "value"), false);

            if (control.kind == Kind::Select)
            {
                for (const auto &candidate : nodes)
                {
                    if (!detail::tag(candidate, "option") || !detail::inside(candidate, node)) continue;
                    // Options belong to their closest select, even in malformed HTML.
                    auto parent = candidate->parent();
                    while (parent && !detail::tag(parent, "select")) parent = parent->parent();
                    if (parent != node) continue;
                    Option option;
                    option.element = candidate;
                    option.label = detail::has(candidate, "label") ? detail::attr(candidate, "label") :
                                   detail::collapse_space(detail::text(candidate));
                    option.value = detail::has(candidate, "value") ? detail::attr(candidate, "value") :
                                   detail::collapse_space(detail::text(candidate));
                    option.selected = detail::has(candidate, "selected");
                    option.disabled = detail::has(candidate, "disabled");
                    for (auto group = candidate->parent(); group && group != node; group = group->parent())
                        if (detail::tag(group, "optgroup") && detail::has(group, "disabled")) option.disabled = true;
                    control.options.push_back(std::move(option));
                }
                if (!control.multiple)
                {
                    std::size_t selected = npos;
                    for (std::size_t i = 0; i < control.options.size(); ++i)
                        if (control.options[i].selected) selected = i;
                    // A single-row select defaults to its first enabled option.
                    if (selected == npos && detail::unsigned_attribute(detail::attr(node, "size"), 0) <= 1)
                        for (std::size_t i = 0; i < control.options.size(); ++i)
                            if (!control.options[i].disabled) { selected = i; break; }
                    for (std::size_t i = 0; i < control.options.size(); ++i) control.options[i].selected = i == selected;
                }
                for (auto &option : control.options) option.default_selected = option.selected;
                sync_select(control);
            }
            control.default_value = control.value;
            const std::string owner = control.form ? form_keys[control.form.get()] : "unowned";
            const std::string id = detail::attr(node, "id");
            const std::string identity = owner + (id.empty() ? "/name:" + control.name + "/type:" +
                std::to_string(static_cast<int>(control.kind)) : "/id:" + id);
            control.key = identity + "/occurrence:" + std::to_string(occurrences[identity]++);
            controls.push_back(std::move(control));
        }
        normalize_radio_groups();
    }

    std::size_t index_of(litehtml::element::const_ptr element) const
    {
        // Text or generated child nodes inside a button resolve to that button.
        for (auto current = element; current; current = current->parent())
        {
            for (std::size_t i = 0; i < controls.size(); ++i)
                if (controls[i].element == current) return i;
            if (detail::tag(current, "label"))
            {
                const std::string target = detail::attr(current, "for");
                for (std::size_t i = 0; i < controls.size(); ++i)
                    if ((!target.empty() && detail::attr(controls[i].element, "id") == target) ||
                        (target.empty() && detail::inside(controls[i].element, current))) return i;
            }
        }
        return npos;
    }

    std::vector<State> save_state() const
    {
        std::vector<State> state;
        for (const auto &control : controls)
        {
            // A refetched page owns its hidden session/CSRF tokens. Passwords
            // and button/file values must never enter browsing-history state.
            if (!restorable(control)) continue;
            State entry;
            entry.kind = control.kind;
            entry.key = control.key; entry.value = control.value; entry.checked = control.checked;
            for (const auto &option : control.options)
            {
                entry.selected.push_back(option.selected);
                entry.option_values.push_back(option.value);
            }
            state.push_back(std::move(entry));
        }
        return state;
    }

    void restore_state(const std::vector<State> &state)
    {
        for (std::size_t index = 0; index < controls.size(); ++index)
        {
            auto &control = controls[index];
            if (!restorable(control)) continue;
            auto found = std::find_if(state.begin(), state.end(), [&](const State &entry) {
                return entry.key == control.key && entry.kind == control.kind;
            });
            if (found == state.end()) continue;
            if (control.editable())
            {
                // Revalidate because a new response can tighten maxlength.
                set_value(index, found->value);
                continue;
            }
            control.checked = found->checked;
            if (control.kind == Kind::Select)
            {
                bool same_options = found->option_values.size() == control.options.size() &&
                                    found->selected.size() == control.options.size();
                for (std::size_t i = 0; same_options && i < control.options.size(); ++i)
                    same_options = found->option_values[i] == control.options[i].value;
                if (same_options)
                    for (std::size_t i = 0; i < control.options.size(); ++i)
                        control.options[i].selected = found->selected[i];
            }
            sync(control);
        }
    }

    bool set_value(std::size_t index, const std::string &value)
    {
        if (index >= controls.size()) return false;
        auto &control = controls[index];
        if (!control.editable() || control.disabled || control.read_only) return false;
        const std::string normalized = detail::line_endings(value, control.kind == Kind::Textarea);
        if (!detail::valid_length(normalized, control.max_length)) return false;
        control.value = normalized;
        sync(control);
        return true;
    }

    void activate(std::size_t index)
    {
        if (index >= controls.size() || controls[index].disabled) return;
        auto &control = controls[index];
        if (control.kind == Kind::Checkbox) control.checked = !control.checked;
        else if (control.kind == Kind::Radio)
        {
            if (!control.name.empty())
                for (auto &other : controls)
                    if (other.kind == Kind::Radio && other.form == control.form && other.name == control.name)
                        { other.checked = false; sync(other); }
            control.checked = true;
        }
        else if (control.kind == Kind::Reset)
        {
            if (!control.form) return;
            for (auto &other : controls)
            {
                if (other.form != control.form) continue;
                other.value = other.default_value; other.checked = other.default_checked;
                for (auto &option : other.options) option.selected = option.default_selected;
                sync(other);
            }
            normalize_radio_groups();
        }
        else if (control.kind == Kind::Select && !control.options.empty())
        {
            // Until the native popup is available, activation cycles enabled
            // choices. Multiple selection remains represented and submitted.
            std::size_t selected = control.options.size() - 1;
            for (std::size_t i = 0; i < control.options.size(); ++i)
                if (control.options[i].selected) { selected = i; break; }
            for (std::size_t step = 1; step <= control.options.size(); ++step)
            {
                const auto next = (selected + step) % control.options.size();
                if (control.options[next].disabled) continue;
                for (auto &option : control.options) option.selected = false;
                control.options[next].selected = true;
                break;
            }
        }
        sync(control);
    }

    Submission submit(std::size_t source, const std::string &document_url, const std::string &base_url) const
    {
        Submission result;
        if (source >= controls.size() || !controls[source].form || controls[source].disabled)
        {
            result.error = "This control has no enabled form.";
            return result;
        }
        return submit_owner(controls[source].form, document_url, base_url, source);
    }

    // form.submit() has no submitter and bypasses submit events. It must not
    // inherit a button's name, formaction or disabled default-button state.
    Submission submit_owner(const litehtml::element::ptr &owner, const std::string &document_url,
                            const std::string &base_url, std::size_t source = npos) const
    {
        Submission result;
        if (!owner) { result.error = "The form is detached."; return result; }
        const Control *submitter = source < controls.size() && controls[source].kind == Kind::Submit ?
            &controls[source] : nullptr;
        if (!submitter && source < controls.size() && controls[source].editable())
        {
            for (const auto &control : controls)
            {
                if (control.form != owner || control.kind != Kind::Submit) continue;
                if (control.disabled) { result.error = "The default submit button is disabled."; return result; }
                submitter = &control;
                break;
            }
        }
        std::string method = detail::lower(detail::attr(owner, "method"));
        if (submitter && detail::has(submitter->element, "formmethod"))
            method = detail::lower(detail::attr(submitter->element, "formmethod"));
        // Unknown/missing method is GET in HTML. POST and dialog must never be
        // silently converted to GET, which could expose private form contents.
        if (method == "post" || method == "dialog")
        {
            result.error = "Only GET forms are supported.";
            return result;
        }
        std::string action = detail::attr(owner, "action");
        if (submitter && detail::has(submitter->element, "formaction")) action = detail::attr(submitter->element, "formaction");
        std::string url = action.empty() ? document_url : browser_resolve_url(base_url.empty() ? document_url : base_url, action);
        browser_url_detail::Parts parts;
        if (!browser_url_detail::parse(url, parts) || !parts.has_authority ||
            (detail::lower(parts.scheme) != "http" && detail::lower(parts.scheme) != "https"))
        {
            result.error = "The form action must be an HTTP or HTTPS URL.";
            return result;
        }
        std::string query;
        auto pair = [&](const std::string &name, const std::string &value)
        {
            if (!query.empty()) query += '&';
            query += detail::encoded(name); query += '='; query += detail::encoded(value);
        };
        for (const auto &control : controls)
        {
            if (control.form != owner || control.disabled || control.name.empty()) continue;
            if (control.kind == Kind::Button || control.kind == Kind::Reset) continue;
            if (control.kind == Kind::Submit && &control != submitter) continue;
            if ((control.kind == Kind::Checkbox || control.kind == Kind::Radio) && !control.checked) continue;
            if (control.kind == Kind::Select)
            {
                for (const auto &option : control.options)
                    if (option.selected && !option.disabled) pair(control.name, option.value);
            }
            else if (control.kind == Kind::Submit && control.type == "image")
            {
                pair(control.name + ".x", "0"); pair(control.name + ".y", "0");
            }
            else pair(control.name, control.name == "_charset_" && control.kind == Kind::Hidden ? "UTF-8" : control.value);
        }
        parts.has_query = true;
        parts.query = std::move(query);
        result.url = browser_url_detail::serialize(parts);
        if (result.url.size() >= 4096)
        {
            result.url.clear();
            result.error = "The submitted URL exceeds the browser's 4095-byte limit.";
            return result;
        }
        result.ok = true;
        return result;
    }

private:
    void normalize_radio_groups()
    {
        // HTML parsing/reset leaves the last checked radio selected when more
        // than one default-checked input belongs to the same named group.
        for (std::size_t index = 0; index < controls.size(); ++index)
        {
            const auto &control = controls[index];
            if (control.kind != Kind::Radio || !control.checked || control.name.empty()) continue;
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                auto &other = controls[previous];
                if (other.kind == Kind::Radio && other.form == control.form && other.name == control.name)
                    { other.checked = false; sync(other); }
            }
        }
    }

    static bool restorable(const Control &control)
    {
        return !control.disabled && !control.read_only &&
            (control.kind == Kind::Text || control.kind == Kind::Textarea || control.kind == Kind::Checkbox ||
             control.kind == Kind::Radio || control.kind == Kind::Select);
    }

    static void sync_select(Control &control)
    {
        control.value.clear();
        for (const auto &option : control.options)
            if (option.selected) { control.value = option.value; break; }
        control.element->set_attr("value", control.value.c_str());
    }

    static void sync(Control &control)
    {
        if (control.kind == Kind::Select) sync_select(control);
        else control.element->set_attr("value", control.value.c_str());
        if (control.kind == Kind::Textarea)
            control.element->set_attr("data-xj380-value", control.value.c_str());
        // litehtml has no public attribute-removal API. Do not turn an unchecked
        // boolean attribute into checked="false" (which is still checked HTML).
        if (control.kind == Kind::Checkbox || control.kind == Kind::Radio)
            control.element->set_attr("data-xj380-checked", control.checked ? "true" : "false");
    }
};

} // namespace browser_forms
