#pragma once

#include <litehtml.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct BrowserScriptResponse
{
    int status = 0;
    std::string body, final_url, content_type, error;
};

struct BrowserScriptViewport
{
    double width = 0, height = 0, outer_width = 0, outer_height = 0;
    double scroll_x = 0, scroll_y = 0, scroll_width = 0, scroll_height = 0;
};

struct BrowserScriptModuleRequest
{
    unsigned long long token = 0;
    std::string url, referrer;
};

enum class BrowserScriptModuleState
{
    Unknown,
    Loading,
    Evaluating,
    Fulfilled,
    Rejected
};

// The embedding owns network policy, navigation and native form state. No
// operating-system or filesystem bindings are exposed to page JavaScript.
struct BrowserScriptHost
{
    std::function<void(const std::string &, bool)> navigate;
    std::function<unsigned long long()> monotonic_ms;
    std::function<std::string(const std::string &)> fetch;
    std::function<BrowserScriptResponse(const std::string &, const std::string &)> request;
    std::function<std::string(const litehtml::element::ptr &, const std::string &)> get_control;
    std::function<void(const litehtml::element::ptr &, const std::string &, const std::string &)> set_control;
    std::function<void(const litehtml::element::ptr &)> focus;
    std::function<void(const litehtml::element::ptr &)> submit;
    std::function<std::string()> get_cookie;
    std::function<void(const std::string &)> set_cookie;
    std::function<void(const std::string &)> log;
    std::function<void(const litehtml::element::ptr &)> script_added;
    std::function<void(const litehtml::element::ptr &)> link_added;
    std::function<std::string()> current_url;
    // State is an opaque, bounded structured-clone encoding owned by the web
    // platform layer. These callbacks never enter the VM recursively.
    std::function<bool(const std::string &, bool, const std::string &)> history_update;
    std::function<std::string()> history_state;
    std::function<unsigned()> history_length;
    std::function<void(int)> history_go;
    std::function<BrowserScriptViewport()> viewport;
    std::function<void(double, double)> scroll_to;
};

struct BrowserScriptMemoryStatistics
{
    std::size_t used = 0, peak = 0, allocation_failures = 0;
    std::size_t collections = 0, last_collection_before = 0, last_collection_after = 0;
    std::size_t large_live_bytes = 0, large_peak_bytes = 0, large_live_count = 0;
    std::size_t largest_successful_allocation = 0, largest_allocation_request = 0;
    // Allocator blocks: [64,128) KiB through [4,8) MiB, then >=8 MiB.
    std::size_t large_bucket_bytes[8] = {}, large_bucket_counts[8] = {};
};

// This detailed scan is separate from cheap counters and is called only between
// VM executions. Upstream omits rope strings and some internal classes, so the
// classification is partial. Binary objects are serialization buffers, not ArrayBuffers.
struct BrowserScriptMemoryBreakdown
{
    std::size_t allocated = 0, accounted = 0, objects = 0, object_bytes = 0;
    std::size_t strings = 0, string_bytes = 0, atoms = 0, atom_bytes = 0;
    std::size_t properties = 0, property_bytes = 0, shapes = 0, shape_bytes = 0;
    std::size_t functions = 0, function_bytes = 0, bytecode_bytes = 0;
    std::size_t arrays = 0, fast_array_elements = 0, binary_objects = 0, binary_object_bytes = 0;
};

class BrowserScriptRuntime
{
public:
    BrowserScriptRuntime();
    ~BrowserScriptRuntime();
    BrowserScriptRuntime(const BrowserScriptRuntime &) = delete;
    BrowserScriptRuntime &operator=(const BrowserScriptRuntime &) = delete;

    void attach(const litehtml::document::ptr &document, const std::string &url, BrowserScriptHost host);
    void set_current_script(const litehtml::element::ptr &node);
    bool evaluate(const std::string &source, const std::string &url = "<inline>");
    // Module source is compiled without fetching or executing dependencies.
    // The host drains requests, validates fetch policy/MIME, then provides each
    // response. base_url is the final response URL (or document base for inline
    // modules); url is the external module-map key. Inline entries are distinct.
    unsigned long long start_module(const std::string &source, const std::string &url,
                                    const std::string &base_url = "", bool inline_module = false);
    unsigned long long preload_module(const std::string &source, const std::string &url,
                                      const std::string &base_url = "");
    std::vector<BrowserScriptModuleRequest> take_module_requests();
    void provide_module(unsigned long long token, const BrowserScriptResponse &response);
    BrowserScriptModuleState module_state(unsigned long long entry) const;
    bool module_evaluation_started(unsigned long long entry) const;
    // Fetch/import queues and runnable VM jobs; an otherwise idle top-level
    // await in an HTML module entry does not keep native resources pending.
    bool module_work_pending() const;
    std::string module_error(unsigned long long entry) const;
    void history_changed(const std::string &url, const std::string &state_json);
    void view_changed(const std::string &type);
    // Returns true when the default browser action was prevented.
    bool dispatch(const litehtml::element::ptr &target, const std::string &type,
                  const std::string &key = "", int key_code = 0);
    void lifecycle(const std::string &type);
    void pump(unsigned long long now_ms);
    void retain_node(const litehtml::element::ptr &node);
    void release_node(const litehtml::element::ptr &node);
    void collect_garbage();
    std::size_t live_nodes();
    std::size_t memory_bytes() const;
    std::size_t allocation_failures() const;
    BrowserScriptMemoryStatistics memory_statistics() const;
    BrowserScriptMemoryBreakdown memory_breakdown() const;
    bool take_dirty();
    const std::string &last_error() const;
    const std::string &written_html() const;
    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
