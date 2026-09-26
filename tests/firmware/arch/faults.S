    .syntax unified
    .cpu cortex-m33
    .fpu fpv5-sp-d16
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

    .pool
