#pragma once

#include <stdint.h>

static inline uint32_t shapes_square(uint32_t x)
{
  return x * x;
}

uint32_t shapes_perimeter(uint32_t side);
