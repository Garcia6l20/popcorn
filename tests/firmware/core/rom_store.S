    .syntax unified
    .cpu cortex-m33
    .fpu fpv5-sp-d16
    .thumb

    .section .isr_vector, "a"
    .word _estack
    .word Reset_Handler + 1
    .rept 14
    .word 0
    .endr

    .text

    .global Reset_Handler, str_entry, strh_entry, strb_entry, strd_entry, vstr_entry
    .global two_entry
    .type Reset_Handler, %function
Reset_Handler:
    bkpt #0

    .type str_entry, %function
str_entry:
    str r1, [r2]
    adds r0, r0, #1
    bkpt #0

    .type strh_entry, %function
strh_entry:
    strh r1, [r2]
    adds r0, r0, #1
    bkpt #0

    .type strb_entry, %function
strb_entry:
    strb r1, [r2]
    adds r0, r0, #1
    bkpt #0

    .type strd_entry, %function
strd_entry:
    strd r1, r3, [r2]
    adds r0, r0, #1
    bkpt #0

    .type two_entry, %function
two_entry:
    str r1, [r2]
    str r3, [r2, #4]
    adds r0, r0, #1
    bkpt #0

    .type vstr_entry, %function
vstr_entry:
    ldr r4, =0xE000ED88
    ldr r5, [r4]
    orr r5, r5, #0x00F00000
    str r5, [r4]
    dsb
    isb
    vmov d0, r1, r3
    vstr d0, [r2]
    adds r0, r0, #1
    bkpt #0
    .ltorg
