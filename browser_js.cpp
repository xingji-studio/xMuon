#include "browser_js.h"
#include "browser_js_bootstrap.h"
#include "browser_url.h"
#include "../../third_party/quickjs/quickjs.h"
#include <litehtml/el_text.h>
#include <litehtml/el_comment.h>
#include <litehtml/html_tag.h>
#include <algorithm>
#include <atomic>
#include <set>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <ctime>
#include <map>
#include <utility>
#include <vector>

namespace
{
unsigned long long module_token()
{
    static std::atomic<unsigned long long> next {1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// Keep allocation sizes independent from the host malloc implementation. The
// user-space heap does not expose glibc's malloc_usable_size ABI.
struct alignas(16) Allocation { std::size_t size; };
struct AllocationStatistics
{
    std::size_t used = 0, peak = 0, limit = 0, failures = 0, last_failed_size = 0;
    std::size_t large_live = 0, large_peak = 0, large_count = 0, max_success = 0, max_request = 0;
    std::size_t bucket_bytes[8] = {}, bucket_counts[8] = {};
};
unsigned allocation_bucket(std::size_t size)
{
    unsigned bucket = 0;
    while (size >= 128 * 1024 && bucket < 7) { size >>= 1; ++bucket; }
    return bucket;
}
void record_block_change(JSMallocState *state, std::size_t previous, std::size_t current)
{
    auto *statistics = static_cast<AllocationStatistics *>(state->opaque);
    if (!statistics) return;
    constexpr std::size_t large = 64 * 1024;
    if (previous >= large)
    {
        statistics->large_live -= previous;
        --statistics->large_count;
        statistics->bucket_bytes[allocation_bucket(previous)] -= previous;
        --statistics->bucket_counts[allocation_bucket(previous)];
    }
    if (current >= large)
    {
        statistics->large_live += current;
        ++statistics->large_count;
        statistics->bucket_bytes[allocation_bucket(current)] += current;
        ++statistics->bucket_counts[allocation_bucket(current)];
    }
    statistics->large_peak = std::max(statistics->large_peak, statistics->large_live);
    statistics->max_success = std::max(statistics->max_success, current);
    statistics->max_request = std::max(statistics->max_request, current);
}
void record_allocation(JSMallocState *state, std::size_t failed_size = 0)
{
    auto *statistics = static_cast<AllocationStatistics *>(state->opaque);
    if (!statistics) return;
    statistics->used = state->malloc_size;
    statistics->peak = std::max(statistics->peak, statistics->used);
    statistics->limit = state->malloc_limit;
    if (failed_size)
    {
        ++statistics->failures; statistics->last_failed_size = failed_size;
        statistics->max_request = std::max(statistics->max_request, failed_size);
    }
}
void *js_allocate(JSMallocState *state, std::size_t size)
{
    if (!size || size > state->malloc_limit || state->malloc_size > state->malloc_limit - size ||
        size > static_cast<std::size_t>(-1) - sizeof(Allocation))
    { record_allocation(state, size); return nullptr; }
    auto *header = static_cast<Allocation *>(std::malloc(size + sizeof(Allocation)));
    if (!header) { record_allocation(state, size); return nullptr; }
    header->size = size;
    state->malloc_size += size;
    ++state->malloc_count;
    record_block_change(state, 0, size);
    record_allocation(state);
    return header + 1;
}
void js_release(JSMallocState *state, void *pointer)
{
    if (!pointer) return;
    auto *header = static_cast<Allocation *>(pointer) - 1;
    state->malloc_size -= header->size;
    --state->malloc_count;
    record_block_change(state, header->size, 0);
    record_allocation(state);
    std::free(header);
}
void *js_resize(JSMallocState *state, void *pointer, std::size_t size)
{
    if (!pointer) return js_allocate(state, size);
    if (!size) { js_release(state, pointer); return nullptr; }
    auto *old = static_cast<Allocation *>(pointer) - 1;
    const auto previous = old->size;
    if (size > state->malloc_limit || state->malloc_size - previous > state->malloc_limit - size ||
        size > static_cast<std::size_t>(-1) - sizeof(Allocation))
    { record_allocation(state, size); return nullptr; }
    auto *header = static_cast<Allocation *>(std::realloc(old, size + sizeof(Allocation)));
    if (!header) { record_allocation(state, size); return nullptr; }
    header->size = size;
    state->malloc_size = state->malloc_size - previous + size;
    record_block_change(state, previous, size);
    record_allocation(state);
    return header + 1;
}
std::size_t js_size(const void *pointer)
{
    return pointer ? (static_cast<const Allocation *>(pointer) - 1)->size : 0;
}
std::string text(JSContext *context, JSValueConst value)
{
    const char *chars = JS_ToCString(context, value);
    if (!chars) return {};
    std::string result(chars);
    JS_FreeCString(context, chars);
    return result;
}
bool generated(const litehtml::element::ptr &node)
{
    for (auto ancestor = node; ancestor; ancestor = ancestor->parent())
        if (ancestor->tag() == litehtml::__tag_before_ || ancestor->tag() == litehtml::__tag_after_) return true;
    return false;
}
std::string dom_text(const litehtml::element::ptr &node)
{
    std::string value;
    if (!node || generated(node)) return value;
    if (node->is_text() || node->is_comment()) { node->get_text(value); return value; }
    std::vector<litehtml::element::ptr> pending;
    for (auto it = node->children().rbegin(); it != node->children().rend(); ++it) pending.push_back(*it);
    while (!pending.empty())
    {
        auto current = pending.back(); pending.pop_back();
        if (generated(current) || current->is_comment()) continue;
        if (current->is_text()) current->get_text(value);
        else for (auto it = current->children().rbegin(); it != current->children().rend(); ++it) pending.push_back(*it);
    }
    return value;
}
std::string escaped(const std::string &value, bool attribute = false)
{
    std::string result;
    for (char ch : value)
    {
        if (ch == '&') result += "&amp;";
        else if (ch == '<') result += "&lt;";
        else if (ch == '>') result += "&gt;";
        else if (attribute && ch == '"') result += "&quot;";
        else result += ch;
    }
    return result;
}
JSClassID browser_dom_node_class;
} // namespace

struct BrowserScriptRuntime::Impl
{
    BrowserScriptHost host;
    litehtml::document::ptr document;
    std::string url, error, writes;
    AllocationStatistics allocations;
    JSRuntime *runtime = nullptr;
    JSContext *context = nullptr;
    std::map<std::string, JSValue> callbacks;
    unsigned budget = 0;
    bool dirty = false, initialized = false;
    unsigned long long mutations = 0, time_origin = 0;

    struct Module
    {
        std::string key, base, failure;
        std::vector<std::string> dependencies;
        JSValue compiled = JS_UNDEFINED, promise = JS_UNDEFINED, failure_value = JS_UNDEFINED;
        BrowserScriptModuleState state = BrowserScriptModuleState::Loading;
        bool has_source = false, linked = false, started = false;
    };
    struct DynamicModule
    {
        std::string key;
        JSValue resolve = JS_UNDEFINED, reject = JS_UNDEFINED;
    };
    struct ModuleEntry { std::string key; bool preload = false; };
    std::map<std::string, Module> modules;
    std::map<unsigned long long, ModuleEntry> module_entries;
    std::map<unsigned long long, std::string> module_fetches;
    std::vector<BrowserScriptModuleRequest> module_requests;
    std::vector<DynamicModule> dynamic_modules;
    struct Rejection { JSValue promise, reason; };
    std::vector<Rejection> unhandled_rejections;
    std::size_t module_source_bytes = 0, module_edges = 0;
    unsigned module_graph_work = 0;
    bool processing_modules = false;

    std::string module_url(const std::string &base, const std::string &specifier) const
    {
        if (specifier.empty() || specifier.size() > 16384 || specifier.find('\0') != std::string::npos) return {};
        browser_url_detail::Parts explicit_url;
        if (!browser_url_detail::parse(specifier, explicit_url)) return {};
        if (explicit_url.scheme.empty() && specifier.compare(0, 2, "./") &&
            specifier.compare(0, 3, "../") && specifier[0] != '/') return {};
        auto parent = modules.find(base);
        const auto &base_url = parent == modules.end() ? base : parent->second.base;
        auto resolved = browser_resolve_url(base_url, specifier);
        browser_url_detail::Parts parts;
        if (!browser_url_detail::parse(resolved, parts) || !parts.has_authority || parts.authority.empty()) return {};
        std::transform(parts.scheme.begin(), parts.scheme.end(), parts.scheme.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (parts.scheme != "http" && parts.scheme != "https") return {};
        if (parts.authority.find('@') != std::string::npos) return {};
        std::transform(parts.authority.begin(), parts.authority.end(), parts.authority.begin(),
            [](unsigned char c) { return std::tolower(c); });
        const auto suffix = parts.scheme == "https" ? std::string(":443") : std::string(":80");
        if (parts.authority.size() > suffix.size() &&
            parts.authority.compare(parts.authority.size() - suffix.size(), suffix.size(), suffix) == 0)
            parts.authority.resize(parts.authority.size() - suffix.size());
        return browser_url_detail::serialize(parts);
    }
    void module_failure(Module &module, const std::string &message)
    {
        module.failure = message;
        module.state = BrowserScriptModuleState::Rejected;
        if (!JS_IsUndefined(module.promise)) { JS_FreeValue(context, module.promise); module.promise = JS_UNDEFINED; }
        if (host.log) host.log("JavaScript module: " + module.key + ": " + message);
    }
    void module_exception(Module &module)
    {
        const auto exception = JS_GetException(context);
        JS_FreeValue(context, module.failure_value);
        module.failure_value = JS_DupValue(context, exception);
        JS_Throw(context, exception);
        report();
        module_failure(module, error);
    }
    bool request_module(const std::string &key, const std::string &referrer)
    {
        if (modules.find(key) != modules.end()) return true;
        if (modules.size() >= 256 || module_fetches.size() >= 256) return false;
        Module value;
        value.key = value.base = key;
        modules.emplace(key, std::move(value));
        const auto token = module_token();
        try
        {
            module_fetches.emplace(token, key);
            module_requests.push_back({token, key, referrer});
        }
        catch (...)
        {
            module_fetches.erase(token); modules.erase(key); throw;
        }
        return true;
    }
    bool compile_module(Module &module, const std::string &source)
    {
        if (source.size() > 4 * 1024 * 1024 || source.size() > 16 * 1024 * 1024 - module_source_bytes)
        { module_failure(module, "Module source budget exceeded"); return false; }
        module_source_bytes += source.size();
        module.has_source = true;
        module.compiled = JS_Eval(context, source.c_str(), source.size(), module.key.c_str(),
            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_DEFER_MODULE_RESOLVE);
        if (JS_IsException(module.compiled))
        {
            module.compiled = JS_UNDEFINED;
            module_exception(module); return false;
        }
        auto *definition = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(module.compiled));
        auto meta = JS_GetImportMeta(context, definition);
        auto meta_url = JS_IsException(meta) ? JS_EXCEPTION : JS_NewStringLen(context, module.base.data(), module.base.size());
        if (JS_IsException(meta_url) || JS_SetPropertyStr(context, meta, "url", meta_url) < 0)
        {
            JS_FreeValue(context, meta);
            module_exception(module); return false;
        }
        JS_FreeValue(context, meta);
        const int count = JS_GetModuleRequestCount(definition);
        if (count > 1024 || size_t(count) > 4096 - module_edges)
        { module_failure(module, "Module import budget exceeded"); return false; }
        module_edges += count;
        for (int i = 0; i < count; ++i)
        {
            const auto atom = JS_GetModuleRequestName(context, definition, i);
            const char *name = JS_AtomToCString(context, atom);
            JS_FreeAtom(context, atom);
            if (!name) { module_exception(module); return false; }
            const std::string specifier(name);
            JS_FreeCString(context, name);
            auto dependency = module_url(module.key, specifier);
            if (dependency.empty()) { module_failure(module, "Unsupported module specifier: " + specifier); return false; }
            if (!request_module(dependency, module.base))
            { module_failure(module, "Module graph budget exceeded"); return false; }
            module.dependencies.push_back(std::move(dependency));
        }
        return true;
    }
    // 1: complete graph, 0: awaiting fetch, -1: permanent graph failure.
    int module_graph(const std::string &root, std::string &failure, const Module *&failed)
    {
        std::vector<std::string> pending {root};
        std::set<std::string> visited;
        bool waiting = false;
        while (!pending.empty())
        {
            if (++module_graph_work > 8192) return 0;
            auto key = std::move(pending.back()); pending.pop_back();
            if (!visited.insert(key).second) continue;
            if (visited.size() > 256) { failure = "Module graph budget exceeded"; return -1; }
            const auto found = modules.find(key);
            if (found == modules.end()) { waiting = true; continue; }
            const auto &module = found->second;
            if (module.state == BrowserScriptModuleState::Rejected) { failure = module.failure; failed = &module; return -1; }
            if (!module.has_source) { waiting = true; continue; }
            for (const auto &dependency : module.dependencies) pending.push_back(dependency);
        }
        return waiting ? 0 : 1;
    }
    void refresh_linked_modules()
    {
        for (auto &entry : modules)
            if (!JS_IsUndefined(entry.second.compiled) &&
                JS_IsModuleInstantiated(static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(entry.second.compiled))))
                entry.second.linked = true;
    }
    void fail_unlinked_graph(Module &root)
    {
        refresh_linked_modules();
        std::vector<std::string> pending {root.key};
        std::set<std::string> visited;
        while (!pending.empty())
        {
            auto key = std::move(pending.back()); pending.pop_back();
            if (!visited.insert(key).second) continue;
            auto &entry = modules.at(key);
            for (const auto &dependency : entry.dependencies) pending.push_back(dependency);
            if (entry.linked || &entry == &root) continue;
            entry.failure_value = JS_DupValue(context, root.failure_value);
            module_failure(entry, root.failure);
        }
    }
    bool link_module(Module &module)
    {
        if (module.linked) return true;
        if (module.state != BrowserScriptModuleState::Loading) return false;
        std::string failure;
        const Module *failed = nullptr;
        const int ready = module_graph(module.key, failure, failed);
        if (ready < 0)
        {
            if (failed) module.failure_value = JS_DupValue(context, failed->failure_value);
            module_failure(module, failure); return false;
        }
        if (!ready || !budget) return false;
        if (JS_InstantiateModule(context, module.compiled) < 0)
        {
            module_exception(module);
            fail_unlinked_graph(module);
            return false;
        }
        refresh_linked_modules();
        module.linked = true;
        return true;
    }
    void evaluate_module(Module &module)
    {
        if (module.state != BrowserScriptModuleState::Loading || !link_module(module)) return;
        module.started = true;
        module.promise = JS_EvalFunction(context, JS_DupValue(context, module.compiled));
        if (JS_IsException(module.promise))
        {
            module.promise = JS_UNDEFINED;
            module_exception(module); return;
        }
        module.state = BrowserScriptModuleState::Evaluating;
    }
    void forget_rejection(JSValueConst promise)
    {
        for (size_t i = 0; i < unhandled_rejections.size(); ++i)
            if (JS_VALUE_GET_PTR(unhandled_rejections[i].promise) == JS_VALUE_GET_PTR(promise))
            {
                JS_FreeValue(context, unhandled_rejections[i].promise);
                JS_FreeValue(context, unhandled_rejections[i].reason);
                unhandled_rejections.erase(unhandled_rejections.begin() + i);
                return;
            }
    }
    static void promise_rejection(JSContext *context, JSValueConst promise, JSValueConst reason,
                                  JS_BOOL handled, void *opaque)
    {
        auto &self = *static_cast<Impl *>(opaque);
        if (handled) { self.forget_rejection(promise); return; }
        if (self.unhandled_rejections.size() >= 64) return;
        // A diagnostic must not throw C++ allocation errors across the engine.
        try { self.unhandled_rejections.push_back({JS_UNDEFINED, JS_UNDEFINED}); }
        catch (...) { return; }
        auto &entry = self.unhandled_rejections.back();
        entry.promise = JS_DupValue(context, promise);
        entry.reason = JS_DupValue(context, reason);
    }
    void report_rejections()
    {
        if (JS_IsJobPending(runtime)) return;
        auto pending = std::move(unhandled_rejections);
        unhandled_rejections.clear();
        for (const auto &entry : pending)
        {
            JS_Throw(context, JS_DupValue(context, entry.reason));
            report();
            JS_FreeValue(context, entry.reason); JS_FreeValue(context, entry.promise);
        }
    }
    void poll_module(Module &module)
    {
        if (module.state != BrowserScriptModuleState::Evaluating) return;
        const auto state = JS_PromiseState(context, module.promise);
        if (state == JS_PROMISE_PENDING) return;
        forget_rejection(module.promise);
        if (state == JS_PROMISE_REJECTED)
        {
            JS_Throw(context, JS_PromiseResult(context, module.promise));
            module_exception(module);
        }
        else
        {
            module.state = BrowserScriptModuleState::Fulfilled;
            JS_FreeValue(context, module.promise); module.promise = JS_UNDEFINED;
        }
    }
    void progress_modules()
    {
        if (processing_modules || !context) return;
        processing_modules = true;
        module_graph_work = 0;
        struct Guard { bool &flag; ~Guard() { flag = false; } } guard {processing_modules};
        for (const auto &entry : module_entries)
        {
            auto &module = modules.at(entry.second.key);
            if (entry.second.preload) link_module(module);
            else evaluate_module(module);
            poll_module(module);
        }
        for (size_t i = 0; i < dynamic_modules.size();)
        {
            const auto key = dynamic_modules[i].key;
            auto &module = modules.at(key);
            evaluate_module(module);
            poll_module(module);
            if (module.state != BrowserScriptModuleState::Fulfilled && module.state != BrowserScriptModuleState::Rejected)
            { ++i; continue; }
            // Remove the native root before calling a resolver: jobs created by
            // resolution may queue more imports at the next microtask checkpoint.
            auto task = dynamic_modules[i];
            dynamic_modules.erase(dynamic_modules.begin() + i);
            JSValue value;
            if (module.state == BrowserScriptModuleState::Fulfilled)
                value = JS_GetModuleNamespace(context, static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(module.compiled)));
            else if (!JS_IsUndefined(module.failure_value)) value = JS_DupValue(context, module.failure_value);
            else
            {
                JS_ThrowTypeError(context, "Failed to load module: %s", module.failure.c_str());
                value = JS_GetException(context);
            }
            const bool rejected = module.state == BrowserScriptModuleState::Rejected || JS_IsException(value);
            if (JS_IsException(value)) value = JS_GetException(context);
            const auto result = JS_Call(context, rejected ? task.reject : task.resolve, JS_UNDEFINED, 1, &value);
            JS_FreeValue(context, value);
            JS_FreeValue(context, task.resolve); JS_FreeValue(context, task.reject);
            if (JS_IsException(result)) report();
            JS_FreeValue(context, result);
        }
    }
    void release_modules()
    {
        for (const auto &entry : unhandled_rejections)
        { JS_FreeValue(context, entry.promise); JS_FreeValue(context, entry.reason); }
        unhandled_rejections.clear();
        for (const auto &task : dynamic_modules)
        { JS_FreeValue(context, task.resolve); JS_FreeValue(context, task.reject); }
        dynamic_modules.clear();
        for (const auto &item : modules)
        { JS_FreeValue(context, item.second.compiled); JS_FreeValue(context, item.second.promise);
          JS_FreeValue(context, item.second.failure_value); }
        modules.clear();
    }
    static char *normalize_module(JSContext *context, const char *base, const char *name, void *opaque)
    {
        try
        {
            auto &self = *static_cast<Impl *>(opaque);
            const auto value = self.module_url(base, name);
            if (value.empty()) { JS_ThrowTypeError(context, "Unsupported module specifier: %s", name); return nullptr; }
            auto *result = static_cast<char *>(js_malloc(context, value.size() + 1));
            if (result) std::memcpy(result, value.c_str(), value.size() + 1);
            return result;
        }
        catch (const std::bad_alloc &) { JS_ThrowOutOfMemory(context); return nullptr; }
        catch (...) { JS_ThrowInternalError(context, "Module normalization failed"); return nullptr; }
    }
    static JSModuleDef *load_cached_module(JSContext *context, const char *name, void *opaque, JSValueConst)
    {
        try
        {
            auto &self = *static_cast<Impl *>(opaque);
            const auto found = self.modules.find(name);
            if (found == self.modules.end() || JS_IsUndefined(found->second.compiled) ||
                found->second.state == BrowserScriptModuleState::Rejected)
            { JS_ThrowReferenceError(context, "Module is not available in the dependency cache: %s", name); return nullptr; }
            return static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(found->second.compiled));
        }
        catch (const std::bad_alloc &) { JS_ThrowOutOfMemory(context); return nullptr; }
        catch (...) { JS_ThrowInternalError(context, "Module cache lookup failed"); return nullptr; }
    }
    static int check_module_attributes(JSContext *context, void *, JSValueConst attributes)
    {
        if (JS_IsUndefined(attributes)) return 0;
        JSPropertyEnum *properties = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(context, &properties, &count, attributes, JS_GPN_STRING_MASK) < 0) return -1;
        JS_FreePropertyEnum(context, properties, count);
        if (!count) return 0;
        JS_ThrowTypeError(context, "Module import attributes are not supported"); return -1;
    }
    static int import_module(JSContext *context, const char *base, const char *specifier,
                             JSValueConst resolve, JSValueConst reject, JSValueConst, void *opaque)
    {
        try
        {
            auto &self = *static_cast<Impl *>(opaque);
            if (self.dynamic_modules.size() >= 256)
            { JS_ThrowRangeError(context, "Pending dynamic import budget exceeded"); return -1; }
            auto key = self.module_url(base, specifier);
            if (key.empty()) key = self.module_url(self.url, specifier);
            if (key.empty()) { JS_ThrowTypeError(context, "Unsupported module specifier: %s", specifier); return -1; }
            const auto found = self.modules.find(base);
            const auto classic_base = self.module_url(self.url, base);
            const auto referrer = found == self.modules.end() ?
                (classic_base.empty() ? self.url : classic_base) : found->second.base;
            if (!self.request_module(key, referrer))
            { JS_ThrowRangeError(context, "Module graph budget exceeded"); return -1; }
            self.dynamic_modules.push_back({key, JS_UNDEFINED, JS_UNDEFINED});
            auto &task = self.dynamic_modules.back();
            task.resolve = JS_DupValue(context, resolve);
            task.reject = JS_DupValue(context, reject);
            return 0;
        }
        catch (const std::bad_alloc &) { JS_ThrowOutOfMemory(context); return -1; }
        catch (...) { JS_ThrowInternalError(context, "Dynamic module request failed"); return -1; }
    }

    ~Impl()
    {
        if (context)
        {
            release_modules();
            release_external();
            const auto root = document_node; document_node = JS_UNDEFINED;
            JS_FreeValue(context, root);
            for (const auto &entry : callbacks) JS_FreeValue(context, entry.second);
            callbacks.clear();
            JS_FreeContext(context);
        }
        if (runtime) JS_FreeRuntime(runtime);
    }
    unsigned long long now_ms() const
    {
        if (host.monotonic_ms) return host.monotonic_ms();
        struct timespec now {};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
        return static_cast<unsigned long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    }
    static int interrupted(JSRuntime *, void *opaque)
    {
        auto &self = *static_cast<Impl *>(opaque);
        if (!self.budget) return 1;
        --self.budget;
        return 0;
    }
    struct NativeNode
    {
        Impl *owner;
        unsigned number;
        litehtml::element::ptr element;
        // wrapper is borrowed; only the two graph-edge fields own JS references.
        JSValue wrapper = JS_UNDEFINED, parent = JS_UNDEFINED, listener_state = JS_UNDEFINED;
        std::map<unsigned, JSValue> children;
    };
    std::vector<std::unique_ptr<NativeNode>> nodes;
    std::vector<unsigned> free_node_ids;
    std::map<const litehtml::element *, unsigned> identities;
    std::map<const litehtml::element *, std::weak_ptr<litehtml::element>> managed_nodes;
    std::map<const litehtml::element *, std::pair<JSValue, unsigned>> external_nodes;
    JSValue document_node = JS_UNDEFINED;
    unsigned sweep_counter = 0;
    std::size_t collections = 0, collection_before = 0, collection_after = 0, collection_failures = 0;
    unsigned long long collection_time = 0, failure_collection_time = 0;
    bool collected_after_failure = false, reported_memory_breakdown = false;

    Impl() { nodes.emplace_back(); free_node_ids.reserve(50000); }

    static void node_mark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark)
    {
        auto *record = static_cast<NativeNode *>(JS_GetOpaque(value, browser_dom_node_class));
        if (!record) return;
        JS_MarkValue(runtime, record->parent, mark);
        JS_MarkValue(runtime, record->listener_state, mark);
        for (const auto &edge : record->children) JS_MarkValue(runtime, edge.second, mark);
    }
    static void node_finalize(JSRuntime *runtime, JSValue value) noexcept
    {
        auto *record = static_cast<NativeNode *>(JS_GetOpaque(value, browser_dom_node_class));
        if (!record) return;
        JS_SetOpaque(value, nullptr);
        record->owner->destroy_node(runtime, record);
    }
    void destroy_node(JSRuntime *runtime, NativeNode *record) noexcept
    {
        const unsigned number = record->number;
        if (number >= nodes.size() || nodes[number].get() != record) return;
        // Invalidate every lookup before freeing references: another node's
        // finalizer may run synchronously and reenter this registry.
        const auto found = identities.find(record->element.get());
        if (found != identities.end() && found->second == number) identities.erase(found);
        auto owned = std::move(nodes[number]);
        free_node_ids.push_back(number);
        auto children = std::move(owned->children);
        const auto parent = owned->parent;
        const auto listeners = owned->listener_state;
        owned->parent = owned->wrapper = owned->listener_state = JS_UNDEFINED;
        owned.reset();
        for (const auto &edge : children) JS_FreeValueRT(runtime, edge.second);
        JS_FreeValueRT(runtime, parent);
        JS_FreeValueRT(runtime, listeners);
    }
    static int node_own_property(JSContext *context, JSPropertyDescriptor *descriptor,
                                 JSValueConst object, JSAtom atom)
    {
        try { return node_own_property_impl(context, descriptor, object, atom); }
        catch (const std::bad_alloc &) { JS_ThrowOutOfMemory(context); }
        catch (const std::exception &error) { JS_ThrowInternalError(context, "%s", error.what()); }
        catch (...) { JS_ThrowInternalError(context, "Native form lookup failed"); }
        return -1;
    }
    static int node_own_property_impl(JSContext *context, JSPropertyDescriptor *descriptor,
                                      JSValueConst object, JSAtom atom)
    {
        auto *entry = static_cast<NativeNode *>(JS_GetOpaque(object, browser_dom_node_class));
        if (!entry || std::strcmp(entry->element->get_tagName(), "form")) return 0;
        auto key_value = JS_AtomToValue(context, atom);
        if (JS_IsException(key_value)) return -1;
        if (!JS_IsString(key_value)) { JS_FreeValue(context, key_value); return 0; }
        const char *chars = JS_ToCString(context, key_value);
        JS_FreeValue(context, key_value);
        if (!chars) return -1;
        std::string key(chars); JS_FreeCString(context, chars);
        if (key.empty() || key == "_id") return 0;
        auto *self = entry->owner;
        if (!self->budget)
        {
            JS_ThrowInternalError(context, "interrupted");
            JS_SetUncatchableException(context, true);
            return -1;
        }
        --self->budget;
        auto tree = entry->element;
        while (tree->parent()) tree = tree->parent();
        std::vector<litehtml::element::ptr> pending {tree};
        while (!pending.empty())
        {
            auto candidate = pending.back(); pending.pop_back();
            if (generated(candidate)) continue;
            const std::string tag = candidate->get_tagName();
            const bool listed = tag == "input" || tag == "button" || tag == "select" || tag == "textarea" ||
                tag == "fieldset" || tag == "object" || tag == "output";
            if (listed && (key == candidate->get_attr("name", "") || key == candidate->get_attr("id", "")))
            {
                litehtml::element::ptr owner;
                if (const char *explicit_form = candidate->get_attr("form"))
                {
                    std::vector<litehtml::element::ptr> search {tree};
                    while (!search.empty() && *explicit_form)
                    {
                        auto node = search.back(); search.pop_back();
                        if (!std::strcmp(node->get_attr("id", ""), explicit_form))
                        { if (!std::strcmp(node->get_tagName(), "form")) owner = node; break; }
                        for (auto it = node->children().rbegin(); it != node->children().rend(); ++it) search.push_back(*it);
                    }
                }
                else for (auto parent = candidate->parent(); parent; parent = parent->parent())
                    if (!std::strcmp(parent->get_tagName(), "form")) { owner = parent; break; }
                if (owner == entry->element)
                {
                    if (!descriptor) return 1;
                    const auto number = self->id(candidate);
                    if (!number) { JS_ThrowRangeError(context, "DOM wrapper budget exceeded"); return -1; }
                    const auto callback = self->callbacks.find("__node");
                    if (callback == self->callbacks.end()) return 0;
                    JSValue argument = JS_NewUint32(context, number);
                    // Preserve the outer execution budget and stack baseline.
                    auto value = JS_Call(context, callback->second, JS_UNDEFINED, 1, &argument);
                    JS_FreeValue(context, argument);
                    if (JS_IsException(value)) return -1;
                    descriptor->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
                    descriptor->value = value;
                    descriptor->getter = descriptor->setter = JS_UNDEFINED;
                    return 1;
                }
            }
            for (auto it = candidate->children().rbegin(); it != candidate->children().rend(); ++it) pending.push_back(*it);
        }
        return 0;
    }
    NativeNode *record(unsigned number) const
    {
        return number < nodes.size() ? nodes[number].get() : nullptr;
    }
    unsigned id(const litehtml::element::ptr &element)
    {
        if (!element) return 0;
        const auto found = identities.find(element.get());
        if (found != identities.end()) return found->second;
        unsigned number;
        if (!free_node_ids.empty()) { number = free_node_ids.back(); free_node_ids.pop_back(); }
        else
        {
            if (nodes.size() > 50000) return 0;
            number = static_cast<unsigned>(nodes.size());
            nodes.emplace_back();
        }
        auto entry = std::make_unique<NativeNode>();
        entry->owner = this; entry->number = number; entry->element = element;
        nodes[number] = std::move(entry);
        identities[element.get()] = number;
        return number;
    }
    litehtml::element::ptr node(unsigned number) const
    {
        const auto *entry = record(number);
        return entry ? entry->element : nullptr;
    }
    JSValue wrap_node(unsigned number)
    {
        auto *entry = record(number);
        if (!entry) return JS_NULL;
        if (!JS_IsUndefined(entry->wrapper)) return JS_DupValue(context, entry->wrapper);
        std::vector<NativeNode *> chain;
        JSValue ancestor_pin = JS_UNDEFINED;
        for (auto *current = entry; current;)
        {
            if (!JS_IsUndefined(current->wrapper))
            { ancestor_pin = JS_DupValue(context, current->wrapper); break; }
            chain.push_back(current);
            if (chain.size() > 4096) return JS_ThrowRangeError(context, "DOM ancestry budget exceeded");
            const auto parent = current->element->parent();
            if (!parent) break;
            const auto parent_id = id(parent);
            if (!parent_id) return JS_ThrowRangeError(context, "DOM wrapper budget exceeded");
            current = record(parent_id);
        }
        std::vector<JSValue> pins;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            auto *current = *it;
            auto wrapper = JS_NewObjectClass(context, browser_dom_node_class);
            if (JS_IsException(wrapper))
            {
                for (auto pin : pins) JS_FreeValue(context, pin);
                JS_FreeValue(context, ancestor_pin);
                return JS_EXCEPTION;
            }
            current->wrapper = wrapper;
            JS_SetOpaque(wrapper, current);
            pins.push_back(wrapper);
            if (const auto parent = current->element->parent())
            {
                auto *parent_record = record(identities.find(parent.get())->second);
                current->parent = JS_DupValue(context, parent_record->wrapper);
                parent_record->children.emplace(current->number, JS_DupValue(context, wrapper));
            }
            if (current->element == document->root() && JS_IsUndefined(document_node))
                document_node = JS_DupValue(context, wrapper);
        }
        auto result = JS_DupValue(context, entry->wrapper);
        for (auto pin : pins) JS_FreeValue(context, pin);
        JS_FreeValue(context, ancestor_pin);
        return result;
    }
    bool reparent_wrapper(const litehtml::element::ptr &element)
    {
        const auto found = identities.find(element.get());
        if (found == identities.end()) return true;
        auto *entry = record(found->second);
        if (!entry || JS_IsUndefined(entry->wrapper)) return true;
        JSValue new_parent_wrapper = JS_UNDEFINED;
        if (const auto parent = element->parent())
        {
            const auto parent_id = id(parent);
            if (!parent_id) { JS_ThrowRangeError(context, "DOM wrapper budget exceeded"); return false; }
            new_parent_wrapper = wrap_node(parent_id);
            if (JS_IsException(new_parent_wrapper)) return false;
            if (JS_IsNull(new_parent_wrapper))
            { JS_ThrowRangeError(context, "Missing DOM parent wrapper"); return false; }
        }
        auto pin = JS_DupValue(context, entry->wrapper);
        const auto old_parent = entry->parent;
        entry->parent = JS_UNDEFINED;
        auto *old = static_cast<NativeNode *>(JS_GetOpaque(old_parent, browser_dom_node_class));
        if (old)
        {
            const auto edge = old->children.find(entry->number);
            if (edge != old->children.end())
            {
                const auto reference = edge->second;
                old->children.erase(edge);
                JS_FreeValue(context, reference);
            }
        }
        JS_FreeValue(context, old_parent);
        if (!JS_IsUndefined(new_parent_wrapper))
        {
            auto *new_parent = static_cast<NativeNode *>(JS_GetOpaque(new_parent_wrapper, browser_dom_node_class));
            entry->parent = new_parent_wrapper;
            new_parent->children.emplace(entry->number, JS_DupValue(context, pin));
        }
        JS_FreeValue(context, pin);
        return true;
    }
    void detach_children(const litehtml::element::ptr &parent)
    {
        const std::vector<litehtml::element::ptr> children(parent->children().begin(), parent->children().end());
        for (const auto &child : children)
        {
            parent->removeChild(child);
            reparent_wrapper(child);
        }
    }
    void discard_unwrapped()
    {
        for (unsigned i = 1; i < nodes.size(); ++i)
            if (nodes[i] && JS_IsUndefined(nodes[i]->wrapper)) destroy_node(runtime, nodes[i].get());
    }
    void sweep_nodes()
    {
        for (auto it = managed_nodes.begin(); it != managed_nodes.end();)
            if (it->second.expired()) it = managed_nodes.erase(it); else ++it;
    }
    void collect_nodes()
    {
        collection_before = allocations.used;
        JS_RunGC(runtime);
        sweep_nodes();
        collection_after = allocations.used;
        collection_time = now_ms();
        collection_failures = allocations.failures;
        ++collections;
    }
    BrowserScriptMemoryBreakdown memory_breakdown() const
    {
        BrowserScriptMemoryBreakdown result;
        if (!runtime) return result;
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(runtime, &usage);
        result.allocated = usage.malloc_size; result.accounted = usage.memory_used_size;
        result.objects = usage.obj_count; result.object_bytes = usage.obj_size;
        result.strings = usage.str_count; result.string_bytes = usage.str_size;
        result.atoms = usage.atom_count; result.atom_bytes = usage.atom_size;
        result.properties = usage.prop_count; result.property_bytes = usage.prop_size;
        result.shapes = usage.shape_count; result.shape_bytes = usage.shape_size;
        result.functions = usage.js_func_count; result.function_bytes = usage.js_func_size;
        result.bytecode_bytes = usage.js_func_code_size; result.arrays = usage.array_count;
        result.fast_array_elements = usage.fast_array_elements;
        result.binary_objects = usage.binary_object_count; result.binary_object_bytes = usage.binary_object_size;
        return result;
    }
    void report_memory_breakdown()
    {
        if (reported_memory_breakdown || !allocations.failures || !host.log) return;
        // JS_ComputeMemoryUsage walks engine objects without invoking JavaScript.
        // Emit once after returning to native code, including caught JS OOMs.
        reported_memory_breakdown = true;
        const auto usage = memory_breakdown();
        host.log("JavaScript heap classes: allocated=" + std::to_string(usage.allocated) +
            " accounted=" + std::to_string(usage.accounted) + " objects=" + std::to_string(usage.objects) +
            "/" + std::to_string(usage.object_bytes) + " strings=" + std::to_string(usage.strings) +
            "/" + std::to_string(usage.string_bytes) + " atoms=" + std::to_string(usage.atoms) +
            "/" + std::to_string(usage.atom_bytes) + " properties=" + std::to_string(usage.properties) +
            "/" + std::to_string(usage.property_bytes) + " shapes=" + std::to_string(usage.shapes) +
            "/" + std::to_string(usage.shape_bytes) + " functions=" + std::to_string(usage.functions) +
            "/" + std::to_string(usage.function_bytes) + " bytecode=" + std::to_string(usage.bytecode_bytes) +
            " arrays=" + std::to_string(usage.arrays) + " fast-elements=" + std::to_string(usage.fast_array_elements) +
            " binary=" + std::to_string(usage.binary_objects) + "/" + std::to_string(usage.binary_object_bytes));
        host.log("JavaScript heap large blocks: live=" + std::to_string(allocations.large_live) +
            " peak=" + std::to_string(allocations.large_peak) + " count=" + std::to_string(allocations.large_count) +
            " max-success=" + std::to_string(allocations.max_success) + " max-request=" + std::to_string(allocations.max_request));
        std::string buckets = "JavaScript heap buckets >=64KiB (bytes/count):";
        for (unsigned i = 0; i < 8; ++i)
            buckets += " " + std::to_string(allocations.bucket_bytes[i]) + "/" + std::to_string(allocations.bucket_counts[i]);
        host.log(buckets);
    }
    void boundary_collection()
    {
        // Run between VM calls, never from the allocator. Retained heaps above
        // the watermark must grow before another normal collection, and failed
        // allocations are rate limited so a full heap cannot monopolize pumps.
        const auto now = now_ms();
        const bool failure = allocations.failures > collection_failures;
        if (failure && (!collected_after_failure || now - failure_collection_time >= 500))
        {
            collect_nodes();
            collected_after_failure = true;
            failure_collection_time = now;
        }
        else if (allocations.used >= 32 * 1024 * 1024 &&
                 allocations.used >= collection_after + 4 * 1024 * 1024 &&
                 (!collections || now - collection_time >= 250)) collect_nodes();
    }
    bool reserve_nodes(std::size_t count)
    {
        if (count > 50000) return false;
        if (++sweep_counter >= 256 || managed_nodes.size() + count > 50000)
        { sweep_counter = 0; sweep_nodes(); }
        if (managed_nodes.size() + count > 50000) collect_nodes();
        return managed_nodes.size() + count <= 50000;
    }
    void track_node(const litehtml::element::ptr &element)
    {
        if (element && !generated(element)) managed_nodes[element.get()] = element;
    }
    bool track_tree(const litehtml::element::ptr &root)
    {
        std::vector<litehtml::element::ptr> pending {root}, created;
        while (!pending.empty())
        {
            auto current = pending.back(); pending.pop_back();
            if (generated(current)) continue;
            if (managed_nodes.find(current.get()) == managed_nodes.end()) created.push_back(current);
            for (const auto &child : current->children()) pending.push_back(child);
        }
        if (!reserve_nodes(created.size())) return false;
        for (const auto &element : created) track_node(element);
        return true;
    }
    bool replace_html(const litehtml::element::ptr &parent, const std::string &html, bool replace)
    {
        // Reject plainly excessive markup before allocating a temporary tree;
        // the exact native-node count is checked before publishing the fragment.
        const std::string context = parent->get_tagName();
        const bool rcdata = context == "textarea" || context == "title";
        if (!rcdata && std::count(html.begin(), html.end(), '<') > 50000) return false;
        auto holder = document->create_element(rcdata ? context.c_str() : "div", {});
        holder->compute_styles(false);
        document->append_children_from_string(*holder, html.c_str(), false);
        std::vector<litehtml::element::ptr> roots(holder->children().begin(), holder->children().end());
        std::vector<litehtml::element::ptr> created, pending = roots;
        while (!pending.empty())
        {
            auto current = pending.back(); pending.pop_back();
            if (generated(current)) continue;
            created.push_back(current);
            for (const auto &child : current->children()) pending.push_back(child);
        }
        if (!reserve_nodes(created.size())) return false;
        for (const auto &element : created) track_node(element);
        if (replace) detach_children(parent);
        for (const auto &child : roots)
        {
            holder->removeChild(child);
            parent->appendChild(child);
            reparent_wrapper(child);
            added(child);
        }
        return true;
    }
    bool replace_text(const litehtml::element::ptr &parent, const std::string &value)
    {
        litehtml::element::ptr text;
        if (!value.empty())
        {
            if (!reserve_nodes(1)) return false;
            text = std::make_shared<litehtml::el_text>(value.c_str(), document);
            track_node(text);
        }
        detach_children(parent);
        if (text) parent->appendChild(text);
        return true;
    }
    void release_external()
    {
        auto references = std::move(external_nodes);
        external_nodes.clear();
        for (const auto &entry : references) JS_FreeValue(context, entry.second.first);
    }
    void report()
    {
        auto exception = JS_GetException(context);
        error = text(context, exception);
        if (error.empty() || JS_IsNull(exception)) error = "JavaScript exception (message unavailable)";
        if (JS_IsObject(exception))
        {
            const auto stack = JS_GetPropertyStr(context, exception, "stack");
            if (!JS_IsException(stack) && !JS_IsUndefined(stack)) error += "\n" + text(context, stack);
            JS_FreeValue(context, stack);
        }
        JS_FreeValue(context, exception);
        // Error formatting can itself run out of memory. Discard that secondary
        // exception instead of leaving a pending exception behind a successful call.
        const auto secondary = JS_GetException(context);
        JS_FreeValue(context, secondary);
        if (allocations.failures)
            error += " [heap=" + std::to_string(allocations.used) + "/" + std::to_string(allocations.limit) +
                " failed-allocation=" + std::to_string(allocations.last_failed_size) +
                " failures=" + std::to_string(allocations.failures) + "]";
        if (host.log) host.log("JavaScript: " + error);
    }
    bool result(JSValue value)
    {
        const bool good = !JS_IsException(value);
        if (!good) report();
        JS_FreeValue(context, value);
        return good;
    }
    JSValue call(const char *name, int count, JSValueConst *args)
    {
        if (!context) return JS_UNDEFINED;
        for (int i = 0; i < count; ++i) if (JS_IsException(args[i])) return JS_EXCEPTION;
        JS_UpdateStackTop(runtime);
        budget = 4000;
        JS_SetUncatchableException(context, false);
        auto global = JS_GetGlobalObject(context);
        const auto found = callbacks.find(name);
        if (found == callbacks.end() || !JS_IsFunction(context, found->second))
        { JS_FreeValue(context, global); return JS_UNDEFINED; }
        auto value = JS_Call(context, found->second, global, count, args);
        JS_FreeValue(context, global);
        return value;
    }
    void jobs()
    {
        progress_modules();
        for (unsigned i = 0; i < 64; ++i)
        {
            JSContext *job_context = nullptr;
            const int status = JS_ExecutePendingJob(runtime, &job_context);
            if (!status) break;
            if (status < 0)
            {
                report();
                if (!budget) break;
            }
        }
        progress_modules();
        report_rejections();
        discard_unwrapped();
        boundary_collection();
        report_memory_breakdown();
    }
    bool inert(litehtml::element::ptr element) const
    {
        unsigned remaining = 50001;
        while (element && remaining--)
        {
            if (std::strcmp(element->get_tagName(), "template") == 0) return true;
            element = element->parent();
        }
        return bool(element);
    }
    bool connected(litehtml::element::ptr element) const
    {
        unsigned remaining = 50000;
        while (element && remaining--)
        {
            if (std::strcmp(element->get_tagName(), "template") == 0) return false;
            if (element == document->root()) return true;
            element = element->parent();
        }
        return false;
    }
    void added(const litehtml::element::ptr &element)
    {
        dirty = true;
        ++mutations;
        if (inert(element)) return;
        std::vector<litehtml::element::ptr> pending;
        if (element) pending.push_back(element);
        unsigned remaining = 50000;
        while (!pending.empty() && remaining--)
        {
            auto current = pending.back(); pending.pop_back();
            if (std::strcmp(current->get_tagName(), "template") == 0) continue;
            if (std::strcmp(current->get_tagName(), "script") == 0 && host.script_added && connected(current)) host.script_added(current);
            if (std::strcmp(current->get_tagName(), "link") == 0 && host.link_added && connected(current)) host.link_added(current);
            for (const auto &child : current->children()) pending.push_back(child);
        }
    }
    std::string markup(const litehtml::element::ptr &element, unsigned remaining = 128)
    {
        if (!element || !remaining || generated(element)) return {};
        std::string value;
        if (element->is_comment()) { element->get_text(value); return "<!--" + value + "-->"; }
        if (element->is_text()) { element->get_text(value); return escaped(value); }
        value = "<" + std::string(element->get_tagName());
        // litehtml exposes node attributes through this diagnostic surface.
        for (const auto &attribute : element->attributes())
            value += " " + attribute.first + "=\"" + escaped(attribute.second, true) + "\"";
        value += ">";
        const std::string tag = element->get_tagName();
        if (tag == "script" || tag == "style") value += dom_text(element);
        else for (const auto &child : element->children()) value += markup(child, remaining - 1);
        value += "</" + std::string(element->get_tagName()) + ">";
        return value;
    }
    litehtml::element::ptr clone(const litehtml::element::ptr &original, bool deep, unsigned remaining = 256)
    {
        if (!original || generated(original) || !remaining || !reserve_nodes(1)) return nullptr;
        std::string value;
        litehtml::element::ptr copy;
        if (original->is_comment())
        {
            original->get_text(value);
            copy = std::make_shared<litehtml::el_comment>(document);
            copy->set_data(value.c_str());
        }
        else if (original->is_text())
        {
            original->get_text(value);
            copy = std::make_shared<litehtml::el_text>(value.c_str(), document);
        }
        else
        {
            copy = document->create_element(original->get_tagName(), original->attributes());
            if (copy) copy->compute_styles(false);
        }
        if (!copy) return nullptr;
        track_node(copy);
        if (deep && !original->is_text() && !original->is_comment())
            for (const auto &child : original->children())
            {
                if (generated(child)) continue;
                auto copied_child = clone(child, true, remaining - 1);
                if (!copied_child) return nullptr;
                copy->appendChild(copied_child);
            }
        return copy;
    }
    static JSValue microtask_job(JSContext *context, int count, JSValueConst *args)
    {
        return count ? JS_Call(context, args[0], JS_UNDEFINED, 0, nullptr) : JS_UNDEFINED;
    }
    static JSValue bridge(JSContext *context, JSValueConst, int count, JSValueConst *args)
    {
        // C++ host code must unwind only to this boundary. Crossing the C
        // interpreter would leave QuickJS's active frame and exception state stale.
        try { return bridge_impl(context, count, args); }
        catch (const std::bad_alloc &) { return JS_ThrowOutOfMemory(context); }
        catch (const std::exception &error) { return JS_ThrowInternalError(context, "%s", error.what()); }
        catch (...) { return JS_ThrowInternalError(context, "Native browser operation failed"); }
    }
    static JSValue bridge_impl(JSContext *context, int count, JSValueConst *args)
    {
        auto *self = static_cast<Impl *>(JS_GetContextOpaque(context));
        if (!self || !self->document || !count) return JS_NULL;
        bool conversion_failed = false;
        auto argument = [&](int index) {
            if (index >= count) return std::string();
            const char *chars = JS_ToCString(context, args[index]);
            if (!chars) { conversion_failed = true; return std::string(); }
            std::string result(chars); JS_FreeCString(context, chars); return result;
        };
        auto number = [&](int index) {
            uint32_t value = 0;
            if (index < count && JS_ToUint32(context, &value, args[index]) < 0) conversion_failed = true;
            return value;
        };
        const auto operation = argument(0);
        auto element = self->node(number(1));
        if (conversion_failed) return JS_EXCEPTION;
        if (operation == "enqueue-microtask")
        {
            if (count < 3 || !JS_IsFunction(context, args[2]))
                return JS_ThrowTypeError(context, "Microtask callback must be callable");
            // The engine duplicates argv and retains the context until execution.
            // No page-replaceable Promise or .then method participates here.
            if (JS_EnqueueJob(context, microtask_job, 1, args + 2) < 0) return JS_EXCEPTION;
            return JS_UNDEFINED;
        }
        if (operation == "listeners")
        {
            auto *entry = self->record(number(1));
            if (!entry) return JS_UNDEFINED;
            if (count > 2 && JS_IsObject(args[2]) && JS_IsUndefined(entry->listener_state))
                entry->listener_state = JS_DupValue(context, args[2]);
            return JS_DupValue(context, entry->listener_state);
        }
        const auto key = argument(2), value = argument(3);
        if (conversion_failed) return JS_EXCEPTION;
        auto string = [&](const std::string &s) { return JS_NewStringLen(context, s.data(), s.size()); };
        auto identifier = [&](const litehtml::element::ptr &n) {
            const auto number = self->id(n);
            return !number && n ? JS_ThrowRangeError(context, "DOM wrapper budget exceeded") : JS_NewUint32(context, number);
        };
        auto array = [&](const std::vector<litehtml::element::ptr> &elements) {
            auto output = JS_NewArray(context);
            if (JS_IsException(output)) return JS_EXCEPTION;
            unsigned index = 0;
            for (const auto &n : elements)
                if (!generated(n))
                {
                    auto item = identifier(n);
                    if (JS_IsException(item)) { JS_FreeValue(context, output); return JS_EXCEPTION; }
                    if (JS_SetPropertyUint32(context, output, index++, item) < 0)
                    { JS_FreeValue(context, output); return JS_EXCEPTION; }
                }
            return output;
        };
        auto root = self->document->root();
        if (operation == "wrap") return self->wrap_node(number(1));
        if (operation == "root") return identifier(root);
        if (operation == "mutations") return JS_NewFloat64(context, self->mutations);
        if (operation == "url") return string(self->host.current_url ? self->host.current_url() : self->url);
        if (operation == "history-state") return string(self->host.history_state ? self->host.history_state() : "null");
        if (operation == "history-length") return JS_NewUint32(context, self->host.history_length ? self->host.history_length() : 1);
        if (operation == "history-update")
        {
            if (value.size() > 1024 * 1024) return JS_ThrowRangeError(context, "History state budget exceeded");
            const auto next = browser_resolve_url(self->url, key);
            const bool good = self->host.history_update && self->host.history_update(next, number(1) != 0, value);
            if (good) self->url = next;
            return JS_NewBool(context, good);
        }
        if (operation == "history-go")
        {
            int32_t delta = 0;
            if (count > 2 && JS_ToInt32(context, &delta, args[2]) < 0) return JS_EXCEPTION;
            if (self->host.history_go) self->host.history_go(delta);
            return JS_UNDEFINED;
        }
        if (operation == "viewport")
        {
            BrowserScriptViewport viewport;
            if (self->host.viewport) viewport = self->host.viewport();
            else
            {
                litehtml::position box;
                self->document->container()->get_viewport(box);
                viewport.width = viewport.outer_width = box.width;
                viewport.height = viewport.outer_height = box.height;
                viewport.scroll_width = self->document->width(); viewport.scroll_height = self->document->height();
            }
            auto output = JS_NewObject(context);
            if (JS_IsException(output)) return JS_EXCEPTION;
            auto field = [&](const char *name, double value)
            {
                auto item = JS_NewFloat64(context, value);
                return !JS_IsException(item) && JS_SetPropertyStr(context, output, name, item) >= 0;
            };
            if (!field("width", viewport.width) || !field("height", viewport.height) ||
                !field("outerWidth", viewport.outer_width) || !field("outerHeight", viewport.outer_height) ||
                !field("scrollX", viewport.scroll_x) || !field("scrollY", viewport.scroll_y) ||
                !field("scrollWidth", viewport.scroll_width) || !field("scrollHeight", viewport.scroll_height))
            { JS_FreeValue(context, output); return JS_EXCEPTION; }
            return output;
        }
        if (operation == "scroll")
        {
            double x = 0, y = 0;
            if ((count > 2 && JS_ToFloat64(context, &x, args[2]) < 0) ||
                (count > 3 && JS_ToFloat64(context, &y, args[3]) < 0)) return JS_EXCEPTION;
            if (x != x || y != y || x > 1e9 || x < -1e9 || y > 1e9 || y < -1e9)
                return JS_ThrowRangeError(context, "Scroll coordinate outside supported range");
            if (self->host.scroll_to) self->host.scroll_to(x, y);
            return JS_UNDEFINED;
        }
        if (operation == "link-supports")
            return JS_NewBool(context, self->host.link_added && (key == "stylesheet" || key == "modulepreload"));
        if (operation == "now")
        {
            const auto now = self->now_ms();
            return JS_NewFloat64(context, now >= self->time_origin ? now - self->time_origin : 0);
        }
        if (operation == "resolve" || operation == "resolve-url")
        {
            std::string base = operation == "resolve-url" ? value : self->url;
            if (operation == "resolve")
                if (auto b = root->select_one("base[href]")) base = browser_resolve_url(self->url, b->get_attr("href", ""));
            return string(browser_resolve_url(base, key));
        }
        if (operation == "request")
        {
            const auto resolved = browser_resolve_url(self->url, key);
            auto origin = [](const std::string &input) {
                browser_url_detail::Parts parts;
                if (!browser_url_detail::parse(input, parts) || !parts.has_authority) return std::string();
                std::transform(parts.scheme.begin(), parts.scheme.end(), parts.scheme.begin(),
                    [](unsigned char ch) { return std::tolower(ch); });
                std::transform(parts.authority.begin(), parts.authority.end(), parts.authority.begin(),
                    [](unsigned char ch) { return std::tolower(ch); });
                if (parts.scheme != "http" && parts.scheme != "https") return std::string();
                const std::string suffix = parts.scheme == "https" ? ":443" : ":80";
                if (parts.authority.size() > suffix.size() &&
                    parts.authority.compare(parts.authority.size() - suffix.size(), suffix.size(), suffix) == 0)
                    parts.authority.erase(parts.authority.size() - suffix.size());
                return parts.scheme + "://" + parts.authority;
            };
            BrowserScriptResponse response;
            if (value != "GET" && value != "HEAD") response.error = "Only GET/HEAD requests are supported";
            else if (origin(resolved).empty() || origin(resolved) != origin(self->url)) response.error = "Cross-origin response access denied";
            else if (!self->host.request) response.error = "Network requests are unavailable";
            else
            {
                response = self->host.request(resolved, value);
                if (!response.final_url.empty() && origin(response.final_url) != origin(self->url))
                { response = BrowserScriptResponse(); response.error = "Cross-origin redirect response access denied"; }
            }
            auto output = JS_NewObject(context);
            if (JS_IsException(output)) return JS_EXCEPTION;
            auto property = [&](const char *name, JSValue item) {
                return !JS_IsException(item) && JS_SetPropertyStr(context, output, name, item) >= 0;
            };
            if (!property("status", JS_NewInt32(context, response.status)) ||
                !property("body", string(response.body)) || !property("url", string(response.final_url)) ||
                !property("contentType", string(response.content_type)) || !property("error", string(response.error)))
            { JS_FreeValue(context, output); return JS_EXCEPTION; }
            return output;
        }
        if (operation == "navigate")
        {
            if (self->host.navigate) self->host.navigate(browser_resolve_url(self->url, key), value == "true");
            return JS_UNDEFINED;
        }
        if (operation == "log") { if (self->host.log) self->host.log(key); return JS_UNDEFINED; }
        if (operation == "cookie") return string(self->host.get_cookie ? self->host.get_cookie() : "");
        if (operation == "cookie-set") { if (self->host.set_cookie) self->host.set_cookie(key); return JS_UNDEFINED; }
        if (operation == "title") { self->document->container()->set_caption(key.c_str()); return JS_UNDEFINED; }
        if (operation == "id" || operation == "name")
        {
            std::vector<litehtml::element::ptr> found, pending {root};
            while (!pending.empty())
            {
                auto current = pending.back(); pending.pop_back();
                if (generated(current)) continue;
                const auto *attribute = current->get_attr(operation.c_str());
                if (attribute && key == attribute) found.push_back(current);
                for (auto it = current->children().rbegin(); it != current->children().rend(); ++it) pending.push_back(*it);
            }
            return operation == "id" ? identifier(found.empty() ? nullptr : found.front()) : array(found);
        }
        if (operation == "create")
        {
            if (!self->reserve_nodes(1)) return JS_ThrowRangeError(context, "DOM node budget exceeded");
            auto created = self->document->create_element(key.c_str(), {});
            if (created) created->compute_styles(false);
            self->track_node(created);
            return identifier(created);
        }
        if (operation == "create-comment")
        {
            if (!self->reserve_nodes(1)) return JS_ThrowRangeError(context, "DOM node budget exceeded");
            auto comment = std::make_shared<litehtml::el_comment>(self->document);
            comment->set_data(key.c_str());
            self->track_node(comment);
            return identifier(comment);
        }
        if (operation == "create-text")
        {
            if (!self->reserve_nodes(1)) return JS_ThrowRangeError(context, "DOM node budget exceeded");
            auto created = std::make_shared<litehtml::el_text>(key.c_str(), self->document);
            self->track_node(created);
            return identifier(created);
        }
        if (operation == "query" || operation == "query-all")
        {
            if (!element) element = root;
            if (key.empty()) return operation == "query" ? JS_NewInt32(context, 0) : JS_NewArray(context);
            const auto found = element->select_all(key);
            std::vector<litehtml::element::ptr> descendants;
            for (const auto &match : found)
                if (!generated(match) && (!number(1) || match != element)) descendants.push_back(match);
            if (operation == "query") return identifier(descendants.empty() ? nullptr : descendants.front());
            return array(descendants);
        }
        if (operation == "write")
        {
            if (self->writes.size() + key.size() > 2 * 1024 * 1024) return JS_ThrowRangeError(context, "document.write budget exceeded");
            auto body = root->select_one("body");
            if (body && !self->replace_html(body, key, false)) return JS_ThrowRangeError(context, "DOM node budget exceeded");
            self->writes += key;
            return JS_UNDEFINED;
        }
        if (!element) return JS_NULL;
        if (operation == "parent") return identifier(element->parent());
        if (operation == "children") return array({element->children().begin(), element->children().end()});
        if (operation == "relative")
        {
            const bool first = key == "firstChild" || key == "firstElementChild";
            const bool last = key == "lastChild" || key == "lastElementChild";
            const bool forward = first || key == "nextSibling" || key == "nextElementSibling";
            const bool elements_only = key == "firstElementChild" || key == "lastElementChild" ||
                key == "nextElementSibling" || key == "previousElementSibling";
            const auto parent = first || last ? element : element->parent();
            if (!parent) return identifier(nullptr);
            const auto &children = parent->children();
            auto position = first ? children.begin() : last ? children.end() :
                std::find(children.begin(), children.end(), element);
            if (!first && !last)
            {
                if (position == children.end()) return identifier(nullptr);
                if (forward) ++position;
            }
            if (forward)
            {
                for (; position != children.end(); ++position)
                    if (!generated(*position) && (!elements_only || (!(*position)->is_text() && !(*position)->is_comment())))
                        return identifier(*position);
            }
            else while (position != children.begin())
            {
                --position;
                if (!generated(*position) && (!elements_only || (!(*position)->is_text() && !(*position)->is_comment())))
                    return identifier(*position);
            }
            return identifier(nullptr);
        }
        if (operation == "get")
        {
            if (key == "nodeType") return JS_NewInt32(context, element->is_comment() ? 8 :
                element->is_text() ? 3 : std::strcmp(element->get_tagName(), "xj-fragment") == 0 ? 11 : 1);
            std::string name = element->get_tagName();
            if (std::strcmp(element->get_tagName(), "xj-fragment") == 0) name = "#document-fragment";
            else if (element->is_comment()) name = "#comment";
            else if (element->is_text()) name = "#text";
            else std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::toupper(ch); });
            return string(name);
        }
        if (operation == "attr-names")
        {
            auto names = JS_NewArray(context);
            if (JS_IsException(names)) return JS_EXCEPTION;
            unsigned index = 0;
            for (const auto &attribute : element->attributes())
            {
                auto item = string(attribute.first);
                if (JS_IsException(item) || JS_SetPropertyUint32(context, names, index++, item) < 0)
                { JS_FreeValue(context, names); return JS_EXCEPTION; }
            }
            return names;
        }
        if (operation == "attr")
        {
            const auto *attribute = element->get_attr(key.c_str());
            return attribute ? string(attribute) : JS_NULL;
        }
        if (operation == "attr-set")
        {
            element->set_attr(key.c_str(), value.c_str()); self->dirty = true; ++self->mutations;
            // src assignment to an already inserted script starts its fetch.
            if (key == "src" && self->connected(element) &&
                std::strcmp(element->get_tagName(), "script") == 0 && self->host.script_added)
                self->host.script_added(element);
            if (std::strcmp(element->get_tagName(), "link") == 0 && self->host.link_added && self->connected(element))
                self->host.link_added(element);
            return JS_UNDEFINED;
        }
        if (operation == "attr-remove")
        {
            element->remove_attr(key.c_str()); self->dirty = true; ++self->mutations;
            if (std::strcmp(element->get_tagName(), "link") == 0 && self->host.link_added && self->connected(element))
                self->host.link_added(element);
            return JS_UNDEFINED;
        }
        if (operation == "text") return string(dom_text(element));
        if (operation == "text-set")
        {
            if (element->is_text() || element->is_comment()) element->set_data(key.c_str());
            else
            {
                if (!self->replace_text(element, key)) return JS_ThrowRangeError(context, "DOM node budget exceeded");
            }
            self->dirty = true;
            ++self->mutations;
            return JS_UNDEFINED;
        }
        if (operation == "html" || operation == "outer")
        {
            std::string output;
            if (operation == "outer") output = self->markup(element);
            else if (std::strcmp(element->get_tagName(), "script") == 0 || std::strcmp(element->get_tagName(), "style") == 0)
                output = dom_text(element);
            else for (const auto &child : element->children()) output += self->markup(child);
            return string(output);
        }
        if (operation == "html-set")
        {
            if (key.size() > 2 * 1024 * 1024) return JS_ThrowRangeError(context, "HTML fragment budget exceeded");
            const std::string tag = element->get_tagName();
            const bool raw = tag == "script" || tag == "style";
            if (!(raw ? self->replace_text(element, key) : self->replace_html(element, key, true)))
                return JS_ThrowRangeError(context, "DOM node budget exceeded");
            self->dirty = true;
            ++self->mutations;
            return JS_UNDEFINED;
        }
        if (operation == "clone")
        {
            auto copy = self->clone(element, key == "true");
            return copy ? identifier(copy) : JS_ThrowRangeError(context, "DOM clone budget exceeded");
        }
        if (operation == "matches") return JS_NewBool(context, element->select(key) != litehtml::select_no_match);
        if (operation == "append" || operation == "insert")
        {
            auto child = self->node(number(2));
            if (!child) return JS_ThrowTypeError(context, "Invalid DOM insertion node");
            auto ancestor = element;
            unsigned remaining = 50001;
            for (; ancestor && remaining--; ancestor = ancestor->parent())
                if (ancestor == child) return JS_ThrowTypeError(context, "Cyclic DOM insertion");
            if (ancestor) return JS_ThrowRangeError(context, "DOM ancestry budget exceeded");
            if (operation == "insert" && self->node(number(3)) == child) return JS_UNDEFINED;
            if (auto parent = child->parent()) parent->removeChild(child);
            if (operation == "append") element->appendChild(child);
            else
            {
                auto tag = std::dynamic_pointer_cast<litehtml::html_tag>(element);
                auto reference = self->node(number(3));
                if (!tag || !reference) element->appendChild(child);
                else { auto &children = tag->children(); const auto it = std::find(children.begin(), children.end(), reference); children.insert(it, child); child->parent(element); }
            }
            if (!self->reparent_wrapper(child)) return JS_EXCEPTION;
            self->added(child);
            return JS_UNDEFINED;
        }
        if (operation == "remove")
        {
            auto child = self->node(number(2));
            if (child) { element->removeChild(child); if (!self->reparent_wrapper(child)) return JS_EXCEPTION; }
            self->dirty = true; ++self->mutations; return JS_UNDEFINED;
        }
        if (operation == "control-get")
        {
            if (self->host.get_control) return string(self->host.get_control(element, key));
            if (key == "checked" || key == "selected") return string(element->get_attr(key.c_str()) ? "true" : "false");
            if (std::strcmp(element->get_tagName(), "textarea") == 0) { std::string content; element->get_text(content); return string(content); }
            return string(element->get_attr(key.c_str(), ""));
        }
        if (operation == "control-set")
        {
            if (self->host.set_control) self->host.set_control(element, key, value);
            else if (key == "checked" || key == "selected")
            { if (value == "true") element->set_attr(key.c_str(), ""); else element->remove_attr(key.c_str()); }
            else element->set_attr(key.c_str(), value.c_str());
            self->dirty = true; return JS_UNDEFINED;
        }
        if (operation == "focus") { if (self->host.focus) self->host.focus(element); return JS_UNDEFINED; }
        if (operation == "submit") { if (self->host.submit) self->host.submit(element); return JS_UNDEFINED; }
        if (operation == "click")
        {
            if (const auto *href = element->get_attr("href"))
            { if (self->host.navigate) self->host.navigate(browser_resolve_url(self->url, href), false); }
            else if (self->host.submit) self->host.submit(element);
            return JS_UNDEFINED;
        }
        if (operation == "rect")
        {
            const auto rect = element->get_placement();
            auto result = JS_NewObject(context);
            if (JS_IsException(result)) return JS_EXCEPTION;
            const auto view = self->host.viewport ? self->host.viewport() : BrowserScriptViewport{};
            if (JS_SetPropertyStr(context, result, "x", JS_NewFloat64(context, rect.x - view.scroll_x)) < 0 ||
                JS_SetPropertyStr(context, result, "y", JS_NewFloat64(context, rect.y - view.scroll_y)) < 0 ||
                JS_SetPropertyStr(context, result, "width", JS_NewFloat64(context, rect.width)) < 0 ||
                JS_SetPropertyStr(context, result, "height", JS_NewFloat64(context, rect.height)) < 0)
            { JS_FreeValue(context, result); return JS_EXCEPTION; }
            return result;
        }
        if (operation == "computed")
        {
            if (key == "display") return string(element->css().get_display() == litehtml::display_none ? "none" : "block");
            if (key == "visibility") return string("visible");
            return string("");
        }
        return JS_UNDEFINED;
    }
    bool initialize()
    {
        const JSMallocFunctions functions {js_allocate, js_release, js_resize, js_size};
        runtime = JS_NewRuntime2(&functions, &allocations);
        if (!runtime) { error = "Could not create JavaScript runtime"; return false; }
        JS_SetMemoryLimit(runtime, 64 * 1024 * 1024);
        JS_SetMaxStackSize(runtime, 512 * 1024);
        JS_SetInterruptHandler(runtime, interrupted, this);
        context = JS_NewContext(runtime);
        if (!context) { error = "Could not create JavaScript context"; return false; }
        JS_SetContextOpaque(context, this);
        JS_SetModuleLoaderFunc2(runtime, normalize_module, load_cached_module, check_module_attributes, this);
        JS_SetHostImportModuleFunc(runtime, import_module, this);
        JS_SetHostPromiseRejectionTracker(runtime, promise_rejection, this);
        if (!browser_dom_node_class) JS_NewClassID(&browser_dom_node_class);
        JSClassDef definition {};
        definition.class_name = "BrowserDOMNode";
        definition.finalizer = node_finalize;
        definition.gc_mark = node_mark;
        static JSClassExoticMethods exotic = [] {
            JSClassExoticMethods methods {};
            methods.get_own_property = node_own_property;
            return methods;
        }();
        definition.exotic = &exotic;
        if (JS_NewClass(runtime, browser_dom_node_class, &definition) < 0) return false;
        auto global = JS_GetGlobalObject(context);
        JS_SetPropertyStr(context, global, "__browser", JS_NewCFunction(context, bridge, "__browser", 4));
        JS_FreeValue(context, global);
        JS_UpdateStackTop(runtime);
        budget = 4000;
        if (!result(JS_Eval(context, browser_js_bootstrap, sizeof(browser_js_bootstrap) - 1,
                            "xj380:web-platform", JS_EVAL_TYPE_GLOBAL))) return false;
        global = JS_GetGlobalObject(context);
        for (const char *name : {"__dispatch", "__lifecycle", "__pump", "__currentScript", "__node", "__historyChanged", "__viewChanged"})
        {
            callbacks.emplace(name, JS_GetPropertyStr(context, global, name));
            const auto atom = JS_NewAtom(context, name);
            JS_DeleteProperty(context, global, atom, 0);
            JS_FreeAtom(context, atom);
        }
        JS_FreeValue(context, global);
        if (!track_tree(document->root())) { error = "Document node budget exceeded"; return false; }
        auto root_wrapper = wrap_node(id(document->root()));
        return result(root_wrapper);
    }
};

