#include "shapes.h"

#include <stdint.h>

__attribute__((noinline)) static uint32_t scale(uint32_t x)
{
  return x + 1;
}

uint8_t zero_buffer[64];

uint32_t shapes_perimeter(uint32_t side)
{
  return scale(side) * 4;
}
