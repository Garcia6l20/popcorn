#include "lto.h"

struct lto_pair lto_global = {3, 4};

static uint8_t lto_peer_hits = 1;

uint32_t lto_bump(uint32_t step)
{
  lto_peer_hits     = (uint8_t)(lto_peer_hits + 1);
  lto_global.second = (uint16_t)(lto_global.second + step + lto_peer_hits);
  return lto_global.first;
}
