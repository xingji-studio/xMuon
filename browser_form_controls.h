#pragma once

// Included after BrowserApp: these controls remain in the browser integration,
// with layout, stacking and clipping owned by litehtml's normal render tree.
class BrowserFormElement final : public litehtml::html_tag
{
    BrowserApp &app_;
public:
    BrowserFormElement(const litehtml::document::ptr &document, BrowserApp &app)
        : litehtml::html_tag(document), app_(app) {}

    bool has_rich_button_content() const
    {
        if (std::strcmp(get_tagName(), "button") != 0) return false;
        for (const auto &child : litehtml::element::children())
            if (!child->is_text() && !child->is_comment()) return true;
        return false;
    }

    bool is_replaced() const override { return !has_rich_button_content(); }

    void get_content_size(litehtml::size &size, litehtml::pixel_t) override
    {
        const std::string tag = get_tagName();
        const std::string type = lower_copy(get_attr("type", "text"));
        const float font = css().get_font_size();
        size.height = font + 10;
        if (tag == "textarea") size.height *= std::clamp(std::atoi(get_attr("rows", "2")), 1, 30);
        if (type == "checkbox" || type == "radio") size.width = size.height = 18;
        else if (tag == "button" || type == "submit" || type == "reset" || type == "button")
        {
            std::string label = get_attr("value", "");
            if (tag == "button") get_text(label);
            if (label.empty()) label = type == "reset" ? "Reset" : "Submit";
            const auto style = app_.container_.font_style(css().get_font());
            size.width = std::max(64.0f, static_cast<float>(browser_ui::text_width(label, style)) + 16);
        }
        else size.width = std::clamp(std::atoi(get_attr(tag == "textarea" ? "cols" : "size", "20")), 1, 200) *
                          std::max(8.0f, css().get_font_metrics().ch_width);
    }

    void compute_styles(bool recursive = true) override
    {
        litehtml::html_tag::compute_styles(recursive);
        litehtml::size size;
        if (!has_rich_button_content())
        {
            get_content_size(size, 0);
            // Native fields have independent intrinsic dimensions, unlike images.
            if (css().get_width().is_predefined()) css_w().set_width(litehtml::css_length(size.width));
            if (css().get_height().is_predefined()) css_w().set_height(litehtml::css_length(size.height));
        }
        if (lower_copy(get_attr("type", "")) == "hidden") css_w().set_display(litehtml::display_none);
    }

    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item> &parent) override
    {
        // Icon/composite buttons lay out their real descendants. Their native
        // activation still uses the same form model and ancestor hit lookup.
        if (has_rich_button_content()) return litehtml::html_tag::create_render_item(parent);
        auto item = std::make_shared<litehtml::render_item_image>(shared_from_this());
        item->parent(parent);
        return item;
    }

    void draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
              const litehtml::position *clip, const std::shared_ptr<litehtml::render_item> &item) override
    {
        litehtml::html_tag::draw(hdc, x, y, clip, item);
        if (has_rich_button_content()) return;
        auto position = item->pos();
        position.x += x;
        position.y += y;
        app_.draw_form_control(hdc, shared_from_this(), position, clip);
    }
};

static const std::string &browser_master_css()
{
    static const std::string css = std::string(litehtml::master_css) +
        "input,textarea,select,button{padding:4px;border:1px solid #8793a5;background-color:white;"
        "color:#17202e;vertical-align:middle;}"
        "button{display:inline-block;}"
        "input[type=submit],input[type=button],input[type=reset],button{background-color:#edf1f7;}";
    return css;
}

litehtml::element::ptr BrowserContainer::create_element(const char *tag, const litehtml::string_map &,
                                                       const std::shared_ptr<litehtml::document> &document)
{
    if (std::strcmp(tag, "svg") == 0) return std::make_shared<BrowserInlineSvgElement>(document);
    if (std::strcmp(tag, "input") == 0 || std::strcmp(tag, "textarea") == 0 ||
        std::strcmp(tag, "select") == 0 || std::strcmp(tag, "button") == 0)
        return std::make_shared<BrowserFormElement>(document, app_);
    return nullptr;
}

