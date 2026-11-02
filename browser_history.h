#pragma once

BrowserScriptViewport BrowserApp::script_viewport() const
{
    BrowserScriptViewport viewport;
    viewport.width = content_width();
    viewport.height = content_height();
    viewport.outer_width = window_width_;
    viewport.outer_height = window_height_;
    viewport.scroll_y = scroll_y_;
    viewport.scroll_width = std::max<double>(viewport.width, document_ ? document_->width() : viewport.width);
    viewport.scroll_height = std::max<double>(viewport.height, scroll_content_height());
    return viewport;
}

bool BrowserApp::update_script_history(const std::string &target, bool replace, const std::string &state)
{
    if (!document_ || target.size() > 8192 || state.size() > 256 * 1024 ||
        !browser_http_url(target) || !browser_same_origin(current_url_, target)) return false;
    std::size_t bytes = state.size();
    for (const auto &entry : history_states_) bytes += entry.script_state.size();
    for (const auto &entry : script_history_updates_) bytes += entry.state.size();
    if (bytes > 4 * 1024 * 1024 || (!replace && history_.size() + script_history_updates_.size() >= 512))
        return false;
    if (loading_)
    {
        // Classic startup scripts run inside a transactional page load. Keep
        // their history mutations with that transaction until the page commits.
        if (script_history_updates_.size() >= 128) return false;
        if (script_history_updates_.empty()) script_history_base_ = current_url_;
        script_history_updates_.push_back({target, state, replace});
        loading_history_state_ = state;
        current_url_ = target;
    }
    else
    {
        save_history_state();
        record_history(target, replace, true);
        history_states_[history_index_].script_state = state;
    }
    if (!base_url_set_) base_url_ = target;
    std::snprintf(url_input_, sizeof(url_input_), "%s", target.c_str());
    input_len_ = input_cursor_ = input_anchor_ = std::strlen(url_input_);
    invalidate_header();
    return true;
}

void BrowserApp::flush_script_history()
{
    auto changes = std::move(script_history_updates_);
    script_history_updates_.clear();
    script_history_base_.clear();
    for (const auto &change : changes)
    {
        record_history(change.url, change.replace, true);
        if (history_index_ >= 0) history_states_[history_index_].script_state = change.state;
    }
}

void BrowserApp::flush_script_effects()
{
    if (loading_ || processing_scripts_ || !script_runtime_) return;
    if (script_scroll_pending_)
    {
        script_scroll_pending_ = false;
        const int previous = scroll_y_;
        scroll_y_ = static_cast<int>(std::clamp(script_scroll_y_, 0.0, 1000000000.0));
        clamp_scroll();
        if (previous != scroll_y_)
        {
            invalidate_content();
            BrowserScriptScope scope(processing_scripts_);
            script_runtime_->view_changed("scroll");
        }
    }
    if (script_history_go_pending_)
    {
        script_history_go_pending_ = false;
        const int delta = script_history_delta_;
        script_history_delta_ = 0;
        if (delta) go_history(delta);
        else reload_current();
    }
}
