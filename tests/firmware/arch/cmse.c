#include "arch_test.h"

typedef void __attribute__((cmse_nonsecure_call)) nonsecure_fn(void);

uintptr_t cmse_callee;

void cmse_call(void)
{
  nonsecure_fn* const callee = (nonsecure_fn*)cmse_callee;
  callee();
  __asm__ volatile("bkpt #2" ::: "memory");
  for(;;)
  {
  }
}