void BrowserApp::save_history_state()
{
    if (loading_ || history_index_ < 0 || history_index_ >= static_cast<int>(history_.size())) return;
    history_states_.resize(history_.size());
    history_states_[history_index_].forms = forms_.save_state();
    history_states_[history_index_].scroll = scroll_y_;
}

static void browser_preserve_form_defaults(const std::vector<browser_forms::Control> &previous,
                                           browser_forms::Model &current)
{
    for (auto &control : current.controls)
        for (const auto &old : previous)
            if (old.element == control.element)
            {
                control.default_value = old.default_value;
                control.default_checked = old.default_checked;
                for (auto &option : control.options)
                    for (const auto &old_option : old.options)
                        if (old_option.element == option.element) option.default_selected = old_option.default_selected;
                break;
            }
}

static bool browser_form_visible(litehtml::element::const_ptr element)
{
    for (auto node = element; node; node = node->parent())
        if (node->css().get_display() == litehtml::display_none ||
            node->css().get_visibility() != litehtml::visibility_visible) return false;
    const auto box = element->get_placement();
    return box.width > 0 && box.height > 0;
}

void BrowserApp::focus_form(int index, bool select_all)
{
    if (index < 0 || index >= static_cast<int>(forms_.controls.size()) ||
        !forms_.controls[index].focusable() || !browser_form_visible(forms_.controls[index].element)) return;
    const auto previous = input_mode_ == BrowserInputMode::Page && focused_form_ >= 0 &&
        focused_form_ < static_cast<int>(forms_.controls.size()) ? forms_.controls[focused_form_].element : nullptr;
    auto &control = forms_.controls[index];
    const auto target = control.element;
    focused_form_ = index;
    input_mode_ = BrowserInputMode::Page;
    page_len_ = std::min(control.value.size(), sizeof(page_input_) - 1);
    while (page_len_ > 0 && page_len_ < static_cast<int>(control.value.size()) &&
           (static_cast<unsigned char>(control.value[page_len_]) & 0xc0) == 0x80) --page_len_;
    std::memcpy(page_input_, control.value.data(), page_len_);
    page_input_[page_len_] = 0;
    page_cursor_ = page_len_;
    page_anchor_ = select_all ? 0 : page_cursor_;
    const auto box = control.element->get_placement();
    if (box.y < scroll_y_) scroll_y_ = std::max(0, static_cast<int>(box.y) - 8);
    else if (box.bottom() > scroll_y_ + content_height())
        scroll_y_ = static_cast<int>(box.bottom()) - content_height() + 8;
    clamp_scroll();
    invalidate_content();
    update_ime_context();
    if (previous != target)
    {
        if (previous) { dispatch_script(previous, "change"); dispatch_script(previous, "blur"); }
        dispatch_script(target, "focus");
    }
}

void BrowserApp::tab_form(int direction)
{
    std::vector<int> order;
    for (std::size_t i = 0; i < forms_.controls.size(); ++i)
    {
        const auto &control = forms_.controls[i];
        if (control.focusable() && control.tab_index >= 0 && browser_form_visible(control.element)) order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const int x = forms_.controls[a].tab_index, y = forms_.controls[b].tab_index;
        return x > 0 && y == 0 ? true : x == 0 && y > 0 ? false : x < y;
    });
    if (order.empty()) return;
    auto current = std::find(order.begin(), order.end(), input_mode_ == BrowserInputMode::Page ? focused_form_ : -1);
    const int start = current == order.end() ? (direction > 0 ? -1 : 0) : current - order.begin();
    focus_form(order[(start + direction + static_cast<int>(order.size())) % order.size()]);
}

