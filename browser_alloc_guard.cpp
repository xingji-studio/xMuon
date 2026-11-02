#include "browser_platform.h"
#include <cstdio>

extern "C" void __real_free(void *ptr);
extern "C" void __real_abort(void);
extern "C" __attribute__((noreturn)) void __real___cxa_throw(void *, void *, void (*)(void *));

extern "C" void *g_browser_heap_base;
extern "C" unsigned long long g_browser_heap_size;

namespace
{

static void log_invalid_free(void *ptr)
{
    char buffer[160];
    snprintf(buffer,
             sizeof(buffer),
             "browser: ignored invalid free ptr=%p heap=[%p,%p)\n",
             ptr,
             g_browser_heap_base,
             (void *)((unsigned char *)g_browser_heap_base + g_browser_heap_size));
    xapi_OutputSerial(buffer);
}

static bool pointer_in_browser_heap(void *ptr)
{
    if (ptr == nullptr || g_browser_heap_base == nullptr || g_browser_heap_size == 0) {
        return true;
    }

    const unsigned long long value = (unsigned long long)ptr;
    const unsigned long long begin = (unsigned long long)g_browser_heap_base;
    const unsigned long long end = begin + g_browser_heap_size;
    return value >= begin && value < end;
}

} // namespace

extern "C" void __wrap_free(void *ptr)
{
    if (ptr == nullptr) {
        __real_free(ptr);
        return;
    }

    if (!pointer_in_browser_heap(ptr)) {
        log_invalid_free(ptr);
    }

    __real_free(ptr);
}

// Used by the runtime probe's --wrap=abort diagnostic. In a build without
// exception unwinding, libstdc++ can abort directly instead of throwing.
extern "C" void __wrap_abort(void)
{
    char message[160];
    snprintf(message, sizeof(message), "browser runtime abort: caller=%p\n", __builtin_return_address(0));
    xapi_OutputSerial(message);
    __real_abort();
    __builtin_unreachable();
}

extern "C" __attribute__((noreturn)) void __wrap___cxa_throw(void *exception, void *type, void (*destroy)(void *))
{
    const char *name = type != nullptr ? static_cast<const char *const *>(type)[1] : "?";
    char message[240];
    snprintf(message, sizeof(message), "browser runtime throw: type=%s caller=%p\n",
             name, __builtin_return_address(0));
    xapi_OutputSerial(message);
    __real___cxa_throw(exception, type, destroy);
}
