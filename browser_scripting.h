#pragma once

// BrowserApp integration: page JavaScript runs only on the browser event thread.
// Host callbacks queue navigation and inserted scripts, never reenter the VM.
struct BrowserScriptScope
{
    bool &busy;
    bool previous;
    explicit BrowserScriptScope(bool &value) : busy(value), previous(value) { busy = true; }
    ~BrowserScriptScope() { busy = previous; }
};

static bool browser_same_origin(const std::string &left, const std::string &right)
{
    browser_url_detail::Parts a, b;
    if (!browser_url_detail::parse(left, a) || !browser_url_detail::parse(right, b) ||
        !a.has_authority || !b.has_authority) return false;
    auto origin = [](const browser_url_detail::Parts &parts) {
        const auto scheme = lower_copy(parts.scheme);
        auto authority = lower_copy(parts.authority);
        const std::string port = scheme == "http" ? ":80" : scheme == "https" ? ":443" : "";
        if (!port.empty() && authority.size() >= port.size() &&
            authority.compare(authority.size() - port.size(), port.size(), port) == 0)
            authority.resize(authority.size() - port.size());
        return scheme + "://" + authority;
    };
    return origin(a) == origin(b);
}

#include "browser_modules.h"
#include "browser_history.h"

void BrowserApp::execute_script(const std::string &source, const std::string &url)
{
    if (!script_runtime_ || source.empty() || source.size() > 2 * 1024 * 1024 ||
        executed_scripts_ >= 128 || script_bytes_ + source.size() > 8 * 1024 * 1024) return;
    ++executed_scripts_;
    script_bytes_ += source.size();
    if (!script_runtime_->evaluate(source, url))
        log_line("JavaScript " + shorten_url_for_log(url) + ": " + script_runtime_->last_error());
}

