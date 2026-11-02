#include "browser_parser_memory.h"

#include <cstdlib>
#include <new>
#include <gumbo.h>

extern "C" GumboOutput *__real_gumbo_parse_with_options(const GumboOptions *, const char *, size_t);
extern "C" void __real_gumbo_destroy_output(const GumboOptions *, GumboOutput *);

namespace
{

struct ParserRecord
{
    GumboOutput  *output;
    ParserRecord *next;
};

BrowserParserScope *g_scope = nullptr;
unsigned long long g_live_allocations = 0;

void *parser_allocate(void *, size_t bytes)
{
    void *result = std::malloc(bytes);
    if (result != nullptr) __atomic_add_fetch(&g_live_allocations, 1ULL, __ATOMIC_RELAXED);
    return result;
}

void parser_deallocate(void *, void *pointer)
{
    if (pointer == nullptr) return;
    __atomic_sub_fetch(&g_live_allocations, 1ULL, __ATOMIC_RELAXED);
    std::free(pointer);
}

GumboOptions owned_options(const GumboOptions *options)
{
    GumboOptions owned = options != nullptr ? *options : kGumboDefaultOptions;
    // The vendored document adapter disables deallocation during parsing.
    // Override both callbacks, at both entry points: fixing only final tree
    // destruction still leaks tokenizer buffers and intermediate nodes.
    owned.allocator = &parser_allocate;
    owned.deallocator = &parser_deallocate;
    owned.userdata = nullptr;
    return owned;
}

} // namespace

struct BrowserParserAccess
{
    static void track(GumboOutput *output)
    {
        if (g_scope == nullptr || output == nullptr) return;
        auto *record = static_cast<ParserRecord *>(std::malloc(sizeof(ParserRecord)));
        if (record == nullptr)
        {
            GumboOptions options = owned_options(nullptr);
            __real_gumbo_destroy_output(&options, output);
            throw std::bad_alloc();
        }
        record->output = output;
        record->next = static_cast<ParserRecord *>(g_scope->records_);
        g_scope->records_ = record;
    }

    static void forget(GumboOutput *output)
    {
        for (BrowserParserScope *scope = g_scope; scope != nullptr; scope = scope->previous_)
        {
            auto *record = static_cast<ParserRecord *>(scope->records_);
            ParserRecord *previous = nullptr;
            while (record != nullptr)
            {
                if (record->output == output)
                {
                    if (previous != nullptr) previous->next = record->next;
                    else scope->records_ = record->next;
                    std::free(record);
                    return;
                }
                previous = record;
                record = record->next;
            }
        }
    }
};

BrowserParserScope::BrowserParserScope() : previous_(g_scope)
{
    g_scope = this;
}

BrowserParserScope::~BrowserParserScope()
{
    GumboOptions options = owned_options(nullptr);
    auto *record = static_cast<ParserRecord *>(records_);
    while (record != nullptr)
    {
        ParserRecord *next = record->next;
        __real_gumbo_destroy_output(&options, record->output);
        std::free(record);
        record = next;
    }
    g_scope = previous_;
}

extern "C" GumboOutput *__wrap_gumbo_parse_with_options(const GumboOptions *options, const char *text, size_t bytes)
{
    GumboOptions owned = owned_options(options);
    GumboOutput *output = __real_gumbo_parse_with_options(&owned, text, bytes);
    BrowserParserAccess::track(output);
    return output;
}

extern "C" void __wrap_gumbo_destroy_output(const GumboOptions *options, GumboOutput *output)
{
    if (output == nullptr) return;
    BrowserParserAccess::forget(output);
    GumboOptions owned = owned_options(options);
    __real_gumbo_destroy_output(&owned, output);
}

extern "C" unsigned long long browser_parser_live_allocations()
{
    return __atomic_load_n(&g_live_allocations, __ATOMIC_RELAXED);
}
