// OneiOS vmprobe: a Linux-Image-format payload of a few hundred instructions for a crosvm VM under the phone's Gunyah
// (docs/native.md "A VM under Gunyah"). It says that it runs, prints the ID registers XNU's choices rest on (TGran16,
// VARange, PAC), the PSCI version and a loop timed on the virtual counter with the MMU off and then on (an identity
// map, write-back RAM), then powers off by PSCI over HVC by itself. One vCPU, no device but the UART at UART_BASE:
// crosvm's 8250 at 0x3f8 (THR at +0), or QEMU virt's PL011 at 0x09000000 (DR at +0) for the PC's check. It never waits in WFE or WFI (a guest parked in WFE and killed from the host
// once reset a phone: UniXNU-droidvm tools/rig/pacprobe/README.md). Every synchronous exception prints ESR, ELR and FAR
// and is stepped over; an asynchronous one, or the 16th, powers off.
//
// Output lines: "ONEIOS VMPROBE start", "NAME 0123456789abcdef" per value, "ONEIOS VMPROBE done", "ONEIOS VMPROBE off".

#ifndef UART_BASE
#define UART_BASE 0x3f8
#endif
// The 1 GiB block of the VM's RAM, mapped write-back once the MMU is on: crosvm's RAM is at 0x80000000 (2), QEMU
// virt's at 0x40000000 (1).
#ifndef RAM_GIB_BLOCK
#define RAM_GIB_BLOCK 2
#endif

        .section .text.head, "ax"
        .globl  _head
_head:
        b       start                   // code0
        .long   0                       // code1
        .quad   0                       // text_offset
        .quad   _end - _head            // image_size
        .quad   0xa                     // flags: little-endian, 4 KiB pages, placed anywhere
        .quad   0, 0, 0                 // res2..res4
        .ascii  "ARM\x64"               // magic
        .long   0                       // res5

        .text
start:
        msr     daifset, #0xf
        mov     x19, x0                 // the device tree's address (Linux's boot protocol)
        adr     x1, stack_top
        mov     sp, x1
        adr     x1, vectors
        msr     vbar_el1, x1
        isb
        mrs     x20, sctlr_el1          // as found
        orr     x1, x20, #(1 << 12)     // the instruction cache on (fetches cacheable with the MMU off): the timed loop
        msr     sctlr_el1, x1
        isb

        adr     x0, s_start
        bl      puts

        .macro  show name, reg
        adr     x0, 98f
        mrs     x1, \reg
        bl      putkv
        .pushsection .rodata
98:     .asciz  "\name "
        .popsection
        .endm

        show    CurrentEL,        S3_0_C4_C2_2
        show    MIDR_EL1,         S3_0_C0_C0_0
        show    MPIDR_EL1,        S3_0_C0_C0_5
        show    REVIDR_EL1,       S3_0_C0_C0_6
        show    ID_AA64PFR0_EL1,  S3_0_C0_C4_0
        show    ID_AA64PFR1_EL1,  S3_0_C0_C4_1
        show    ID_AA64DFR0_EL1,  S3_0_C0_C5_0
        show    ID_AA64ISAR0_EL1, S3_0_C0_C6_0
        show    ID_AA64ISAR1_EL1, S3_0_C0_C6_1
        show    ID_AA64ISAR2_EL1, S3_0_C0_C6_2
        show    ID_AA64MMFR0_EL1, S3_0_C0_C7_0
        show    ID_AA64MMFR1_EL1, S3_0_C0_C7_1
        show    ID_AA64MMFR2_EL1, S3_0_C0_C7_2
        show    CTR_EL0,          S3_3_C0_C0_1
        show    CNTFRQ_EL0,       S3_3_C14_C0_0

        adr     x0, s_sctlr
        mov     x1, x20
        bl      putkv
        adr     x0, s_fdt
        mov     x1, x19
        bl      putkv
        tst     x19, #7                 // the tree's first word (d00dfeed, read little-endian) when it is aligned
        b.ne    1f
        adr     x0, s_fdt_magic
        ldr     w1, [x19]
        bl      putkv
1:
        movz    x0, #0x0000             // PSCI_VERSION over HVC (crosvm's conduit on Gunyah)
        movk    x0, #0x8400, lsl #16
        hvc     #0
        mov     x1, x0
        adr     x0, s_psci
        bl      putkv

        // A two-instruction loop timed on the virtual counter with the MMU off, 2^20 turns: the fetches are what
        // SCTLR_EL1.I and the hypervisor make them (run 1 on the S25: 2^28 turns did not end in 20 s).
        adr     x0, s_sctlr_now
        mrs     x1, sctlr_el1
        bl      putkv
        mrs     x21, S3_3_C14_C0_2      // CNTVCT_EL0
        mov     x2, #(1 << 20)
