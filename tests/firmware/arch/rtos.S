    .syntax unified
    .thumb

    .text

    .equ TASK_SP, 0
    .equ TASK_PSPLIM, 4
    .equ TASK_EXC_RETURN, 8
    .equ MAX_SYSCALL, 0x40
    .equ FP_MARK, 0x40490FDB

    .global start_scheduler
    .type start_scheduler, %function
start_scheduler:
    ldr r0, =0xE000ED08
    ldr r0, [r0]
    ldr r0, [r0]
    msr msp, r0
    cpsie i
    cpsie f
    dsb
    isb
    svc #0
    bkpt #0xfc

    .global SVC_Handler
    .type SVC_Handler, %function
SVC_Handler:
    ldr r3, =current
    ldr r1, [r3]
    ldr r0, [r1, #TASK_SP]
    ldr r2, [r1, #TASK_PSPLIM]
    ldr lr, [r1, #TASK_EXC_RETURN]
    ldmia r0!, {r4-r11}
    msr psplim, r2
    msr psp, r0
    movs r0, #2
    msr control, r0
    isb
    bx lr

    .global PendSV_Handler
    .type PendSV_Handler, %function
PendSV_Handler:
    mrs r0, psp
    tst lr, #0x10
    it eq
    vstmdbeq r0!, {s16-s31}
    stmdb r0!, {r4-r11}
    ldr r3, =current
    ldr r1, [r3]
    str r0, [r1, #TASK_SP]
    mrs r2, psplim
    str r2, [r1, #TASK_PSPLIM]
    str lr, [r1, #TASK_EXC_RETURN]
    movs r0, #MAX_SYSCALL
    msr basepri, r0
    dsb
    isb
    bl rtos_select
    movs r0, #0
    msr basepri, r0
    ldr r3, =current
    ldr r1, [r3]
    ldr r0, [r1, #TASK_SP]
    ldr r2, [r1, #TASK_PSPLIM]
    ldr lr, [r1, #TASK_EXC_RETURN]
    ldmia r0!, {r4-r11}
    tst lr, #0x10
    it eq
    vldmiaeq r0!, {s16-s31}
    msr psplim, r2
    msr psp, r0
    isb
    bx lr

    .global task_a
    .type task_a, %function
task_a:
    mrs r1, psp
    movs r0, #0
    bl task_probe
    ldr r0, =tick
    ldr r0, [r0]
    vmov s16, r0
    vmov s31, r0
    bl task_a_step
    b task_a

    .global task_b
    .type task_b, %function
task_b:
    ldr r0, =FP_MARK
    vmov s16, r0
1:
    mrs r1, psp
    movs r0, #2
    bl task_probe
    bl task_b_step
    vmov r0, s16
    ldr r1, =FP_MARK
    bl fp_check
    b 1b

    .global task_idle
    .type task_idle, %function
task_idle:
    wfi
    b task_idle

    .global task_exit
    .type task_exit, %function
task_exit:
    bkpt #0xfb
    b task_exit

    .pool
