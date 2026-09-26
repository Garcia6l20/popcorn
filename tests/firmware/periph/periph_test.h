#pragma once

#include <stdint.h>

extern uint32_t log_buf[256];
extern uint32_t log_len;

#define LOG(value) (log_buf[log_len++ & 0xffu] = (uint32_t)(value))

#define FAIL_MARK 0xfa110000u

#define DONE()                                  \
  do                                            \
  {                                             \
    for(;;)                                     \
    {                                           \
      __asm__ volatile("bkpt #1" ::: "memory"); \
    }                                           \
  } while(0)

#define FAIL(code)                                 \
  do                                               \
  {                                                \
    LOG(FAIL_MARK | ((uint32_t)(code) & 0xffffu)); \
    for(;;)                                        \
    {                                              \
      __asm__ volatile("bkpt #2" ::: "memory");    \
    }                                              \
  } while(0)
