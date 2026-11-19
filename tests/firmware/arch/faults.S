    .syntax unified
    .thumb

    .text

    .macro FAULT_ENTRY name, code
    .global \name
    .type \name, %function
\name:
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    movw r1, #\code
    b fault_log
    .endm

    FAULT_ENTRY hardfault_entry, 0xC10
    FAULT_ENTRY memmanage_entry, 0xC20
    FAULT_ENTRY busfault_entry, 0xC30
    FAULT_ENTRY usagefault_entry, 0xC40

    .global hardfault_udf
    .type hardfault_udf, %function
hardfault_udf:
    udf #3

    .global usage_skip
    .type usage_skip, %function
usage_skip:
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    push {lr}
    bl usage_skip_dispatch
    pop {pc}

    .global bad_return_irq
    .type bad_return_irq, %function
bad_return_irq:
    ldr r0, =0xFFFFFFF5
    bx r0

    .global bad_return_v8_irq
    .type bad_return_v8_irq, %function
bad_return_v8_irq:
    ldr r0, =0xFFFFFFBC
    bx r0

    .global mask_cpsid
    .type mask_cpsid, %function
mask_cpsid:
    cpsid f
    b mask_common

    .global mask_msr
    .type mask_msr, %function
mask_msr:
    movs r0, #1
    msr faultmask, r0
    isb

    .global mask_common
    .type mask_common, %function
mask_common:
    mrs r0, faultmask
    ldr r1, =mask_inside
    str r0, [r1]
    tst lr, #4
    ite eq
    mrseq r2, msp
    mrsne r2, psp
    ldr r3, =mask_resume
    bic r3, r3, #1
    str r3, [r2, #24]
    bx lr

    .global mask_resume
    .type mask_resume, %function
mask_resume:
    mrs r0, faultmask
    ldr r1, =mask_after
    str r0, [r1]
    bkpt #1
    b .

    .global mask_nmi_run
    .type mask_nmi_run, %function
mask_nmi_run:
    ldr r0, =0xE000ED04
    movs r1, #1
    lsls r1, r1, #31
    str r1, [r0]
    dsb
    isb
    b .

    .pool

    .bss
    .align 2
    .global mask_inside
    .type mask_inside, %object
mask_inside:
    .space 4
    .global mask_after
    .type mask_after, %object
mask_after:
    .space 4
