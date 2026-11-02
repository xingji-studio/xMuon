#include "../xapi/include/x3api.h"
#include "../xapi/include/libsys.h"
#define BROWSER_TLS_XAPI
#include "browser_tls_runtime.h"
#undef BROWSER_TLS_XAPI

extern "C" int browser_cpp_main(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" const BrowserTlsImage __browser_tls_image;
extern "C" uint64_t __stack_chk_guard;
extern "C" void (*__preinit_array_start[])(void);
extern "C" void (*__preinit_array_end[])(void);
extern "C" void (*__init_array_start[])(void);
extern "C" void (*__init_array_end[])(void);
extern "C" const unsigned char __browser_eh_frame_start[];
extern "C" void __register_frame(const void *begin);
extern "C" void __deregister_frame(const void *begin);
extern "C" void *g_browser_heap_base = NULL;
extern "C" unsigned long long g_browser_heap_size = 0;

static uintptr_t browser_read_fs()
{
    return enter_syscall(SYS_ARCH_PRCTL, ARCH_GET_FS, 0, 0, 0, 0, 0);
}

static bool browser_write_fs(uintptr_t value)
{
    return static_cast<int64_t>(enter_syscall(SYS_ARCH_PRCTL, ARCH_SET_FS, value, 0, 0, 0, 0)) >= 0;
}

static void run_init_array(void (**begin)(void), void (**end)(void))
{
    for (void (**fn)(void) = begin; fn != end; ++fn) {
        if (*fn != 0) {
            (*fn)();
        }
    }
}

extern "C" __attribute__((force_align_arg_pointer)) void xapi_start(int argc, char *argv[], char *envp[])
{
    // browser 使用自定义启动器，也需要显式挂接 libc environ。
    environ = envp;

    const BrowserTlsHooks tls_hooks {malloc, free, browser_read_fs, browser_write_fs};
    BrowserTlsRuntime runtime_tls;
    if (!browser_tls_prepare(__browser_tls_image, tls_hooks, __stack_chk_guard, runtime_tls)) {
        xapi_OutputSerial((char *)"browser: prepare runtime failed\n");
        exit(-1);
    }

    // There is no crtbegin/crtend in the freestanding link. Constructors and
    // browser error paths still use C++ exceptions, so register the linker-
    // terminated frame table before any hosted C++ code executes.
    __register_frame(__browser_eh_frame_start);
    run_init_array(__preinit_array_start, __preinit_array_end);
    run_init_array(__init_array_start, __init_array_end);

    int ret = browser_cpp_main(argc, argv, envp);
    __deregister_frame(__browser_eh_frame_start);
    if (!browser_tls_restore(tls_hooks, runtime_tls)) {
        xapi_OutputSerial((char *)"browser: restore runtime failed\n");
    }
    exit(ret);
}