BrowserScriptRuntime::BrowserScriptRuntime() = default;
BrowserScriptRuntime::~BrowserScriptRuntime() = default;
void BrowserScriptRuntime::attach(const litehtml::document::ptr &document, const std::string &url, BrowserScriptHost host)
{
    impl_ = std::make_unique<Impl>();
    impl_->document = document;
    impl_->url = url;
    impl_->host = std::move(host);
    impl_->time_origin = impl_->now_ms();
    if (document) impl_->initialized = impl_->initialize();
}
unsigned long long BrowserScriptRuntime::start_module(const std::string &source, const std::string &url,
                                                       const std::string &base_url, bool inline_module)
{
    if (!active()) return 0;
    auto &self = *impl_;
    JS_UpdateStackTop(self.runtime);
    self.budget = 4000;
    JS_SetUncatchableException(self.context, false);
    try
    {
        if (self.module_entries.size() >= 512) { self.error = "Module entry budget exceeded"; return 0; }
        const auto entry = module_token();
        const auto key = inline_module ? "browser:inline-module:" + std::to_string(entry) : self.module_url(self.url, url);
        const auto base = self.module_url(self.url, base_url.empty() ? url : base_url);
        if (key.empty() || base.empty()) { self.error = "Invalid module URL"; return 0; }
        auto found = self.modules.find(key);
        if (found == self.modules.end())
        {
            if (self.modules.size() >= 256) { self.error = "Module graph budget exceeded"; return 0; }
            Impl::Module module; module.key = key; module.base = base;
            found = self.modules.emplace(key, std::move(module)).first;
        }
        self.module_entries.emplace(entry, Impl::ModuleEntry{key, false});
        if (!found->second.has_source && found->second.state != BrowserScriptModuleState::Rejected)
        {
            found->second.base = base;
            self.compile_module(found->second, source);
        }
        self.jobs();
        return entry;
    }
    catch (const std::exception &failure) { self.error = std::string("Native module error: ") + failure.what(); return 0; }
    catch (...) { self.error = "Native module error"; return 0; }
}
unsigned long long BrowserScriptRuntime::preload_module(const std::string &source, const std::string &url,
                                                         const std::string &base_url)
{
    if (!active()) return 0;
    auto &self = *impl_;
    JS_UpdateStackTop(self.runtime);
    self.budget = 4000;
    JS_SetUncatchableException(self.context, false);
    try
    {
        if (self.module_entries.size() >= 512) { self.error = "Module entry budget exceeded"; return 0; }
        const auto key = self.module_url(self.url, url);
        const auto base = self.module_url(self.url, base_url.empty() ? url : base_url);
        if (key.empty() || base.empty()) { self.error = "Invalid module URL"; return 0; }
        auto found = self.modules.find(key);
        if (found == self.modules.end())
        {
            if (self.modules.size() >= 256) { self.error = "Module graph budget exceeded"; return 0; }
            Impl::Module module; module.key = key; module.base = base;
            found = self.modules.emplace(key, std::move(module)).first;
        }
        const auto entry = module_token();
        self.module_entries.emplace(entry, Impl::ModuleEntry{key, true});
        if (!found->second.has_source && found->second.state != BrowserScriptModuleState::Rejected)
        {
            found->second.base = base;
            self.compile_module(found->second, source);
        }
        self.jobs();
        return entry;
    }
    catch (const std::exception &failure) { self.error = std::string("Native module error: ") + failure.what(); return 0; }
    catch (...) { self.error = "Native module error"; return 0; }
}
std::vector<BrowserScriptModuleRequest> BrowserScriptRuntime::take_module_requests()
{
    if (!impl_) return {};
    auto result = std::move(impl_->module_requests);
    impl_->module_requests.clear();
    return result;
}
void BrowserScriptRuntime::provide_module(unsigned long long token, const BrowserScriptResponse &response)
{
    if (!active()) return;
    auto &self = *impl_;
    const auto request = self.module_fetches.find(token);
    if (request == self.module_fetches.end()) return;
    const auto key = request->second;
    self.module_fetches.erase(request);
    auto &module = self.modules.at(key);
    if (module.has_source || module.state == BrowserScriptModuleState::Rejected) return;
    JS_UpdateStackTop(self.runtime);
    self.budget = 4000;
    JS_SetUncatchableException(self.context, false);
    try
    {
        if (!response.error.empty() || response.status < 200 || response.status >= 300)
            self.module_failure(module, response.error.empty() ? "Module HTTP status " + std::to_string(response.status) : response.error);
        else
        {
            module.base = response.final_url.empty() ? key : self.module_url(key, response.final_url);
            if (module.base.empty()) self.module_failure(module, "Invalid final module URL");
            else self.compile_module(module, response.body);
        }
        self.jobs();
    }
    catch (const std::exception &failure) { self.module_failure(module, std::string("Native module error: ") + failure.what()); }
    catch (...) { self.module_failure(module, "Native module error"); }
}
BrowserScriptModuleState BrowserScriptRuntime::module_state(unsigned long long entry) const
{
    if (!impl_) return BrowserScriptModuleState::Unknown;
    const auto found = impl_->module_entries.find(entry);
    if (found == impl_->module_entries.end()) return BrowserScriptModuleState::Unknown;
    const auto &module = impl_->modules.at(found->second.key);
    if (found->second.preload && module.linked) return BrowserScriptModuleState::Fulfilled;
    return module.state;
}
bool BrowserScriptRuntime::module_evaluation_started(unsigned long long entry) const
{
    if (!impl_) return false;
    const auto found = impl_->module_entries.find(entry);
    return found != impl_->module_entries.end() && !found->second.preload && impl_->modules.at(found->second.key).started;
}
bool BrowserScriptRuntime::module_work_pending() const
{
    return active() && (!impl_->module_requests.empty() || !impl_->module_fetches.empty() ||
        !impl_->dynamic_modules.empty() || JS_IsJobPending(impl_->runtime));
}
std::string BrowserScriptRuntime::module_error(unsigned long long entry) const
{
    if (!impl_) return {};
    const auto found = impl_->module_entries.find(entry);
    return found == impl_->module_entries.end() ? std::string() : impl_->modules.at(found->second.key).failure;
}

