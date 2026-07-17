ENTRY(_start)

SECTIONS
{
    /* Must match USER_WINDOW_BASE in include/pureunix/vmm.h — every process
     * gets a private mapping of this same virtual range. */
    . = 0x08000000;
    .text ALIGN(4K) : { *(.text*) }
    .rodata ALIGN(4K) : { *(.rodata*) }
    /* DWARF CFI unwind tables for C++ exceptions. libgcc's unwinder here
     * uses the classic __register_frame()-registered-linked-list lookup
     * (confirmed by its absence of any dl_iterate_phdr reference — that's
     * the alternative mechanism real dynamic loaders provide, which
     * PureUnix has none of, see docs/qt-port.md section 2), not a
     * PT_GNU_EH_FRAME/.eh_frame_hdr binary-search lookup. Without calling
     * __register_frame() on this exact range at startup, any real throw
     * finds no registered unwind info at all and the process takes an
     * invalid-opcode fault. See user/newlib_crt0.c's registration call. */
    .eh_frame ALIGN(4K) : {
        __eh_frame_start = .;
        *(.eh_frame)
        __eh_frame_end = .;
    }
    /* C++ global constructor/destructor pointer tables. Only newlib-linked
     * C++ programs' crt0 (user/newlib_crt0.c) walks these; plain libpure
     * programs' crt0.S doesn't, so these are simply empty arrays for them.
     * Placed inside the .data-covered PT_LOAD segment (not a section type
     * the kernel ELF loader looks for specially — it only ever handles
     * PT_LOAD segments verbatim, see docs/qt-port.md section 2), so no
     * kernel change is needed for constructors/destructors to run.
     *
     * Both .init_array/.fini_array (the modern convention) AND .ctors/
     * .dtors (the legacy one) are captured: this i686-elf-gcc 16.1.0 build
     * turns out to emit legacy .ctors, not .init_array, for ordinary global
     * constructors (confirmed by inspecting a real compiled .o — ELF loader
     * fact, not a documented compiler default, so keeping both is cheap
     * insurance against it differing per translation unit or future
     * toolchain rebuilds). No crtbegin.o/crtend.o exists on this target,
     * so there's no -1/0 sentinel scan — user/newlib_crt0.c just iterates
     * flat between these explicit start/end symbols instead. */
    .data ALIGN(4K) : {
        *(.data*)
        . = ALIGN(4);
        __init_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP (*(.init_array))
        __init_array_end = .;
        . = ALIGN(4);
        __fini_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP (*(.fini_array))
        __fini_array_end = .;
        . = ALIGN(4);
        __ctors_start = .;
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
        __ctors_end = .;
        . = ALIGN(4);
        __dtors_start = .;
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
        __dtors_end = .;
    }
    .bss ALIGN(4K) : { *(COMMON) *(.bss*) }
    /* Real C11/C++11 `thread_local` storage duration (i386 Variant II TLS
     * ABI, R_386_TLS_LE "local exec" relocations — the only TLS model that
     * makes sense for a statically-linked, no-dynamic-loader executable
     * like every PureUnix program, see docs/qt-port.md section 2). The
     * kernel ELF loader has no PT_TLS handling at all (only ever walks
     * PT_LOAD segments, same fact that already made .init_array/.ctors
     * pure userspace work above) — .tdata/.tbss end up inside this
     * PT_LOAD-covered image exactly like any other data, and it's entirely
     * up to user/newlib_crt0.c's tls_init() to actually allocate a real
     * per-task TLS block, copy this template into it, and tell the kernel
     * about it (SYS_SET_TLS). What has to be exact here is the *layout*:
     * .tdata immediately followed by .tbss, no gap, standard ld TLS
     * alignment — R_386_TLS_LE offsets embedded in the compiled code
     * (e.g. Qt6's QBindingStorage doing `%gs:-0x18`) are computed by ld
     * purely from this template's own internal layout, independent of
     * where in the final binary it physically lives; __tdata_start/
     * __tbss_end just have to bracket that exact same template so
     * tls_init() can reproduce it faithfully at runtime. ALIGN(16) is
     * deliberately generous (i386's own natural max alignment need is 8,
     * for double/int64_t) — cheap insurance, not a precisely-reasoned
     * minimum. */
    .tdata ALIGN(16) : {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    }
    .tbss ALIGN(16) : {
        __tbss_start = .;
        *(.tbss .tbss.*)
        __tbss_end = .;
    }
}
