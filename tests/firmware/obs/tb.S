    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .isr_vector, "a"
    .word _estack
    .word tb_entry + 1

    .bss
    .p2align 2
    .global ram_code, ldm_source
    .type ram_code, %object
    .size ram_code, 8
ram_code:
    .space 8
    .type ldm_source, %object
    .size ldm_source, 48
ldm_source:
    .space 48

    .text
    .global Reset_Handler, tb_entry, tb_done
    .type Reset_Handler, %function
    .type tb_entry, %function
Reset_Handler:
tb_entry:
    bl page_cross
    bl wide_at_end
    bl system_ends
    bl flow_ends
    bl more_flow
    bl loop_back
    bl hooked_run
    bl ldm_run
    bl ram_call
    .type tb_done, %function
tb_done:
    bkpt #0
    b tb_done

    .global system_ends
    .type system_ends, %function
system_ends:
    movs r1, #0
    adds r0, r0, #1
    cpsid i
    adds r0, r0, #1
    cpsie i
    adds r0, r0, #1
    msr basepri, r1
    adds r0, r0, #1
    isb
    adds r0, r0, #1
    dsb
    adds r0, r0, #1
    dmb
    adds r0, r0, #1
    sev
    adds r0, r0, #1
    nop
    adds r0, r0, #1
    bx lr

    .global flow_ends, flow_case0, flow_case1, flow_join
    .type flow_ends, %function
flow_ends:
    push {r4, lr}
    movs r0, #0
    cmp r0, #1
    it eq
    bxeq lr
    adds r0, r0, #1
    tbb [pc, r0]
flow_table:
    .byte (flow_case0 - flow_table) / 2
    .byte (flow_case1 - flow_table) / 2
    .type flow_case0, %function
flow_case0:
    adds r0, r0, #2
    b flow_join
    .type flow_case1, %function
flow_case1:
    adds r0, r0, #3
    .type flow_join, %function
flow_join:
    bl flow_leaf
    pop {r4, lr}
    push {lr}
    ldr pc, [sp], #4

    .type flow_leaf, %function
flow_leaf:
    push {lr}
    adds r0, r0, #1
    pop {r4}
    mov lr, r4
    push {lr}
    pop {pc}

    .global more_flow, more_leaf
    .type more_flow, %function
more_flow:
    push.w {r4-r11, lr}
    movs r0, #0
    cbz r0, 1f
    adds r0, r0, #1
1:  cbnz r0, 2f
    adds r0, r0, #1
2:  movs r1, #1
    tbh [pc, r1, lsl #1]
3:  .hword (4f - 3b) / 2
    .hword (5f - 3b) / 2
4:  adds r0, r0, #2
    b 6f
5:  adds r0, r0, #3
6:  ldr r3, =more_leaf
    blx r3
    pop.w {r4-r11, pc}
    .ltorg

    .type more_leaf, %function
more_leaf:
    adds r0, r0, #1
    mov pc, lr

    .global loop_back, loop_top
    .type loop_back, %function
loop_back:
    movs r1, #3
    .type loop_top, %function
loop_top:
    adds r2, r2, #1
    subs r1, r1, #1
    bne loop_top
    bx lr

    .global hooked_run, hooked_site
    .type hooked_run, %function
hooked_run:
    adds r0, r0, #1
    adds r0, r0, #1
    adds r0, r0, #1
    .type hooked_site, %function
hooked_site:
    adds r0, r0, #1
    adds r0, r0, #1
    adds r0, r0, #1
    bx lr

    .global ram_call
    .type ram_call, %function
ram_call:
    push {r4, lr}
    ldr r1, =ram_code
    ldr r2, =0x47703001
    str r2, [r1]
    dsb
    isb
    ldr r4, =ram_code - 0x20000000 + 0x0a000000 + 1
    blx r4
    pop {r4, pc}
    .ltorg

    .global ldm_run
    .type ldm_run, %function
    .p2align 10
ldm_run:
    push {r4-r11, lr}
    ldr r0, =ldm_source
    .rept 200
    ldmia r0, {r1-r12}
    .endr
    pop {r4-r11, pc}
    .ltorg

    .global page_cross, page_cross_next
    .p2align 10
    .fill 508, 2, 0xbf00
    .type page_cross, %function
page_cross:
    adds r0, r0, #1
    adds r0, r0, #1
    adds r0, r0, #1
    adds r0, r0, #1
    .type page_cross_next, %function
page_cross_next:
    adds r0, r0, #1
    adds r0, r0, #1
    bx lr

    .global wide_at_end, wide_insn
    .p2align 10
    .fill 509, 2, 0xbf00
    .type wide_at_end, %function
wide_at_end:
    adds r0, r0, #1
    adds r0, r0, #1
    .type wide_insn, %function
wide_insn:
    add.w r0, r0, #1
    bx lr