void BrowserScriptRuntime::history_changed(const std::string &url, const std::string &state_json)
{
    if (!active()) return;
    impl_->url = url;
    JSValue args[] {JS_NewStringLen(impl_->context, url.data(), url.size()),
                    JS_NewStringLen(impl_->context, state_json.data(), state_json.size())};
    impl_->result(impl_->call("__historyChanged", 2, args));
    for (const auto value : args) JS_FreeValue(impl_->context, value);
    impl_->jobs();
}
void BrowserScriptRuntime::view_changed(const std::string &type)
{
    if (!active()) return;
    JSValue argument = JS_NewString(impl_->context, type.c_str());
    impl_->result(impl_->call("__viewChanged", 1, &argument));
    JS_FreeValue(impl_->context, argument);
    impl_->jobs();
}

bool BrowserScriptRuntime::evaluate(const std::string &source, const std::string &url)
{
    if (!active()) return false;
    if (source.size() > 4 * 1024 * 1024) { impl_->error = "Script source budget exceeded"; return false; }
    JS_UpdateStackTop(impl_->runtime);
    impl_->budget = 4000;
    JS_SetUncatchableException(impl_->context, false);
    const auto good = impl_->result(JS_Eval(impl_->context, source.data(), source.size(), url.c_str(), JS_EVAL_TYPE_GLOBAL));
    impl_->jobs();
    return good;
}
bool BrowserScriptRuntime::dispatch(const litehtml::element::ptr &target, const std::string &type,
                                    const std::string &key, int key_code)
{
    if (!active()) return false;
    auto event_target = target;
    while (event_target && generated(event_target)) event_target = event_target->parent();
    JSValue args[] {JS_NewUint32(impl_->context, impl_->id(event_target)), JS_NewString(impl_->context, type.c_str()),
                    JS_NewString(impl_->context, key.c_str()), JS_NewInt32(impl_->context, key_code)};
    auto result = impl_->call("__dispatch", 4, args);
    const bool prevented = !JS_IsException(result) && JS_ToBool(impl_->context, result) > 0;
    impl_->result(result);
    for (auto value : args) JS_FreeValue(impl_->context, value);
    impl_->jobs();
    return prevented;
}
void BrowserScriptRuntime::lifecycle(const std::string &type)
{
    if (!active()) return;
    JSValue arg = JS_NewString(impl_->context, type.c_str());
    impl_->result(impl_->call("__lifecycle", 1, &arg));
    JS_FreeValue(impl_->context, arg);
    impl_->jobs();
}
void BrowserScriptRuntime::pump(unsigned long long now_ms)
{
    if (!active()) return;
    JSValue arg = JS_NewFloat64(impl_->context, static_cast<double>(now_ms));
    impl_->result(impl_->call("__pump", 1, &arg));
    JS_FreeValue(impl_->context, arg);
    impl_->jobs();
}
bool BrowserScriptRuntime::take_dirty()
{
    if (!impl_) return false;
    const bool dirty = impl_->dirty;
    impl_->dirty = false;
    return dirty;
}
const std::string &BrowserScriptRuntime::last_error() const
{
    static const std::string empty;
    return impl_ ? impl_->error : empty;
}
const std::string &BrowserScriptRuntime::written_html() const
{
    static const std::string empty;
    return impl_ ? impl_->writes : empty;
}
bool BrowserScriptRuntime::active() const { return impl_ && impl_->context && impl_->initialized; }

