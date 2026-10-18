  .syntax unified
  .thumb

  .section .text.ns_fp_clobber, "ax", %progbits
  .global ns_fp_clobber
  .type ns_fp_clobber, %function
ns_fp_clobber:
  movw r0, #:lower16:ns_fp_garbage
  movt r0, #:upper16:ns_fp_garbage
  vldmia r0, {s0-s31}
  bx lr
  .size ns_fp_clobber, .-ns_fp_clobber

  .section .rodata.ns_fp_garbage, "a", %progbits
  .balign 4
  .type ns_fp_garbage, %object
ns_fp_garbage:
  .rept 32
  .word 0xA5A5A5A5
  .endr
  .size ns_fp_garbage, .-ns_fp_garbage