void BrowserApp::initialize_scripts(const std::string &url)
{
    if (!document_) return;
    BrowserScriptHost host;
    host.monotonic_ms = [] { return browser_http_now_ms(); };
    host.current_url = [this] { return current_url_; };
    host.history_update = [this](const std::string &target, bool replace, const std::string &state) {
        return update_script_history(target, replace, state);
    };
    host.history_state = [this] {
        if (loading_) return loading_history_state_;
        return history_index_ >= 0 && history_index_ < static_cast<int>(history_states_.size()) ?
            history_states_[history_index_].script_state : std::string("null");
    };
    host.history_length = [this] { return static_cast<unsigned>(history_.size()); };
    host.history_go = [this](int delta) { script_history_delta_ = delta; script_history_go_pending_ = true; };
    host.viewport = [this] { return script_viewport(); };
    host.scroll_to = [this](double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        script_scroll_x_ = x; script_scroll_y_ = y; script_scroll_pending_ = true;
    };
    host.link_added = [this](const litehtml::element::ptr &node) { queue_script_link(node); };
    host.navigate = [this](const std::string &target, bool replace) {
        const auto resolved = resolve_url(target, base_url_);
        if (!resolved.empty())
        { script_submission_.reset(); script_navigation_ = resolved; script_replace_ = replace; }
    };
    host.request = [this, url](const std::string &target, const std::string &method) {
        BrowserScriptResponse result;
        const auto resolved = resolve_url(target, url);
        if (method != "GET") { result.error = "Only GET script requests are supported."; return result; }
        if (resolved.empty() || !browser_same_origin(url, resolved))
        { result.error = "Cross-origin script requests are not supported."; return result; }
        const auto response = fetch_resource(resolved);
        if (!response.ok)
        { result.error = response.error.empty() ? "Request failed." : response.error; return result; }
        result.final_url = response.final_url.empty() ? resolved : response.final_url;
        if (!browser_same_origin(url, result.final_url))
        { result.error = "Cross-origin redirect response blocked."; result.final_url.clear(); return result; }
        result.status = response.status_code;
        result.body = response.body;
        result.content_type = response.content_type;
        return result;
    };
    host.get_control = [this](const litehtml::element::ptr &node, const std::string &property) {
        const auto index = forms_.index_of(node);
        if (index < forms_.controls.size() && forms_.controls[index].element == node)
        {
            const auto &control = forms_.controls[index];
            if (property == "value") return control.value;
            if (property == "checked") return std::string(control.checked ? "true" : "false");
        }
        if (property == "checked" || property == "selected")
            return std::string(node->get_attr(property.c_str()) ? "true" : "false");
        if (property == "value" && std::strcmp(node->get_tagName(), "textarea") == 0)
        {
            std::string value;
            node->get_text(value);
            return std::string(node->get_attr("data-xj380-value", value.c_str()));
        }
        return std::string(node->get_attr(property.c_str(), ""));
    };
    host.set_control = [this](const litehtml::element::ptr &node, const std::string &property,
                              const std::string &value) {
        if (value.size() > sizeof(page_input_) - 1) return;
        if (property == "checked" || property == "selected")
        {
            if (value == "true" || value == "1") node->set_attr(property.c_str(), "");
            else node->remove_attr(property.c_str());
        }
        else node->set_attr(property.c_str(), value.c_str());
        if (property == "value" && std::strcmp(node->get_tagName(), "textarea") == 0)
            node->set_attr("data-xj380-value", value.c_str());
        const auto index = forms_.index_of(node);
        if (index >= forms_.controls.size() || forms_.controls[index].element != node) return;
        auto &control = forms_.controls[index];
        if (property == "value")
        {
            control.value = value;
            if (control.kind == browser_forms::Kind::Textarea)
                node->set_attr("data-xj380-value", value.c_str());
            if (input_mode_ == BrowserInputMode::Page && focused_form_ == static_cast<int>(index))
            {
                std::snprintf(page_input_, sizeof(page_input_), "%s", value.c_str());
                page_len_ = std::strlen(page_input_);
                page_cursor_ = page_anchor_ = page_len_;
            }
        }
        if (property == "checked")
        {
            control.checked = value == "true" || value == "1";
            node->set_attr("data-xj380-checked", control.checked ? "true" : "false");
        }
        invalidate_content();
    };
    host.focus = [this](const litehtml::element::ptr &node) {
        const auto index = forms_.index_of(node);
        if (index < forms_.controls.size()) focus_form(index);
    };
    host.submit = [this](const litehtml::element::ptr &node) {
        script_submission_ = node;
        script_navigation_.clear();
    };
    host.get_cookie = [this] { return browser_document_cookie(current_url_); };
    host.set_cookie = [this](const std::string &value) { browser_set_document_cookie(current_url_, value); };
    host.log = [this](const std::string &message) { log_line("JavaScript: " + message.substr(0, 2048)); };
    host.script_added = [this](const litehtml::element::ptr &node) {
        if (!browser_connected_node(node, document_)) return;
        started_script_nodes_.erase(std::remove_if(started_script_nodes_.begin(), started_script_nodes_.end(),
            [](const auto &weak) { return weak.expired(); }), started_script_nodes_.end());
        for (const auto &weak : started_script_nodes_) if (weak.lock() == node) return;
        if (std::find(dynamic_scripts_.begin(), dynamic_scripts_.end(), node) == dynamic_scripts_.end())
        {
            if (dynamic_scripts_.size() >= 128) return;
            dynamic_scripts_.push_back(node);
            script_runtime_->retain_node(node);
        }
        const std::string source = node->get_attr("src", "");
        if (!source.empty() && is_executable_script_type(node->get_attr("type", "")))
        {
            const auto target = resolve_url(source, base_url_);
            if (!is_module_script_type(node->get_attr("type", "")) || browser_same_origin(current_url_, target))
                enqueue_resource(target, BrowserScript, 1);
        }
    };
    script_runtime_ = std::make_unique<BrowserScriptRuntime>();
    script_runtime_->attach(document_, url, std::move(host));
    script_runtime_->pump(browser_http_now_ms());
    std::vector<litehtml::element::ptr> script_nodes;
    for (const auto &node : document_->root()->select_all("script"))
    {
        if (!is_executable_script_type(node->get_attr("type", "")) ||
            (node->get_attr("nomodule") && !is_module_script_type(node->get_attr("type", "")))) continue;
        bool inert = false;
        for (auto parent = node->parent(); parent; parent = parent->parent())
            if (std::strcmp(parent->get_tagName(), "template") == 0 ||
                std::strcmp(parent->get_tagName(), "noscript") == 0) inert = true;
        if (!inert) script_nodes.push_back(node);
    }
    for (std::size_t i = 0; i < deferred_scripts_.size() && i < script_nodes.size(); ++i)
    {
        deferred_scripts_[i].node = script_nodes[i];
        script_runtime_->retain_node(script_nodes[i]);
        started_script_nodes_.push_back(script_nodes[i]);
    }
    BrowserScriptScope script_scope(processing_scripts_);
    for (auto &program : deferred_scripts_)
    {
        if (program.deferred || program.async || program.applied) continue;
        program.applied = true;
        std::string source = program.source;
        if (!program.url.empty())
        {
            const auto response = fetch_resource(program.url);
            if (response.ok && response.status_code >= 200 && response.status_code < 300) source = response.body;
            else
            {
                log_line("Script load failed: " + program.url);
                script_runtime_->dispatch(program.node, "error");
                script_runtime_->release_node(program.node);
                continue;
            }
        }
        if (cancelled_ || navigation_serial_ != loading_serial_) break;
        script_runtime_->set_current_script(program.node);
        execute_script(source, program.url.empty() ? url : program.url);
        script_runtime_->set_current_script(nullptr);
        if (!program.url.empty()) script_runtime_->dispatch(program.node, "load");
        script_runtime_->release_node(program.node);
        if (!script_navigation_.empty()) break;
    }
    processing_scripts_ = false;
    sync_script_document();
    flush_script_submission();
    flush_script_effects();
}