void BrowserScriptRuntime::set_current_script(const litehtml::element::ptr &node)
{
    if (!active()) return;
    JSValue arg = JS_NewUint32(impl_->context, impl_->id(node));
    impl_->result(impl_->call("__currentScript", 1, &arg));
    JS_FreeValue(impl_->context, arg);
}

void BrowserScriptRuntime::retain_node(const litehtml::element::ptr &node)
{
    if (!active() || !node) return;
    auto found = impl_->external_nodes.find(node.get());
    if (found != impl_->external_nodes.end()) { ++found->second.second; return; }
    auto wrapper = impl_->wrap_node(impl_->id(node));
    if (JS_IsException(wrapper)) { impl_->result(wrapper); return; }
    impl_->external_nodes.emplace(node.get(), std::make_pair(wrapper, 1U));
}
void BrowserScriptRuntime::release_node(const litehtml::element::ptr &node)
{
    if (!active() || !node) return;
    const auto found = impl_->external_nodes.find(node.get());
    if (found == impl_->external_nodes.end() || --found->second.second) return;
    const auto value = found->second.first;
    impl_->external_nodes.erase(found);
    JS_FreeValue(impl_->context, value);
}
void BrowserScriptRuntime::collect_garbage()
{
    if (!active()) return;
    impl_->discard_unwrapped();
    impl_->collect_nodes();
}
std::size_t BrowserScriptRuntime::live_nodes()
{
    if (!impl_) return 0;
    impl_->sweep_nodes();
    return impl_->managed_nodes.size();
}

