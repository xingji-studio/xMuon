#pragma once

// Module compilation never performs IO. This adapter supplies validated source
// from the same cooperative queue used by CSS/images and yields between fetches.
static bool browser_module_mime(const std::string &input)
{
    const auto type = lower_copy(trim_copy(input.substr(0, input.find(';'))));
    for (const char *allowed : {"application/ecmascript", "application/javascript", "application/x-ecmascript",
             "application/x-javascript", "text/ecmascript", "text/javascript", "text/javascript1.0",
             "text/javascript1.1", "text/javascript1.2", "text/javascript1.3", "text/javascript1.4",
             "text/javascript1.5", "text/jscript", "text/livescript", "text/x-ecmascript", "text/x-javascript"})
        if (type == allowed) return true;
    return false;
}

static bool browser_http_url(const std::string &url)
{
    const auto scheme = lower_copy(url.substr(0, url.find(':')));
    return scheme == "http" || scheme == "https";
}

static bool browser_connected_node(const litehtml::element::ptr &node, const litehtml::document::ptr &document)
{
    if (!document) return false;
    for (auto current = node; current; current = current->parent())
        if (current == document->root()) return true;
    return false;
}

static BrowserScriptResponse browser_module_response(const std::string &url, const std::string &document_url,
                                                     const BrowserFetchResult *response)
{
    BrowserScriptResponse result;
    if (!browser_http_url(url) || !browser_same_origin(url, document_url))
        result.error = "Only same-origin HTTP(S) ES modules are supported";
    else if (!response || !response->ok || response->status_code < 200 || response->status_code >= 300)
        result.error = response && !response->error.empty() ? response->error : "Module request failed";
    else
    {
        result.final_url = response->final_url.empty() ? url : response->final_url;
        if (!browser_http_url(result.final_url) || !browser_same_origin(document_url, result.final_url))
            result.error = "Cross-origin ES module redirect blocked";
        else if (!browser_module_mime(response->content_type)) result.error = "Invalid JavaScript module MIME type";
        else if (response->body.size() > 2 * 1024 * 1024) result.error = "Module source budget exceeded";
        else
        {
            result.status = response->status_code;
            result.content_type = response->content_type;
            result.body = response->body;
        }
    }
    return result;
}

bool BrowserApp::start_module_script(DeferredScript &program)
{
    if (program.module_entry) return true;
    const auto reject = [&](const std::string &error) {
        log_line("Module load failed: " + program.url + ": " + error);
        program.applied = true;
        script_runtime_->dispatch(program.node, "error");
        script_runtime_->release_node(program.node);
        return false;
    };
    if (!program.integrity.empty()) return reject("Module integrity metadata is not supported");
    std::string source = program.source, base = program.base;
    if (!program.url.empty())
    {
        if (!browser_http_url(program.url) || !browser_same_origin(current_url_, program.url))
            return reject("Only same-origin HTTP(S) ES modules are supported");
        const auto *response = resource_response(program.url);
        if (!response)
        {
            if (!enqueue_resource(program.url, BrowserScript, 1)) return reject("Module resource budget exceeded");
            return false;
        }
        const auto validated = browser_module_response(program.url, current_url_, response);
        if (!validated.error.empty()) return reject(validated.error);
        source = validated.body;
        base = validated.final_url;
    }
    if (source.size() > 2 * 1024 * 1024 || executed_scripts_ >= 128 ||
        script_bytes_ + source.size() > 8 * 1024 * 1024) return reject("Script execution budget exceeded");
    ++executed_scripts_;
    script_bytes_ += source.size();
    script_runtime_->set_current_script(nullptr);
    program.module_entry = script_runtime_->start_module(source,
        program.url.empty() ? current_url_ : program.url, base, program.url.empty());
    if (!program.module_entry) return reject(script_runtime_->last_error());
    return true;
}