void BrowserApp::submit_form(int index, bool dispatch_event)
{
    if (index < 0 || index >= static_cast<int>(forms_.controls.size())) return;
    const auto target = forms_.controls[index].element, form = forms_.controls[index].form;
    if (dispatch_event && form && dispatch_script(form, "submit")) return;
    const auto updated = forms_.index_of(target);
    if (updated >= forms_.controls.size()) return;
    index = static_cast<int>(updated);
    const auto submission = forms_.submit(index, current_url_, base_url_);
    if (submission.ok)
    {
        if (processing_scripts_ || loading_) { script_navigation_ = submission.url; script_replace_ = false; }
        else request_navigation(submission.url);
    }
    else
    {
        network_notice_ = submission.error;
        notice_dismissed_ = false;
        invalidate_all();
    }
}

void BrowserApp::activate_form(int index)
{
    using browser_forms::Kind;
    if (index < 0 || index >= static_cast<int>(forms_.controls.size())) return;
    const auto target = forms_.controls[index].element;
    if (forms_.controls[index].disabled) return;
    focus_form(index);
    if (load_pending_) return;
    if (dispatch_script(target, "click")) return;
    const auto updated = forms_.index_of(target);
    if (updated >= forms_.controls.size()) return;
    index = static_cast<int>(updated);
    const auto kind = forms_.controls[index].kind;
    if (kind == Kind::Submit) { submit_form(index); return; }
    forms_.activate(index);
    if (kind == Kind::Checkbox || kind == Kind::Radio || kind == Kind::Select)
    { dispatch_script(target, "input"); dispatch_script(target, "change"); }
    // Model activation handles checkboxes/radios/reset; refresh displayed value.
    invalidate_content();
}

