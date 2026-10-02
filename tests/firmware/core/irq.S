    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .isr_vector, "a"
    .word _estack
    .word Reset_Handler + 1
    .rept 12
    .word 0
    .endr
    .word irq_handler + 1
    .rept 4
    .word 0
    .endr
    .word irq_handler + 1

    .text

    .global Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    bkpt #0

    .global store_raise, store_raise_next, store_raise_done
    .type store_raise, %function
store_raise:
    str r1, [r2, #0x5c]
    .type store_raise_next, %function
store_raise_next:
    movs r4, #1
    str r1, [r2, #0x58]
    .rept 10
    nop
    .endr
    .type store_raise_done, %function
store_raise_done:
    bkpt #0

    .global read_raise, read_raise_next
    .type read_raise, %function
read_raise:
    ldr r0, [r2, #0x5c]
    .type read_raise_next, %function
read_raise_next:
    movs r4, #1
    str r1, [r2, #0x58]
    .rept 10
    nop
    .endr
    bkpt #0

    .global masked_raise
    .type masked_raise, %function
masked_raise:
    cpsid i
    str r1, [r2, #0x5c]
    movs r4, #1
    str r1, [r2, #0x58]
    cpsie i
    movs r6, #1
    nop
    bkpt #0

    .global it_raise, it_raise_after
    .type it_raise, %function
it_raise:
    cmp r0, r0
    itt eq
    streq r1, [r2, #0x5c]
    moveq r4, #1
    .type it_raise_after, %function
it_raise_after:
    movs r6, #1
    str r1, [r2, #0x58]
    .rept 10
    nop
    .endr
    bkpt #0

    .global it_fault
    .type it_fault, %function
it_fault:
    cmp r0, r0
    itt eq
    streq r1, [r2, #0x5c]
    streq r1, [r8]
    .rept 10
    nop
    .endr
    bkpt #0

    .global jump_back
    .type jump_back, %function
jump_back:
    adds r4, r4, #1
    cmp r4, #1
    it eq
    ldreq pc, [r2, #0x5c]
    bkpt #0

    .global jump_mid, jump_mid_target
    .type jump_mid, %function
jump_mid:
    movs r6, #0
    .type jump_mid_target, %function
jump_mid_target:
    adds r4, r4, #1
    cmp r4, #1
    it eq
    ldreq pc, [r2, #0x5c]
    bkpt #0

    .global jump_ahead, jump_ahead_target
    .type jump_ahead, %function
jump_ahead:
    movs r4, #1
    ldr pc, [r2, #0x5c]
    .space 4096
    .type jump_ahead_target, %function
jump_ahead_target:
    movs r6, #1
    bkpt #0

    .global scs_raise, scs_raise_next
    .type scs_raise, %function
scs_raise:
    str r9, [r10]
    .type scs_raise_next, %function
scs_raise_next:
    movs r4, #1
    str r1, [r2, #0x58]
    .rept 10
    nop
    .endr
    bkpt #0

    .global stm_fault, stm_fault_at
    .type stm_fault, %function
stm_fault:
    nop
    .type stm_fault_at, %function
stm_fault_at:
    stmia r6!, {r1, r3}
    .rept 10
    nop
    .endr
    bkpt #0

    .global twice
    .type twice, %function
twice:
    adds r0, r0, r0
    bx lr

    .global irq_handler
    .type irq_handler, %function
irq_handler:
    str r3, [r2, #0x58]
    mov r5, r4
    mov r7, r6
    bx lr
