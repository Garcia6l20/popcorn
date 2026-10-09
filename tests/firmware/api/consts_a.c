#include "consts.h"

extern uint32_t _estack;

volatile channel_t     channel;
volatile link_t        link;
volatile link_high_t   link_high;
volatile route_alias_t route_alias;

static volatile enum
{
  MODE_FAST = 1
} mode_a;

typedef enum
{
  SHAPE_SQUARE = 1,
  SHAPE_ROUND,
} shape_t;

static volatile shape_t shape_a;

enum opaque
{
  OPAQUE_A = 4,
};

volatile enum opaque opaque_a;

[[noreturn]] void Reset_Handler(void)
{
  channel     = CHANNEL_MAX;
  link        = LINK_SPI;
  link_high   = LINK_HIGH;
  route_alias = ROUTE_CAN;
  mode_a      = MODE_FAST;
  shape_a     = SHAPE_ROUND;
  opaque_a    = OPAQUE_A;
  touch_b();
  __asm__ volatile("bkpt #0" ::: "memory");
  for(;;)
  {
  }
}

__attribute__((section(".isr_vector"), used)) const void* const vectors[2] = {
    &_estack,
    (const void*)Reset_Handler,
};
