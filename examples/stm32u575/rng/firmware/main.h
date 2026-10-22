#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

#define RNG_KEY_WORDS 16u
#define RNG_MAX_POLLS 2000u

/** @brief Outcome of the key generation, rng_key.status. */
enum rng_status
{
  RNG_STATUS_OK          = 0u,
  RNG_STATUS_SEED_ERROR  = 1u,
  RNG_STATUS_CLOCK_ERROR = 2u,
  RNG_STATUS_REPEATED    = 3u,
  RNG_STATUS_TIMEOUT     = 4u,
};

/** @brief The key the firmware derived, checked by the host. */
struct rng_key
{
  uint32_t status;
  uint32_t words;
  uint32_t key[RNG_KEY_WORDS];
  uint32_t done;
};

extern struct rng_key rng_key;

/** @brief Whether main starts HSI48, the RNG kernel clock. A test clears it to leave HSI48 off. */
extern volatile uint32_t rng_use_hsi48;

/** @brief Called once rng_key is filled. */
void rng_done(void);

#endif
