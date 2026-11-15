"""Check the static browser's loader shim against the real host GCC unwinder."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StaticUnwindTests(unittest.TestCase):
    def test_registered_frames_work_without_a_dynamic_loader(self):
        source = (ROOT / "browser_start.cpp").read_text()
        start = source.index('extern "C" int dl_iterate_phdr(')
        stop = source.index("\n}", start) + 2
        shim = source[start:stop]
        harness = r'''
#include <cassert>
struct dl_phdr_info;
extern "C" void __register_frame(const void *);
extern "C" void __deregister_frame(const void *);
extern "C" char __test_eh_frame_start[];
// Force the registered-frame path even on glibc hosts with _dl_find_object.
extern "C" int __wrap__dl_find_object(void *, void *) { return -1; }
''' + shim + r'''
static unsigned callbacks;
static int visitor(dl_phdr_info *, unsigned long, void *) { ++callbacks; return 1; }
struct Failure { int value; };
__attribute__((noinline)) static void nested_throw()
{
    try { throw Failure{42}; }
    catch (Failure &failure) { ++failure.value; throw; }
}
int main()
{
    assert(dl_iterate_phdr(visitor, nullptr) == 0 && callbacks == 0);
    __register_frame(__test_eh_frame_start);
    bool caught = false;
    try { nested_throw(); }
    catch (const Failure &failure) { assert(failure.value == 43); caught = true; }
    assert(caught);
    __deregister_frame(__test_eh_frame_start);
}
'''
        with tempfile.TemporaryDirectory(prefix="xj380-browser-unwind-") as directory:
            work = Path(directory)
            (work / "probe.cpp").write_text(harness)
            script = work / "frames.ld"
            script.write_text("SECTIONS { .eh_frame : { __test_eh_frame_start = .; KEEP(*(.eh_frame)) } } "
                              "INSERT AFTER .rodata;\n")
            binary = work / "probe"
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
                            "-Werror", "-static", "-fno-pie", "-no-pie", "-Wl,--no-eh-frame-hdr",
                            "-Wl,--wrap=_dl_find_object",
                            "-Wl,-T," + str(script), str(work / "probe.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
