    .syntax unified
    .thumb

    .equ NVIC_ISPR0, 0xE000E200

#ifdef POPCORN_NO_DSP
#define APSR_FLAGS APSR_nzcvq
#else
#define APSR_FLAGS APSR_nzcvqg
#endif

    .text

    .macro PATTERN
    ldr r0, =0xF80A0000
    msr APSR_FLAGS, r0
    ldr r0, =0xA0A00000
    ldr r1, =0xA1A10001
    ldr r2, =0xA2A20002
    ldr r3, =0xA3A30003
    ldr r4, =0xA4A40004
    ldr r5, =0xA5A50005
    ldr r6, =0xA6A60006
    ldr r7, =0xA7A70007
    ldr r8, =0xA8A80008
    ldr r9, =0xA9A90009
    ldr r10, =0xAAAA000A
    ldr r11, =0xABAB000B
    ldr r12, =0xACAC000C
    .endm

    .macro FP_PATTERN
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ldr r0, =(0xB0000000 + \i)
    vmov s\i, r0
    .endr
    ldr r0, =0x03C00000
    vmsr fpscr, r0
    .endm

    .macro PROBE
    bkpt #2
    bkpt #3
    b .
    .endm

    .macro TO_PSP control
    ldr r0, =probe_psp_top
    msr psp, r0
    movs r0, #\control
    msr control, r0
    isb
    .endm

    .macro PEND irq
    ldr r0, =NVIC_ISPR0
    ldr r1, =(1 << \irq)
    str r1, [r0]
    dsb
    isb
    .endm

    .macro ENTRY name
    .global \name
    .type \name, %function
\name:
    .endm

    ENTRY regs_thread_msp
    PATTERN
    PROBE

#ifndef POPCORN_NO_FPU
    ENTRY regs_thread_msp_fp
    FP_PATTERN
    PATTERN
    PROBE
#endif

    ENTRY regs_thread_psp
    TO_PSP 2
    PATTERN
    PROBE

#ifndef POPCORN_NO_FPU
    ENTRY regs_thread_psp_fp
    TO_PSP 2
    FP_PATTERN
    PATTERN
    PROBE
#endif

    ENTRY regs_thread_psp_unpriv
    TO_PSP 2
    movs r0, #3
    msr control, r0
    isb
    PATTERN
    PROBE

    ENTRY regs_misaligned
    mov r0, sp
    bic r0, r0, #7
    sub r0, r0, #4
    mov sp, r0
    PATTERN
    PROBE

    ENTRY regs_nested
    PEND 12
    b .

#ifndef POPCORN_NO_FPU
    ENTRY regs_nested_fp
    PEND 13
    b .
#endif

    ENTRY nested_body
    PATTERN
    PROBE

#ifndef POPCORN_NO_FPU
    ENTRY nested_fp_body
    FP_PATTERN
    PATTERN
    PROBE
#endif

    ENTRY clobber_irq
    bkpt #4
    ldr r0, =0x00000000
    msr APSR_FLAGS, r0
#ifndef POPCORN_NO_FPU
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    ldr r0, =(0xC0000000 + \i)
    vmov s\i, r0
    .endr
    movs r0, #0
    vmsr fpscr, r0
#endif
    ldr r0, =0xDEAD0000
    ldr r1, =0xDEAD0001
    ldr r2, =0xDEAD0002
    ldr r3, =0xDEAD0003
    ldr r12, =0xDEAD000C
    bx lr

    ENTRY tail_chain
    movs r7, #0
    bkpt #2
    adds r7, #1
    adds r7, #1
    bkpt #3
    b .

    ENTRY tail_a
    bkpt #5
    bx lr

    ENTRY tail_b
    bkpt #6
    bx lr

    ENTRY faultmask_to_unpriv
    TO_PSP 2
    movs r0, #3
    msr control, r0
    isb
    PEND 16
    mrs r0, faultmask
    ldr r1, =faultmask_seen
    str r0, [r1]
    mrs r0, control
    str r0, [r1, #4]
    bkpt #7
    b .

    ENTRY faultmask_irq
    movs r0, #1
    msr faultmask, r0
    bx lr

    .pool

    .bss
    .align 3
    .global faultmask_seen
    .type faultmask_seen, %object
faultmask_seen:
    .space 8
probe_stack:
    .space 1024
    .global probe_psp_top
probe_psp_top:
