    .syntax unified
    .cpu cortex-m33
    .thumb

    .file 1 "../tests/firmware/obs/shared_exit.S"

    .section .isr_vector, "a"
    .word _estack
    .word Reset_Handler + 1

    .text
    .global Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    .loc 1 16
    movs r0, #0
    .loc 1 18
    bl check
    .global settled
settled:
    .loc 1 22
    b .
    .size Reset_Handler, . - Reset_Handler

    .global check
    .type check, %function
check:
    .loc 1 29
    cmp r0, #0
    beq shared
    .loc 1 32
    cmp r0, #2
    beq.w shared
    movs r0, #1
shared:
    bx lr
    .size check, . - check