void BrowserApp::pump_module_requests()
{
    if (!script_runtime_) return;
    for (auto &request : script_runtime_->take_module_requests())
    {
        BrowserScriptResponse error;
        if (module_fetch_cancelled_) error.error = "Module request cancelled";
        else if (!browser_http_url(request.url) || !browser_same_origin(current_url_, request.url))
            error.error = "Only same-origin HTTP(S) ES modules are supported";
        else if (module_transfers_.size() >= 128 || !enqueue_resource(request.url, BrowserScript, 1))
            error.error = "Module resource budget exceeded";
        if (!error.error.empty()) script_runtime_->provide_module(request.token, error);
        else module_transfers_.push_back({request.token, std::move(request.url), std::move(request.referrer)});
    }
    unsigned supplied = 0;
    for (std::size_t i = 0; i < module_transfers_.size() && supplied < 8;)
    {
        const auto transfer = module_transfers_[i];
        if (module_fetch_cancelled_)
        {
            module_transfers_.erase(module_transfers_.begin() + i);
            BrowserScriptResponse error;
            error.error = "Module request cancelled";
            script_runtime_->provide_module(transfer.token, error);
            ++supplied;
            continue;
        }
        const auto *response = resource_response(transfer.url);
        if (!response)
        {
            if (enqueue_resource(transfer.url, BrowserScript, 1)) { ++i; continue; }
        }
        const auto validated = browser_module_response(transfer.url, current_url_, response);
        module_transfers_.erase(module_transfers_.begin() + i);
        ++supplied;
        script_runtime_->set_current_script(nullptr);
        script_runtime_->provide_module(transfer.token, validated);
    }
}

void BrowserApp::queue_script_link(const litehtml::element::ptr &node)
{
    if (!script_runtime_ || !browser_connected_node(node, document_)) return;
    const std::string rel = lower_copy(node->get_attr("rel", ""));
    const bool stylesheet = browser_html_has_stylesheet_rel(rel.c_str());
    bool preload = false;
    std::size_t start = 0;
    while (start < rel.size())
    {
        const auto end = rel.find_first_of(" \t\r\n\f", start);
        if (rel.substr(start, end == std::string::npos ? end : end - start) == "modulepreload") preload = true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if ((!stylesheet && !preload) || !*node->get_attr("href", "") || node->get_attr("disabled")) return;
    const auto url = resolve_url(node->get_attr("href", ""), base_url_);
    for (const auto &link : script_links_) if (link.node == node && link.url == url) return;
    if (script_links_.size() >= 128) return;
    script_runtime_->retain_node(node);
    script_links_.push_back({node, url, stylesheet, 0});
    if (browser_http_url(url) && (stylesheet || browser_same_origin(current_url_, url)))
        enqueue_resource(url, stylesheet ? BrowserStylesheet : BrowserScript, stylesheet ? 0 : 1);
}

void BrowserApp::pump_script_links()
{
    unsigned completed = 0;
    for (std::size_t i = 0; i < script_links_.size() && completed < 8;)
    {
        auto link = script_links_[i];
        bool ready = false, okay = false;
        if (!browser_connected_node(link.node, document_) ||
            resolve_url(link.node->get_attr("href", ""), base_url_) != link.url)
        {
            script_links_.erase(script_links_.begin() + i);
            script_runtime_->release_node(link.node);
            continue;
        }
        if (module_fetch_cancelled_ || !browser_http_url(link.url) ||
            (!link.stylesheet && !browser_same_origin(current_url_, link.url)) ||
            *link.node->get_attr("integrity", "")) ready = true;
        else if (const auto *response = resource_response(link.url))
        {
            ready = true;
            if (link.stylesheet)
            {
                okay = response->ok && response->status_code >= 200 && response->status_code < 300 &&
                    lower_copy(trim_copy(response->content_type.substr(0, response->content_type.find(';')))) == "text/css";
                if (okay && (resource_jobs_.pending(BrowserStylesheet) || rebuild_pending_)) ready = false;
            }
            else
            {
                if (!link.module_entry)
                {
                    const auto validated = browser_module_response(link.url, current_url_, response);
                    if (validated.error.empty())
                    {
                        link.module_entry = script_runtime_->preload_module(validated.body, link.url, validated.final_url);
                        script_links_[i].module_entry = link.module_entry;
                    }
                }
                if (link.module_entry)
                {
                    const auto state = script_runtime_->module_state(link.module_entry);
                    ready = state != BrowserScriptModuleState::Loading;
                    okay = ready && state != BrowserScriptModuleState::Rejected && state != BrowserScriptModuleState::Unknown;
                }
            }
        }
        if (!ready) { ++i; continue; }
        script_links_.erase(script_links_.begin() + i);
        ++completed;
        script_runtime_->dispatch(link.node, okay ? "load" : "error");
        script_runtime_->release_node(link.node);
    }
}
