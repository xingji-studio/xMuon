/* The browser uses a custom entry point instead of the hosted CRT. Give its
 * startup code the complete, terminated frame table needed by libgcc's static
 * exception unwinder. Keep the default ELF linker layout for other sections. */
SECTIONS
{
    /* Preserve the default TLS output sections and their actual padding. */
    __browser_tls_image_start = SIZEOF(.tdata) ? ADDR(.tdata) : ADDR(.tbss);
    __browser_tls_file_size = SIZEOF(.tdata);
    __browser_tls_memory_size = SIZEOF(.tbss) ?
        ADDR(.tbss) + SIZEOF(.tbss) - __browser_tls_image_start : SIZEOF(.tdata);
    __browser_tls_alignment = MAX(1, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));

    /* Numeric TLS metadata must be stored as data. Treating an absolute zero
     * linker symbol as a C object address lets the compiler assume it is nonzero. */
    .browser_tls ALIGN(8) (READONLY) :
    {
        __browser_tls_image = .;
        QUAD(__browser_tls_image_start)
        QUAD(__browser_tls_file_size)
        QUAD(__browser_tls_memory_size)
        QUAD(__browser_tls_alignment)
    }

    .eh_frame ALIGN(8) :
    {
        __browser_eh_frame_start = .;
        KEEP(*(.eh_frame))
        KEEP(*(.eh_frame.*))
        LONG(0)
    }
}
INSERT AFTER .rodata;