void BrowserApp::flush_script_submission()
{
    if (!script_submission_) return;
    const auto node = std::move(script_submission_);
    script_submission_.reset();
    if (std::strcmp(node->get_tagName(), "form") == 0)
    {
        const auto submission = forms_.submit_owner(node, current_url_, base_url_);
        if (submission.ok) { script_navigation_ = submission.url; script_replace_ = false; }
        else { network_notice_ = submission.error; notice_dismissed_ = false; invalidate_header(); }
    }
    else
    {
        const auto index = forms_.index_of(node);
        if (index < forms_.controls.size())
        {
            BrowserScriptScope script_scope(processing_scripts_);
            submit_form(index, false);
            processing_scripts_ = false;
        }
    }
}

void BrowserApp::sync_script_document()
{
    if (!script_runtime_ || !document_ || !script_runtime_->take_dirty()) return;
    auto old_controls = forms_.controls;
    litehtml::element::ptr focused;
    if (input_mode_ == BrowserInputMode::Page && focused_form_ >= 0 &&
        focused_form_ < static_cast<int>(old_controls.size())) focused = old_controls[focused_form_].element;
    BrowserParserScope parser;
    css_import_count_ = 0;
    document_->rebuild();
    forms_.collect(document_->root());
    bool identities_changed = old_controls.size() != forms_.controls.size();
    for (std::size_t i = 0; !identities_changed && i < old_controls.size(); ++i)
        identities_changed = old_controls[i].element != forms_.controls[i].element;
    if (identities_changed) ++form_generation_;
    // Reflow must preserve reset defaults and live values on existing nodes.
    browser_preserve_form_defaults(old_controls, forms_);
    document_->render(static_cast<float>(content_width()));
    page_text_.clear(); text_runs_.clear();
    collect_text(document_->root()); update_find_matches();
    if (focused)
    {
        const auto index = forms_.index_of(focused);
        if (index < forms_.controls.size()) focused_form_ = index;
        else { focused_form_ = -1; input_mode_ = BrowserInputMode::None; }
    }
    update_ime_context();
    clamp_scroll();
    prioritize_visible_images();
    invalidate_content();
}

bool BrowserApp::dispatch_script(const litehtml::element::ptr &target, const std::string &type,
                                  const std::string &key, int key_code)
{
    if (!script_runtime_ || processing_scripts_) return false;
    BrowserScriptScope script_scope(processing_scripts_);
    const bool prevented = script_runtime_->dispatch(target, type, key, key_code);
    pump_module_requests();
    processing_scripts_ = false;
    sync_script_document();
    flush_script_submission();
    flush_script_effects();
    const bool navigation = !script_navigation_.empty();
    if (navigation && !loading_)
    {
        const auto url = std::move(script_navigation_);
        script_navigation_.clear();
        navigate_to(url, script_replace_ ? BrowserNavReason::Replace : BrowserNavReason::Normal);
    }
    return prevented || navigation;
}

