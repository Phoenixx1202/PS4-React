// Replacement for OpenOrbis' lib/crt1.o. Same entry point and process
// parameters, but _sceLibcParam's malloc table points at the ps4_replace_*
// functions in ps4_heap.cpp. Based on DolphinPS4 by iHaiDeeZ and PS4SX2.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stock crt1.o leaves that table empty; libc then builds its own heap by
// mapping about 2.5 GiB of system flexible memory on the first malloc. That
// fails on retail consoles and the next allocation faults at address 0x38.
//
// SceLibcMallocReplace (version 1, 0x70 bytes): size, version, init, finalize,
// malloc, free, calloc, realloc, memalign, reallocalign, posix_memalign,
// malloc_stats, malloc_stats_fast, malloc_usable_size.

    .text
    .globl _start
    .type _start, @function
    .p2align 3
_start:
    jmp _start_ps4_c
    .p2align 3

    .section .text._start_ps4_c, "ax", @progbits
    .globl _start_ps4_c
    .type _start_ps4_c, @function
_start_ps4_c:
    movl (%rdi), %esi
    leaq 8(%rdi), %rdx
    movq main@GOTPCREL(%rip), %rdi
    movq _init@GOTPCREL(%rip), %rcx
    movq _fini@GOTPCREL(%rip), %r8
    xorl %r9d, %r9d
    jmp __libc_start_main@PLT

    .weak _init
    .weak _fini

    .section .data.rel.ro._sceLibcParam, "aw", @progbits
    .p2align 3
_sceLibcParam:
    .quad 0x90                              // size
    .long 0x0c                              // entry count
    .long 1
    .quad sceLibcHeapSize
    .quad 0
    .quad sceLibcHeapExtendedAlloc
    .quad 0
    .quad _sceLibcMallocReplace             // + 0x30
    .quad _sceLibcNewReplace                // + 0x38
    .quad 0, 0, 0, 0
    .quad _sceLibcMallocReplaceForTls       // + 0x60
    .zero 0x90 - 0x68

    .section .data.rel.ro._sceKernelMemParam, "aw", @progbits
    .p2align 3
_sceKernelMemParam:
    .quad 0x30
    .zero 0x28

    .section .data.rel.ro._sceKernelFsParam, "aw", @progbits
    .p2align 3
_sceKernelFsParam:
    .quad 0x10
    .quad 0

    .section .data.rel.ro._sceLibcMallocReplace, "aw", @progbits
    .p2align 3
_sceLibcMallocReplace:
    .quad 0x70                              // size
    .quad 1                                 // version
    .quad ps4_replace_malloc_init
    .quad ps4_replace_malloc_finalize
    .quad ps4_replace_malloc
    .quad ps4_replace_free
    .quad ps4_replace_calloc
    .quad ps4_replace_realloc
    .quad ps4_replace_memalign
    .quad ps4_replace_reallocalign
    .quad ps4_replace_posix_memalign
    .quad ps4_replace_malloc_stats
    .quad ps4_replace_malloc_stats_fast
    .quad ps4_replace_malloc_usable_size

    .section .data.rel.ro._sceLibcNewReplace, "aw", @progbits
    .p2align 3
_sceLibcNewReplace:
    .quad 0x70
    .quad 2
    .zero 0x60

    .section .data.rel.ro._sceLibcMallocReplaceForTls, "aw", @progbits
    .p2align 3
_sceLibcMallocReplaceForTls:
    .quad 0x38
    .quad 1
    .zero 0x28

    .section .data.sce_process_param, "aw", @progbits
    .p2align 3
_sceProcessParam:
    .quad 0x50                              // size
    .long 0x4942524f                        // "ORBI"
    .long 3                                 // entry count
    .quad 0x04508101                        // SDK version
    .zero 0x38 - 0x18
    .quad _sceLibcParam
    .quad _sceKernelMemParam
    .quad _sceKernelFsParam

    .data
sceLibcHeapExtendedAlloc:
    .long 1
    .long 0
sceLibcHeapSize:
    .quad 0xffffffffffffffff
sce_libc_heap_delayed_alloc:
    .long 0
sce_libc_heap_extended_alloc:
    .long 0
__dso_handle:
    .quad 0
_sceLibc:
    .quad 0

    .globl __ps4Argv
    .section .bss.__ps4Argv, "aw", @nobits
    .p2align 4
__ps4Argv:
    .zero 0x20

    .section .note.GNU-stack, "", @progbits
