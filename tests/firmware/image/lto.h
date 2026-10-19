#pragma once

#include <stdint.h>

struct lto_pair
{
  uint32_t first;
  uint16_t second;
};

extern struct lto_pair lto_global;

uint32_t lto_bump(uint32_t step);
