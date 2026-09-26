    .syntax unified
    .cpu cortex-m33
    .fpu fpv5-sp-d16
    .thumb

    .text

    .macro LOGV value
    ldr r2, =log_len
    ldr r3, [r2]
    ldr r1, =log_buf
    and r0, r3, #0xff
    str \value, [r1, r0, lsl #2]
    adds r3, #1
    str r3, [r2]
    .endm

    .global switch_run
    .type switch_run, %function
switch_run:
    bl switch_prepare
    svc #0
    bkpt #0xfd

    .global switch_svc
    .type switch_svc, %function
switch_svc:
    ldr r0, =task_sp
    ldr r0, [r0]
    msr psp, r0
    ldr lr, =0xFFFFFFFD
    bx lr

    .global switch_pendsv
    .type switch_pendsv, %function
switch_pendsv:
    mrs r0, psp
    tst lr, #0x10
    it eq
    vstmdbeq r0!, {s16-s31}
    stmdb r0!, {r4-r11, lr}
    ldr r1, =current_task
    ldr r2, [r1]
    ldr r3, =task_sp
    str r0, [r3, r2, lsl #2]
    ldr r12, =task_sp_first
    ldr lr, [r12, r2, lsl #2]
    cmp lr, #0
    it eq
    streq r0, [r12, r2, lsl #2]
    eor r2, r2, #1
    str r2, [r1]
    ldr r0, [r3, r2, lsl #2]
    ldmia r0!, {r4-r11, lr}
    tst lr, #0x10
    it eq
    vldmiaeq r0!, {s16-s31}
    msr psp, r0
    bx lr

    .global task_a
    .type task_a, %function
task_a:
    mov r12, #0x41
    LOGV r12
    mrs r0, control
    ldr r1, =control_seen
    str r0, [r1]
    bl switch_step
    b task_a

    .global task_b
    .type task_b, %function
task_b:
    ldr r0, =0x44000000
    vmov s16, r0
1:
    mov r12, #0x42
    LOGV r12
    vmov r0, s16
    ldr r1, =b_fp_seen
    str r0, [r1]
    mrs r0, control
    ldr r1, =control_seen
    str r0, [r1, #4]
    bl switch_step
    b 1b

    .pool