void BrowserApp::pump_scripts()
{
    if (!script_runtime_ || processing_scripts_ || loading_ || load_pending_) return;
    BrowserScriptScope script_scope(processing_scripts_);
    const unsigned serial = navigation_serial_;
    pump_module_requests();
    bool waiting_defer = false;
    for (auto &program : deferred_scripts_)
    {
        if (program.applied) continue;
        if (!program.async && waiting_defer) continue;
        if (program.module)
        {
            if (!start_module_script(program))
            {
                if (!program.applied && !program.async) waiting_defer = true;
                continue;
            }
            const auto state = script_runtime_->module_state(program.module_entry);
            if (state == BrowserScriptModuleState::Loading)
            {
                if (!program.async) waiting_defer = true;
                continue;
            }
            // Starting evaluation unblocks parser-deferred ordering and DCL.
            // A top-level await continues through the native Promise queue.
            program.applied = true;
            const bool started = script_runtime_->module_evaluation_started(program.module_entry);
            if (state == BrowserScriptModuleState::Rejected)
                log_line("Module " + program.url + ": " + script_runtime_->module_error(program.module_entry));
            if (!started || !program.url.empty()) script_runtime_->dispatch(program.node, started ? "load" : "error");
            script_runtime_->release_node(program.node);
            if (!script_navigation_.empty()) break;
            continue;
        }
        const auto *job = resource_jobs_.find(program.url);
        if (job && !job->finished)
        {
            if (!program.async) waiting_defer = true;
            continue;
        }
        program.applied = true;
        const bool fetched = job && job->response.ok && job->response.status_code >= 200 &&
                             job->response.status_code < 300;
        if (fetched)
        {
            script_runtime_->set_current_script(program.node);
            execute_script(job->response.body, program.url);
            script_runtime_->set_current_script(nullptr);
        }
        script_runtime_->dispatch(program.node, fetched ? "load" : "error");
        script_runtime_->release_node(program.node);
        if (!script_navigation_.empty()) break;
    }
    // Dynamic external scripts share the bounded asynchronous resource queue.
    // A slow decorative module must not block other modules or page events.
    unsigned executed = 0;
    for (std::size_t i = 0; i < dynamic_scripts_.size() && executed < 4 && script_navigation_.empty();)
    {
        const auto node = dynamic_scripts_[i];
        if (!is_executable_script_type(node->get_attr("type", "")))
        {
            dynamic_scripts_.erase(dynamic_scripts_.begin() + i);
            script_runtime_->release_node(node);
            continue;
        }
        if (node->get_attr("nomodule") && !is_module_script_type(node->get_attr("type", "")))
        {
            dynamic_scripts_.erase(dynamic_scripts_.begin() + i);
            script_runtime_->release_node(node);
            continue;
        }
        if (is_module_script_type(node->get_attr("type", "")))
        {
            DeferredScript program;
            program.node = node;
            program.module = program.async = true;
            program.parser_inserted = false;
            program.base = base_url_;
            program.integrity = node->get_attr("integrity", "");
            const std::string src = node->get_attr("src", "");
            if (!src.empty()) program.url = resolve_url(src, base_url_);
            else node->get_text(program.source);
            started_script_nodes_.push_back(node);
            deferred_scripts_.push_back(std::move(program));
            dynamic_scripts_.erase(dynamic_scripts_.begin() + i);
            ++executed;
            continue;
        }
        const std::string src = node->get_attr("src", "");
        const auto url = src.empty() ? current_url_ : resolve_url(src, base_url_);
        std::string source;
        bool fetched = true;
        if (src.empty()) node->get_text(source);
        else
        {
            const auto *response = resource_response(url);
            if (!response)
            {
                const auto *job = resource_jobs_.find(url);
                if ((!job || !job->finished) && enqueue_resource(url, BrowserScript, 1)) { ++i; continue; }
                fetched = false;
            }
            else
            {
                fetched = response->ok && response->status_code >= 200 && response->status_code < 300;
                if (fetched) source = response->body;
            }
            if (!fetched) log_line("Script load failed: " + url);
        }
        dynamic_scripts_.erase(dynamic_scripts_.begin() + i);
        started_script_nodes_.push_back(node);
        ++executed;
        script_runtime_->set_current_script(node);
        if (fetched) execute_script(source, url);
        script_runtime_->set_current_script(nullptr);
        script_runtime_->dispatch(node, fetched ? "load" : "error");
        script_runtime_->release_node(node);
    }
    const bool dom_programs_done = std::all_of(deferred_scripts_.begin(), deferred_scripts_.end(),
        [](const auto &script) { return script.applied || script.async || !script.parser_inserted; });
    if (dom_programs_done && !script_dom_ready_ && script_navigation_.empty())
    { script_dom_ready_ = true; script_runtime_->lifecycle("DOMContentLoaded"); }
    script_runtime_->pump(browser_http_now_ms());
    pump_module_requests();
    pump_script_links();
    // A stylesheet load listener can queue import() (as Vite does for lazy
    // routes). Publish that work before deciding whether the queue is idle.
    pump_module_requests();
    const bool all_programs_started = std::all_of(deferred_scripts_.begin(), deferred_scripts_.end(),
        [](const auto &script) { return script.applied; });
    if (script_dom_ready_ && !script_load_ready_ && all_programs_started && !resource_jobs_.pending() &&
        dynamic_scripts_.empty() && script_links_.empty())
    { script_load_ready_ = true; script_runtime_->lifecycle("load"); }
    processing_scripts_ = false;
    if (serial != navigation_serial_ || load_pending_) return;
    sync_script_document();
    flush_script_submission();
    flush_script_effects();
    if (!script_navigation_.empty())
    {
        const auto target = std::move(script_navigation_);
        script_navigation_.clear();
        navigate_to(target, script_replace_ ? BrowserNavReason::Replace : BrowserNavReason::Normal);
    }
}