void BrowserApp::draw_form_control(HDLE hdc, const litehtml::element::ptr &element,
                                   const litehtml::position &position, const litehtml::position *clip)
{
    const auto index = forms_.index_of(element);
    if (index >= forms_.controls.size()) return;
    const auto &control = forms_.controls[index];
    using browser_forms::Kind;
    if (control.kind == Kind::Hidden || element->css().get_visibility() != litehtml::visibility_visible) return;
    auto effective_clip = container_.current_clip();
    if (clip) effective_clip = effective_clip.intersect(*clip);
    const browser_ui::Rect box {static_cast<int>(position.x), static_cast<int>(position.y),
                               static_cast<int>(position.width), static_cast<int>(position.height)};
    const int left = static_cast<int>(std::ceil(effective_clip.x));
    const int top = static_cast<int>(std::ceil(effective_clip.y));
    const browser_ui::Rect clipped = box.intersect({left, top,
        std::max(0, static_cast<int>(std::floor(effective_clip.right())) - left),
        std::max(0, static_cast<int>(std::floor(effective_clip.bottom())) - top)});
    if (clipped.empty()) return;
    const bool focused = input_mode_ == BrowserInputMode::Page && focused_form_ == static_cast<int>(index);
    const bool editable = control.editable();
    const auto style = container_.font_style(element->css().get_font());
    XTextMetrics font_metrics {};
    if (xapi_GetTextMetrics(&style, nullptr, &font_metrics) < 0) return;
    const int line_height = std::max(1, font_metrics.line_height);
    const auto color = control.disabled ? 0x87909fff : litehtml_color(element->css().get_color());
    form_background_.resize(static_cast<std::size_t>(clipped.w) * clipped.h);
    xapi_ReadBufferA(hdc, clipped.x, clipped.y, clipped.w, clipped.h, form_background_.data());
    form_canvas_.begin(clipped, 0xffffffff, &form_background_);
    if (focused)
    {
        form_canvas_.fill({box.x, box.y, box.w, 1}, 0x255fdbff);
        form_canvas_.fill({box.x, box.bottom() - 1, box.w, 1}, 0x255fdbff);
    }
    std::string label = control.value;
    if (control.kind == Kind::Checkbox || control.kind == Kind::Radio)
    {
        form_canvas_.round(box.inset(2), control.kind == Kind::Radio ? 8 : 2, 0x8793a5ff);
        form_canvas_.round(box.inset(3), control.kind == Kind::Radio ? 7 : 1, 0xffffffff);
        if (control.checked) form_canvas_.round(box.inset(5), 2, 0x255fdbff);
        form_canvas_.present(hdc);
        return;
    }
    if (control.kind == Kind::Select)
        for (const auto &option : control.options) if (option.selected) { label = option.label; break; }
    if (control.kind == Kind::Submit || control.kind == Kind::Button || control.kind == Kind::Reset)
    {
        if (std::strcmp(element->get_tagName(), "button") == 0) { label.clear(); element->get_text(label); }
        if (label.empty()) label = control.kind == Kind::Reset ? "Reset" : "Submit";
    }
    if (control.kind == Kind::Password)
    {
        label.clear();
        for (std::size_t offset = 0; offset < control.value.size();)
        {
            label += '*';
            offset += std::max<std::size_t>(1, ime_utf8_scalar_bytes(control.value.data() + offset, control.value.size() - offset));
        }
    }
    int caret = 0;
    if (editable && control.kind != Kind::Textarea)
    {
        int cursor = focused ? page_cursor_ : 0;
        if (control.kind == Kind::Password)
        {
            int masked_cursor = 0;
            for (std::size_t offset = 0; offset < control.value.size() && static_cast<int>(offset) < cursor;)
            {
                offset += std::max<std::size_t>(1, ime_utf8_scalar_bytes(control.value.data() + offset,
                                                                        control.value.size() - offset));
                ++masked_cursor;
            }
            cursor = masked_cursor;
        }
        const auto view = browser_ui::input_view(label.c_str(), label.size(), cursor, std::max(0, box.w - 8), style);
        if (focused && page_cursor_ != page_anchor_ && control.kind != Kind::Password)
        {
            const int start = std::clamp(std::min(page_cursor_, page_anchor_) - view.first, 0, static_cast<int>(view.text.size()));
            const int end = std::clamp(std::max(page_cursor_, page_anchor_) - view.first, start, static_cast<int>(view.text.size()));
            const int left = browser_ui::text_position(view.text, start, style);
            const int right = browser_ui::text_position(view.text, end, style);
            form_canvas_.fill({box.x + 4 + left, box.y + 2, right - left, box.h - 4}, 0xcfe0ffff);
        }
        label = view.text;
        caret = view.caret;
    }
    if (label.empty() && editable && !focused) label = element->get_attr("placeholder", "");
    int caret_y = box.y + 3;
    if (control.kind == Kind::Textarea)
    {
        std::size_t offset = 0;
        for (int y = box.y + 3; offset <= label.size() && y < box.bottom(); y += line_height)
        {
            const auto end = label.find('\n', offset);
            const auto line_end = end == std::string::npos ? label.size() : end;
            const auto line = label.substr(offset, line_end - offset);
            if (focused && page_cursor_ >= static_cast<int>(offset) && page_cursor_ <= static_cast<int>(line_end))
            {
                caret = browser_ui::text_position(line, page_cursor_ - offset, style);
                caret_y = y;
            }
            form_canvas_.text_with_style({box.x + 4, y, std::max(0, box.w - 8), line_height}, line, color, style);
            if (end == std::string::npos) break;
            offset = end + 1;
        }
    }
    else form_canvas_.text_with_style({box.x + 4, box.y + 2, std::max(0, box.w - 8), std::max(0, box.h - 4)},
                                      label, color, style);
    if (focused && editable)
        form_canvas_.fill({box.x + 4 + std::min(std::max(0, box.w - 9), caret), caret_y, 1,
                            std::max(0, std::min(line_height, box.bottom() - caret_y - 3))}, 0x255fdbff);
    form_canvas_.present(hdc);
}
