    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .isr_vector, "a"
    .word _estack
    .word read_entry + 1
    .rept 14
    .word 0
    .endr
    .word irq_handler + 1

    .text

    .global Reset_Handler, read_entry, read_done, write_entry, write_done
    .global store_entry, store_done
    .type Reset_Handler, %function
    .type read_entry, %function
Reset_Handler:
read_entry:
    movs r1, #0
1:  ldr r3, [r2]
    adds r1, r1, r3
    subs r0, r0, #1
    bne 1b
    .type read_done, %function
read_done:
    b read_done

    .type write_entry, %function
write_entry:
    movs r1, #0
1:  str r1, [r2]
    adds r1, r1, #1
    subs r0, r0, #1
    bne 1b
    .type write_done, %function
write_done:
    b write_done

    .type store_entry, %function
store_entry:
    movs r1, #0
1:  str r1, [r2]
    ldr r3, [r2, #4]
    adds r1, r1, #1
    subs r0, r0, #1
    bne 1b
    .type store_done, %function
store_done:
    b store_done

    .global irq_handler
    .type irq_handler, %function
irq_handler:
    str r1, [r2]
    bx lr