std::size_t BrowserScriptRuntime::memory_bytes() const
{
    return impl_ ? impl_->allocations.used : 0;
}
std::size_t BrowserScriptRuntime::allocation_failures() const
{
    return impl_ ? impl_->allocations.failures : 0;
}

BrowserScriptMemoryStatistics BrowserScriptRuntime::memory_statistics() const
{
    BrowserScriptMemoryStatistics result;
    if (impl_)
    {
        result.used = impl_->allocations.used;
        result.peak = impl_->allocations.peak;
        result.allocation_failures = impl_->allocations.failures;
        result.collections = impl_->collections;
        result.last_collection_before = impl_->collection_before;
        result.last_collection_after = impl_->collection_after;
        result.large_live_bytes = impl_->allocations.large_live;
        result.large_peak_bytes = impl_->allocations.large_peak;
        result.large_live_count = impl_->allocations.large_count;
        result.largest_successful_allocation = impl_->allocations.max_success;
        result.largest_allocation_request = impl_->allocations.max_request;
        for (unsigned i = 0; i < 8; ++i)
        {
            result.large_bucket_bytes[i] = impl_->allocations.bucket_bytes[i];
            result.large_bucket_counts[i] = impl_->allocations.bucket_counts[i];
        }
    }
    return result;
}
BrowserScriptMemoryBreakdown BrowserScriptRuntime::memory_breakdown() const
{
    return impl_ ? impl_->memory_breakdown() : BrowserScriptMemoryBreakdown();
}