2:      subs    x2, x2, #1
        b.ne    2b
        mrs     x22, S3_3_C14_C0_2
        adr     x0, s_loop_off
        sub     x1, x22, x21
        bl      putkv

        // The RAM the VM was given, from the device tree's /memory (MMU off: the walk reads aligned words only).
        bl      fdt_memory
        adr     x0, s_mem_base
        mov     x1, x23
        bl      putkv
        adr     x0, s_mem_size
        mov     x1, x24
        bl      putkv
        adr     x0, s_image
        adr     x1, _head
        bl      putkv
        adr     x0, s_image_end
        adr     x1, _end
        bl      putkv

        // The MMU on, as a kernel has it: an identity map of 1 GiB blocks, the UART's (block 0, Device-nGnRnE, never
        // executed) and the RAM's (Normal write-back, inner shareable: every block /memory covers, else block
        // RAM_GIB_BLOCK); 4 KiB granule, 39-bit VA from TTBR0 (TTBR1's walks off), 36-bit IPA (the VM's PARange),
        // non-cacheable table walks.
        adr     x3, table
        movz    x1, #0x0401             // block, AF, AttrIndx 0
        movk    x1, #0x0060, lsl #48    // PXN, UXN
        str     x1, [x3]
        cbz     x24, 11f
        lsr     x4, x23, #30            // the first block of RAM
        add     x5, x23, x24
        sub     x5, x5, #1
        lsr     x5, x5, #30             // the last
        b       12f
11:     mov     x4, #RAM_GIB_BLOCK
        mov     x5, #RAM_GIB_BLOCK
12:     cmp     x4, #1                  // block 0 stays the UART's
        b.hs    13f
        mov     x4, #1
13:     cmp     x5, #511
        b.ls    14f
        mov     x5, #511
