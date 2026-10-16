    .syntax unified
    .cpu cortex-m33
    .fpu fpv5-sp-d16
    .thumb

    .section .isr_vector, "a"
    .word _estack
    .word Reset_Handler + 1
    .rept 17
    .word 0
    .endr
    .word irq_handler + 1

    .text

    .global Reset_Handler, str_entry, strh_entry, strb_entry, strd_entry, vstr_entry
    .global two_entry, ldr_entry, ldrd_entry, ldm_entry, reread_entry, enable_irq_entry
    .global irq_entry, irq_handler, vldr_entry, reread_store_entry
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

    .type ldr_entry, %function
ldr_entry:
    ldr r1, [r2]
    adds r0, r0, #1
    bkpt #0

    .type ldrd_entry, %function
ldrd_entry:
    ldrd r1, r3, [r2]
    adds r0, r0, #1
    bkpt #0

    .type ldm_entry, %function
ldm_entry:
    ldm r2, {r1, r3, r4, r6}
    adds r0, r0, #1
    bkpt #0

    .type reread_entry, %function
reread_entry:
    ldr r1, [r2]
    ldr r3, [r2]
    adds r0, r0, #1
    bkpt #0

    .type enable_irq_entry, %function
enable_irq_entry:
    ldr r4, =0xE000E100
    movs r3, #8
    str r3, [r4]
    dsb
    isb
    bkpt #0
    .ltorg

    .type irq_entry, %function
irq_entry:
    ldr r1, [r2]
    adds r5, r5, #1
    adds r0, r0, #1
    bkpt #0

    .type irq_handler, %function
irq_handler:
    mov r6, r5
    bx lr

    .type vldr_entry, %function
vldr_entry:
    ldr r4, =0xE000ED88
    ldr r5, [r4]
    orr r5, r5, #0x00F00000
    str r5, [r4]
    dsb
    isb
    vldr d0, [r2]
    adds r0, r0, #1
    bkpt #0
    .ltorg

    .type reread_store_entry, %function
reread_store_entry:
    ldr r1, [r2]
    str r1, [r4]
    ldr r3, [r2]
    adds r0, r0, #1
    bkpt #0
