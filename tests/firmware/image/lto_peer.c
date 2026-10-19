#include "lto.h"

extern uint32_t lto_late;

struct lto_pair lto_global = {3, 4};

uint32_t lto_shared = 9;

static uint8_t lto_peer_hits = 1;

uint32_t lto_late = 11;

uint32_t lto_bump(uint32_t step)
{
  lto_peer_hits = (uint8_t)(lto_peer_hits + 1);
  lto_shared    = lto_shared + step;
  lto_late      = lto_late + lto_shared;
  __builtin_memcpy(&lto_late, &lto_shared, step & 3U);
  lto_global.second = (uint16_t)(lto_global.second + step + lto_peer_hits);
  return lto_global.first;
}