14:     lsl     x1, x4, #30
        movz    x2, #0x0705             // block, AttrIndx 1, inner shareable, AF
        orr     x1, x1, x2
        str     x1, [x3, x4, lsl #3]
        add     x4, x4, #1
        cmp     x4, x5
        b.ls    14b
        dsb     sy
        mov     x1, #0xff00             // MAIR: Attr0 Device-nGnRnE, Attr1 Normal write-back RW-allocate
        msr     mair_el1, x1
        movz    x1, #0x3019             // T0SZ 25, SH0 inner, walks non-cacheable, TG0 4 KiB
        movk    x1, #0x0099, lsl #16    // T1SZ 25, EPD1
        movk    x1, #0x0001, lsl #32    // IPS 36 bits
        msr     tcr_el1, x1
        msr     ttbr0_el1, x3
        isb
        tlbi    vmalle1
        ic      iallu
        dsb     sy
        isb
        mrs     x1, sctlr_el1
        orr     x1, x1, #1              // M
        orr     x1, x1, #(1 << 2)       // C
        orr     x1, x1, #(1 << 12)      // I
        msr     sctlr_el1, x1
        isb
        adr     x0, s_sctlr_mmu
        mrs     x1, sctlr_el1
        bl      putkv
        mrs     x21, S3_3_C14_C0_2
        mov     x2, #(1 << 28)          // 2^28 turns: about 70 ms on a 4 GHz core
7:      subs    x2, x2, #1
        b.ne    7b
        mrs     x22, S3_3_C14_C0_2
        adr     x0, s_loop_on
        sub     x1, x22, x21
        bl      putkv

        // Pointer authentication at EL1, as an arm64e kernel uses it: the IA and GA keys written, EnIA, EnIB, EnDA
        // and EnDB set, a pointer signed (PACIA) and authenticated back (AUTIA), a generic PAC (PACGA), then an AUTIA
        // with the wrong modifier: with FEAT_FPAC that one traps (an EXCEPTION line, EC 0x1c), else it gives a
        // pointer with an error code in it. A key write or PAC instruction the hypervisor traps also shows as an
        // EXCEPTION line. With the MMU on (39-bit VA): the PAC sits in bits 39 to 54 and 56 to 63.
        movz    x1, #0x1111
        movk    x1, #0x2222, lsl #16
        movk    x1, #0x3333, lsl #32
        movk    x1, #0x4444, lsl #48
        msr     S3_0_C2_C1_0, x1        // APIAKEYLo_EL1
        msr     S3_0_C2_C1_1, x1        // APIAKEYHi_EL1
        msr     S3_0_C2_C3_0, x1        // APGAKEYLo_EL1
        msr     S3_0_C2_C3_1, x1        // APGAKEYHi_EL1
        isb
        mrs     x1, sctlr_el1
        orr     x1, x1, #(1 << 31)      // EnIA
        orr     x1, x1, #(1 << 30)      // EnIB
        orr     x1, x1, #(1 << 27)      // EnDA
        orr     x1, x1, #(1 << 13)      // EnDB
        msr     sctlr_el1, x1
        isb
        adr     x0, s_sctlr_pac
        mrs     x1, sctlr_el1
        bl      putkv
        adr     x25, _head              // a pointer of ours
        mov     x26, #0x42              // the modifier
        mov     x27, x25
        pacia   x27, x26
        adr     x0, s_pac_signed
        mov     x1, x27
        bl      putkv
        mov     x28, x27
        autia   x28, x26
        adr     x0, s_pac_auth
        mov     x1, x28
        bl      putkv
        pacga   x1, x25, x26
        adr     x0, s_pacga
        bl      putkv
        mov     x28, x27
        mov     x26, #0x43              // the wrong modifier
        autia   x28, x26
        adr     x0, s_pac_wrong
        mov     x1, x28
        bl      putkv

        // Every 4 KiB page of the RAM above the image, but the tree's 2 MiB blocks, gets its own address written and
        // then read back: what a VM of this size costs the host, and whether all of it is there. "MEM_AT" every 256 MiB.
        cbz     x24, 29f
        mov     x9, #0x1fffff           // 2 MiB - 1 (not an add immediate)
        adr     x4, _end
        add     x4, x4, x9
        and     x4, x4, #~0x1fffff      // from: above the image, 2 MiB aligned
        add     x5, x23, x24            // to: the end of RAM
        and     x6, x19, #~0x1fffff     // the tree's blocks: skipped
        ldr     w1, [x19, #4]
        rev     w1, w1
        add     x7, x19, x1
        add     x7, x7, x9
        and     x7, x7, #~0x1fffff
        adr     x0, s_mem_from
        mov     x1, x4
        bl      putkv
        mrs     x21, S3_3_C14_C0_2
        mov     x8, x4
        mov     x15, #0                 // pages written
21:     cmp     x8, x5
        b.hs    23f
        cmp     x8, x6
        b.lo    22f
        cmp     x8, x7
        b.hs    22f
        mov     x8, x7
        b       21b
22:     str     x8, [x8]
        add     x15, x15, #1
        tst     x8, #0xfffffff
        b.ne    24f
        adr     x0, s_mem_at
        mov     x1, x8
        bl      putkv
24:     add     x8, x8, #4096
        b       21b
23:     mrs     x22, S3_3_C14_C0_2
        adr     x0, s_mem_pages
        mov     x1, x15
        bl      putkv
        adr     x0, s_mem_wticks
        sub     x1, x22, x21
        bl      putkv
        mrs     x21, S3_3_C14_C0_2
        mov     x8, x4
        mov     x16, #0                 // pages that do not read back
25:     cmp     x8, x5
        b.hs    27f
        cmp     x8, x6
        b.lo    26f
        cmp     x8, x7
        b.hs    26f
        mov     x8, x7
        b       25b
26:     ldr     x1, [x8]
        cmp     x1, x8
        b.eq    28f
        add     x16, x16, #1
28:     add     x8, x8, #4096
        b       25b
27:     mrs     x22, S3_3_C14_C0_2
        adr     x0, s_mem_bad
        mov     x1, x16
        bl      putkv
        adr     x0, s_mem_rticks
        sub     x1, x22, x21
        bl      putkv
29:
        adr     x0, s_done
        bl      puts
        b       poweroff

// PSCI SYSTEM_OFF over HVC; what it returns, if it returns, once; then again and again, never WFE or WFI.
poweroff:
        adr     x0, s_off
        bl      puts
        movz    x0, #0x0008
        movk    x0, #0x8400, lsl #16
        hvc     #0
        mov     x1, x0
        adr     x0, s_off_ret
        bl      putkv
3:      movz    x0, #0x0008
        movk    x0, #0x8400, lsl #16
        hvc     #0
        b       3b

// fdt_memory: the device tree at x19 (big-endian); x23 = /memory's first base, x24 = its size (two cells each, as
// crosvm and QEMU write them), both 0 when there is none. With the MMU off: aligned words and bytes only. Uses x0..x2,
// x10..x16.
fdt_memory:
        mov     x23, #0
        mov     x24, #0
        tst     x19, #7
        b.ne    49f
        ldr     w1, [x19]
        rev     w1, w1
        movz    w2, #0xfeed
        movk    w2, #0xd00d, lsl #16
        cmp     w1, w2
        b.ne    49f
        ldr     w1, [x19, #8]           // off_dt_struct
        rev     w1, w1
        add     x10, x19, x1            // the token
        ldr     w1, [x19, #12]          // off_dt_strings
        rev     w1, w1
        add     x11, x19, x1
        ldr     w1, [x19, #4]           // totalsize: the walk's bound
        rev     w1, w1
        add     x12, x19, x1
        mov     x13, #0                 // depth: the root's is 1, its children's 2
        mov     x14, #0                 // the memory node's depth while in it, else 0
41:     cmp     x10, x12
        b.hs    49f
        ldr     w1, [x10], #4
        rev     w1, w1
        cmp     w1, #1                  // FDT_BEGIN_NODE
        b.eq    42f
        cmp     w1, #2                  // FDT_END_NODE
        b.eq    45f
        cmp     w1, #3                  // FDT_PROP
        b.eq    46f
        cmp     w1, #4                  // FDT_NOP
        b.eq    41b
        b       49f                     // FDT_END, or a tree this walk does not know
42:     add     x13, x13, #1
        cmp     x13, #2
        b.ne    44f
        ldrb    w1, [x10]               // "memory", then NUL or '@'
        cmp     w1, #'m'
        b.ne    44f
        ldrb    w1, [x10, #1]
        cmp     w1, #'e'
        b.ne    44f
        ldrb    w1, [x10, #2]
        cmp     w1, #'m'
        b.ne    44f
        ldrb    w1, [x10, #3]
        cmp     w1, #'o'
        b.ne    44f
        ldrb    w1, [x10, #4]
        cmp     w1, #'r'
        b.ne    44f
        ldrb    w1, [x10, #5]
        cmp     w1, #'y'
        b.ne    44f
        ldrb    w1, [x10, #6]
        cmp     w1, #'@'
        b.eq    43f
        cbnz    w1, 44f
43:     mov     x14, x13
44:     ldrb    w1, [x10], #1           // past the name and its NUL, to the next word
        cbnz    w1, 44b
        add     x10, x10, #3
        and     x10, x10, #~3
        b       41b
45:     cmp     x14, x13
        b.ne    47f
        mov     x14, #0
47:     sub     x13, x13, #1
        b       41b
46:     ldr     w15, [x10], #4          // len
        rev     w15, w15
        ldr     w16, [x10], #4          // nameoff
        rev     w16, w16
        cbz     x14, 48f
        cmp     x14, x13
        b.ne    48f
        cmp     w15, #16
        b.lo    48f
        add     x0, x11, x16            // the name: "reg"
        ldrb    w1, [x0]
        cmp     w1, #'r'
        b.ne    48f
        ldrb    w1, [x0, #1]
        cmp     w1, #'e'
        b.ne    48f
        ldrb    w1, [x0, #2]
        cmp     w1, #'g'
        b.ne    48f
        ldrb    w1, [x0, #3]
        cbnz    w1, 48f
        ldr     w1, [x10]               // base: two big-endian cells
        rev     w1, w1
        ldr     w2, [x10, #4]
        rev     w2, w2
        orr     x23, x2, x1, lsl #32
        ldr     w1, [x10, #8]           // size
        rev     w1, w1
        ldr     w2, [x10, #12]
        rev     w2, w2
        orr     x24, x2, x1, lsl #32
        b       49f
48:     add     x10, x10, x15           // past the value, to the next word
        add     x10, x10, #3
        and     x10, x10, #~3
        b       41b
49:     ret

// putc: w0 to the UART. Uses x9.
putc:
        mov     x9, #(UART_BASE & 0xffff)
        movk    x9, #((UART_BASE >> 16) & 0xffff), lsl #16
        strb    w0, [x9]
        ret

// puts: the string at x0. Uses x0, x9, x10.
puts:
        stp     x29, x30, [sp, #-16]!
        mov     x10, x0
4:      ldrb    w0, [x10], #1
        cbz     w0, 5f
        bl      putc
        b       4b
5:      ldp     x29, x30, [sp], #16
        ret

// putkv: the label at x0, x1 in 16 hex digits, a newline. Uses x0..x3, x9, x10.
putkv:
        stp     x29, x30, [sp, #-16]!
        stp     x1, xzr, [sp, #-16]!
        bl      puts
        ldp     x1, xzr, [sp], #16
        mov     x2, #60
6:      lsr     x3, x1, x2
        and     x3, x3, #0xf
        cmp     x3, #10
        add     x0, x3, #'0'
        add     x3, x3, #('a' - 10)
        csel    x0, x3, x0, hs
        bl      putc
        subs    x2, x2, #4
        b.pl    6b
        mov     w0, #'\n'
        bl      putc
        ldp     x29, x30, [sp], #16
        ret

// The vectors: x0 = the entry's number (0..15) for exc_common.
        .macro  vent n
        .balign 128
        stp     x0, x1, [sp, #-16]!
        mov     x0, #\n
        b       exc_common
        .endm
        .balign 2048
vectors:
        .irp    n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        vent    \n
        .endr

exc_common:
        stp     x2, x3, [sp, #-16]!
        stp     x9, x10, [sp, #-16]!
        stp     x29, x30, [sp, #-16]!
        mov     x2, x0
        adr     x0, s_exc
        mov     x1, x2
        bl      putkv
        adr     x0, s_esr
        mrs     x1, esr_el1
        bl      putkv
        adr     x0, s_elr
        mrs     x1, elr_el1
        bl      putkv
        adr     x0, s_far
        mrs     x1, far_el1
        bl      putkv
        and     x3, x2, #3              // IRQ, FIQ or SError: nothing to step over
        cbnz    x3, poweroff
        adr     x9, exc_count
        ldr     x10, [x9]
        add     x10, x10, #1
        str     x10, [x9]
        cmp     x10, #16
        b.hs    poweroff
        mrs     x1, elr_el1             // step over the instruction
        add     x1, x1, #4
        msr     elr_el1, x1
        ldp     x29, x30, [sp], #16
        ldp     x9, x10, [sp], #16
        ldp     x2, x3, [sp], #16
        ldp     x0, x1, [sp], #16
        eret

        .section .rodata
s_start:     .asciz "ONEIOS VMPROBE start\n"
s_done:      .asciz "ONEIOS VMPROBE done\n"
s_off:       .asciz "ONEIOS VMPROBE off\n"
s_off_ret:   .asciz "SYSTEM_OFF_returned "
s_sctlr:     .asciz "SCTLR_EL1 "
s_fdt:       .asciz "FDT "
s_fdt_magic: .asciz "FDT_WORD0 "
s_psci:      .asciz "PSCI_VERSION "
s_sctlr_now: .asciz "SCTLR_EL1_ICACHE "
s_loop_off:  .asciz "LOOP_MMUOFF_2E20_TICKS "
s_sctlr_mmu: .asciz "SCTLR_EL1_MMU "
s_loop_on:   .asciz "LOOP_MMUON_2E28_TICKS "
s_sctlr_pac: .asciz "SCTLR_EL1_PAC "
s_pac_signed: .asciz "PAC_SIGNED "
s_pac_auth:  .asciz "PAC_AUTH "
s_pacga:     .asciz "PACGA "
s_pac_wrong: .asciz "PAC_WRONG_AUTH "
s_mem_base:  .asciz "MEM_BASE "
s_mem_size:  .asciz "MEM_SIZE "
s_image:     .asciz "IMAGE "
s_image_end: .asciz "IMAGE_END "
s_mem_from:  .asciz "MEM_FROM "
s_mem_at:    .asciz "MEM_AT "
s_mem_pages: .asciz "MEM_PAGES "
s_mem_wticks: .asciz "MEM_WRITE_TICKS "
s_mem_bad:   .asciz "MEM_BAD_PAGES "
s_mem_rticks: .asciz "MEM_READ_TICKS "
s_exc:       .asciz "EXCEPTION "
s_esr:       .asciz "ESR_EL1 "
s_elr:       .asciz "ELR_EL1 "
s_far:       .asciz "FAR_EL1 "

        .data
        .balign 4096
table:       .space 4096                // the level 1 table: 512 entries, two of them set at run time
exc_count:   .quad 0, 0
stack:       .space 4096
stack_top:
        .balign 16
_end:
