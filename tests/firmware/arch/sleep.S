    .syntax unified
    .thumb

    .equ SYST_CSR, 0xE000E010
    .equ SYST_RVR, 0xE000E014
    .equ SYST_CVR, 0xE000E018
    .equ DEMCR, 0xE000EDFC
    .equ DWT_CTRL, 0xE0001000
    .equ DWT_CYCCNT, 0xE0001004

    .text

    .macro CYC_ON
    ldr r1, =DEMCR
    ldr r2, [r1]
    orr r2, r2, #(1 << 24)
    str r2, [r1]
    ldr r1, =DWT_CTRL
    ldr r2, [r1]
    orr r2, r2, #1
    str r2, [r1]
    .endm

    .macro SPIN count
    movw r3, #\count
1:
    subs r3, #1
    bne 1b
    .endm

    .balign 128
    .global tick_phase
    .type tick_phase, %function
tick_phase:
    ldr r1, =SYST_RVR
    movw r2, #3999
    str r2, [r1]
    ldr r1, =SYST_CVR
    str r2, [r1]
    ldr r1, =SYST_CSR
    movs r2, #7
    str r2, [r1]
    SPIN 10
    ldr r1, =SYST_CVR
    ldr r0, [r1]
    bkpt #2
2:
    wfi
    b 2b

    .size tick_phase, . - tick_phase

    .balign 128
    .global cvr_pair
    .type cvr_pair, %function
cvr_pair:
    ldr r1, =SYST_RVR
    ldr r2, =0x00FFFFFF
    str r2, [r1]
    ldr r1, =SYST_CVR
    str r2, [r1]
    ldr r1, =SYST_CSR
    movs r2, #5
    str r2, [r1]
    SPIN 10
    ldr r1, =SYST_CVR
    ldr r4, [r1]
    bkpt #2
    SPIN 1000
    ldr r5, [r1]
    bkpt #3

    .size cvr_pair, . - cvr_pair

    .balign 128
    .global cyc_pair
    .type cyc_pair, %function
cyc_pair:
    CYC_ON
    ldr r1, =DWT_CYCCNT
    ldr r4, [r1]
    bkpt #2
    SPIN 1000
    ldr r5, [r1]
    bkpt #3

    .size cyc_pair, . - cyc_pair

    .balign 128
    .global cyc_same_block
    .type cyc_same_block, %function
cyc_same_block:
    CYC_ON
    SPIN 10
    ldr r1, =DWT_CYCCNT
    ldr r4, [r1]
    ldr r5, [r1]
    bkpt #2

    .size cyc_same_block, . - cyc_same_block

    .balign 128
    .global cyc_frozen
    .type cyc_frozen, %function
cyc_frozen:
    CYC_ON
    ldr r1, =DEMCR
    ldr r2, [r1]
    bic r2, r2, #(1 << 24)
    str r2, [r1]
    ldr r1, =DWT_CYCCNT
    ldr r4, [r1]
    SPIN 100
    ldr r5, [r1]
    bkpt #2

    .size cyc_frozen, . - cyc_frozen

    .balign 128
    .global cyc_restart
    .type cyc_restart, %function
cyc_restart:
    CYC_ON
    SPIN 100
    ldr r1, =DWT_CYCCNT
    movs r2, #0
    str r2, [r1]
    ldr r4, [r1]
    bkpt #2

    .size cyc_restart, . - cyc_restart

    .balign 128
    .global rvr_race
    .type rvr_race, %function
rvr_race:
    CYC_ON
    ldr r1, =SYST_RVR
    movs r2, #99
    str r2, [r1]
    ldr r0, =SYST_CVR
    str r2, [r0]
    ldr r0, =SYST_CSR
    ldr r6, =DWT_CYCCNT
    movs r7, #0
    movs r2, #7
    str r2, [r0]
    str r7, [r6]
    movw r3, #300
3:
    movs r2, #99
    str r2, [r1]
    subs r3, #1
    bne 3b
    ldr r4, [r6]
    movs r2, #5
    str r2, [r0]
    bkpt #2

    .size rvr_race, . - rvr_race

    .global wfi_empty
    .type wfi_empty, %function
wfi_empty:
    wfi
    bkpt #1

    .global wfi_wide
    .type wfi_wide, %function
wfi_wide:
    wfi.w
    bkpt #1

    .global wfi_spot
    .type wfi_spot, %function
wfi_spot:
    wfi
    nop
    nop
    bkpt #1

    .global return_only
    .type return_only, %function
return_only:
    bx lr

    .global wfi_tick
    .type wfi_tick, %function
wfi_tick:
    ldr r1, =SYST_RVR
    movw r2, #3999
    str r2, [r1]
    ldr r1, =SYST_CVR
    str r2, [r1]
    ldr r1, =SYST_CSR
    movs r2, #7
    str r2, [r1]
    .global wfi_tick_at
    .type wfi_tick_at, %function
wfi_tick_at:
    wfi
    nop
    b wfi_tick_at

    .pool
