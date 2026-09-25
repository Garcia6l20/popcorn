    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .isr_vector, "a"
    .word _estack
    .word alu_entry + 1

    .text

    .global Reset_Handler, alu_entry, alu_body, alu_done
    .type Reset_Handler, %function
    .type alu_entry, %function
Reset_Handler:
alu_entry:
    movs r1, #1
    movs r2, #0
    movs r3, #0
    .type alu_body, %function
alu_body:
    adds r2, r2, r1
    eors r3, r3, r2
    adds r1, r1, #3
    subs r0, r0, #1
    bne alu_body
    b alu_done
    .rept 8
    nop
    .endr
    .type alu_done, %function
alu_done:
    b .

    .global store_entry, store_body, store_done
    .type store_entry, %function
store_entry:
    movs r1, #0
    .type store_body, %function
store_body:
    str r1, [r2]
    str r1, [r2, #4]
    adds r1, r1, #1
    subs r0, r0, #1
    bne store_body
    b store_done
    .rept 8
    nop
    .endr
    .type store_done, %function
store_done:
    b .
