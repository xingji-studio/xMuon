#pragma once

#ifdef BROWSER_TLS_XAPI
#include "../xapi/include/stdint.h"
#else
#include <stddef.h>
#include <stdint.h>
#endif

// The static x86-64 TLS image precedes the thread pointer. Its size and
// alignment come from the final ELF, including TLS contributed by libraries.
struct BrowserTlsImage
{
    const unsigned char *data;
    size_t file_size;
    size_t memory_size;
    size_t alignment;
};

// The linker emits four 64-bit words in .browser_tls; this is their exact ABI.
static_assert(sizeof(void *) == 8 && sizeof(size_t) == 8 && sizeof(BrowserTlsImage) == 32,
              "Browser TLS descriptor requires the x86-64 ABI");

struct BrowserTlsHooks
{
    void *(*allocate)(size_t);
    void (*deallocate)(void *);
    uintptr_t (*read_fs)();
    bool (*write_fs)(uintptr_t);
};

struct BrowserTlsRuntime
{
    void *allocation = nullptr;
    uintptr_t previous_fs = 0;
    uintptr_t thread_pointer = 0;
};

inline bool browser_tls_prepare(const BrowserTlsImage &image, const BrowserTlsHooks &hooks,
                                uintptr_t stack_guard, BrowserTlsRuntime &runtime)
{
    const size_t maximum = static_cast<size_t>(-1);
    const size_t alignment = image.alignment;
    if (runtime.allocation || !hooks.allocate || !hooks.deallocate || !hooks.read_fs || !hooks.write_fs ||
        !alignment || (alignment & (alignment - 1)) || image.file_size > image.memory_size ||
        (image.file_size && !image.data) || image.memory_size > maximum - (alignment - 1))
        return false;

    const size_t span = (image.memory_size + alignment - 1) & ~(alignment - 1);
    const size_t pointer_alignment = alignment < 16 ? 16 : alignment;
    const size_t tcb_size = 0x40;
    if (span > maximum - (pointer_alignment - 1) ||
        span + pointer_alignment - 1 > maximum - tcb_size)
        return false;
    const size_t allocation_size = span + pointer_alignment - 1 + tcb_size;
    auto *allocation = static_cast<unsigned char *>(hooks.allocate(allocation_size));
    if (!allocation) return false;

    const uintptr_t address = reinterpret_cast<uintptr_t>(allocation);
    if (address > maximum - allocation_size)
    {
        hooks.deallocate(allocation);
        return false;
    }
    // Align TP independently of the image span: rounding a 40-byte/8-aligned
    // image to 48 would disagree with the linker's local-exec TLS offsets.
    const uintptr_t pointer = (address + span + pointer_alignment - 1) & ~(pointer_alignment - 1);
    auto *tls = reinterpret_cast<unsigned char *>(pointer - span);
    for (size_t index = 0; index < allocation_size; ++index) allocation[index] = 0;
    for (size_t index = 0; index < image.file_size; ++index) tls[index] = image.data[index];
    *reinterpret_cast<uintptr_t *>(pointer) = pointer;
    *reinterpret_cast<uintptr_t *>(pointer + 0x28) = stack_guard;

    const uintptr_t previous = hooks.read_fs();
    if (!hooks.write_fs(pointer))
    {
        hooks.deallocate(allocation);
        return false;
    }
    runtime.allocation = allocation;
    runtime.previous_fs = previous;
    runtime.thread_pointer = pointer;
    return true;
}

inline bool browser_tls_restore(const BrowserTlsHooks &hooks, BrowserTlsRuntime &runtime)
{
    if (!runtime.allocation) return true;
    // A failed restore must not free storage still referenced by the live FS.
    if (!hooks.write_fs(runtime.previous_fs)) return false;
    hooks.deallocate(runtime.allocation);
    runtime = BrowserTlsRuntime {};
    return true;
}
