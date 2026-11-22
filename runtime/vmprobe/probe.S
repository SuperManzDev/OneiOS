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

        // The MMU on, as a kernel has it: an identity map of two 1 GiB blocks, the UART's (block 0, Device-nGnRnE,
        // never executed) and the RAM's (block RAM_GIB_BLOCK, Normal write-back, inner shareable); 4 KiB granule,
        // 39-bit VA from TTBR0 (TTBR1's walks off), 36-bit IPA (the VM's PARange), non-cacheable table walks.
        adr     x3, table
        movz    x1, #0x0401             // block, AF, AttrIndx 0
        movk    x1, #0x0060, lsl #48    // PXN, UXN
        str     x1, [x3]
        movz    x1, #0x0705             // block, AttrIndx 1, inner shareable, AF
        movk    x1, #(RAM_GIB_BLOCK << 14), lsl #16
        str     x1, [x3, #(RAM_GIB_BLOCK * 8)]
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
