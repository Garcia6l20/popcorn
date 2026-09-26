    .syntax unified
    .cpu cortex-m33
    .fpu fpv5-sp-d16
    .thumb

    .equ NVIC_ISPR0, 0xE000E200

    .text

    .macro LOGV value
    ldr r2, =log_len
    ldr r3, [r2]
    ldr r1, =log_buf
    and r0, r3, #0xff
    str \value, [r1, r0, lsl #2]
    adds r3, #1
    str r3, [r2]
    .endm

    .global fpu_check
    .type fpu_check, %function
fpu_check:
    push {r4, lr}
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    ldr r1, =(0x3F800000 + \i)
    vmov s\i, r1
    .endr
    ldr r1, =0x00C0001F
    vmsr fpscr, r1
    ldr r2, =NVIC_ISPR0
    mov r3, #(1 << 8)
    str r3, [r2]
    dsb
    isb
    movs r4, #1
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vmov r1, s\i
    ldr r3, =(0x3F800000 + \i)
    cmp r1, r3
    it ne
    movne r4, #0
    .endr
    vmrs r1, fpscr
    ldr r3, =0x00C0001F
    cmp r1, r3
    it ne
    movne r4, #0
    mov r0, r4
    pop {r4, pc}

    .global fpu_check_irq
    .type fpu_check_irq, %function
fpu_check_irq:
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    ldr r1, =(0xDEAD0000 + \i)
    vmov s\i, r1
    .endr
    movs r1, #0
    vmsr fpscr, r1
    ldr r12, =0x710
    LOGV r12
    bx lr

    .global psp_section
    .type psp_section, %function
psp_section:
    push {r4, lr}
    ldr r0, =psp_stack_top
    msr psp, r0
    movs r0, #2
    msr control, r0
    isb
    ldr r2, =NVIC_ISPR0
    mov r3, #(1 << 9)
    str r3, [r2]
    dsb
    isb
    movs r0, #0
    msr control, r0
    isb
    pop {r4, pc}

    .global psp_irq
    .type psp_irq, %function
psp_irq:
    push {r4, lr}
    ldr r4, =0x900
    LOGV r4
    ldr r4, [sp, #4]
    LOGV r4
    mrs r4, psp
    LOGV r4
    pop {r4, pc}

    .global it_irq_test
    .type it_irq_test, %function
it_irq_test:
    movs r1, #0
    movs r2, #0
    movs r3, #0
    cmp r1, r1
    itte eq
    moveq r1, #1
    .global it_irq_label
    .type it_irq_label, %function
it_irq_label:
    moveq r2, #1
    movne r3, #1
    orr r0, r1, r2, lsl #1
    orr r0, r0, r3, lsl #2
    bx lr

    .global it_irq
    .type it_irq, %function
it_irq:
    push {r4, lr}
    ldr r4, =0xA10
    LOGV r4
    pop {r4, pc}

    .pool

    .bss
    .align 3
psp_stack:
    .space 1024
    .global psp_stack_top
    .type psp_stack_top, %object
psp_stack_top:
