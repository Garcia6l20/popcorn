    .syntax unified
    .cpu cortex-m33
    .fpu fpv5-sp-d16
    .thumb

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

    .global svc_entry
    .type svc_entry, %function
svc_entry:
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    mov r1, lr
    b svc_dispatch

    .global hardfault_entry
    .type hardfault_entry, %function
hardfault_entry:
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    b hardfault_dispatch

    .type svc_bkpt, %function
svc_bkpt:
    bkpt #2
    bx lr

    .global svc_regs
    .type svc_regs, %function
svc_regs:
    ldr r0, =ram_vector
    ldr r1, =svc_bkpt
    str r1, [r0, #44]
    dsb
    isb
    bkpt #3
    .global svc_regs_at
    .type svc_regs_at, %function
svc_regs_at:
    svc #5
    bkpt #4
    bkpt #1

    .global svc_psp
    .type svc_psp, %function
svc_psp:
    ldr r0, =svc_psp_top
    ldr r0, [r0]
    msr psp, r0
    movs r0, #2
    msr control, r0
    isb
    svc #6
    movs r0, #0
    msr control, r0
    isb
    bkpt #1

    .global svc_unpriv
    .type svc_unpriv, %function
svc_unpriv:
    ldr r0, =svc_psp_top
    ldr r0, [r0]
    msr psp, r0
    movs r0, #3
    msr control, r0
    isb
    bkpt #3
    svc #10
    bkpt #4
    mrs r4, control
    ldr r12, =0x8FE
    LOGV r12
    LOGV r4
    bkpt #1

    .global svc_escalate
    .type svc_escalate, %function
svc_escalate:
    cpsid i
    .global svc_escalate_at
    .type svc_escalate_at, %function
svc_escalate_at:
    svc #8
    cpsie i
    bkpt #1

    .global faultmask_svc
    .type faultmask_svc, %function
faultmask_svc:
    movs r0, #1
    msr faultmask, r0
    isb
    svc #11
    .global faultmask_svc_after
    .type faultmask_svc_after, %function
faultmask_svc_after:
    bkpt #1

    .global svc_until
    .type svc_until, %function
svc_until:
    svc #12
    .global svc_until_after
    .type svc_until_after, %function
svc_until_after:
    nop
    bkpt #1

    .pool
