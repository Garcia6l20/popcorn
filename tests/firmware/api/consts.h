#pragma once

#include <stdint.h>

typedef enum
{
  CHANNEL_A,
  CHANNEL_B,
  CHANNEL_MAX,
} channel_t;

enum link
{
  LINK_NONE    = -1,
  LINK_UART    = 3,
  LINK_SPI     = 7,
  LINK_DEFAULT = 3,
};

typedef enum
{
  LINK_HIGH = 0x80000000u,
} link_high_t;

typedef enum link link_t;

typedef enum
{
  ROUTE_CAN = 9,
} route_alias_t;

enum unused
{
  UNUSED_A,
};

typedef uint8_t repeat_t;

#define CONSTS_LIMIT     16
#define CONSTS_MASK      (0xF0u | 0x0Fu)
#define CONSTS_SHIFTED   (1UL << 5)
#define CONSTS_DERIVED   (CONSTS_LIMIT * 2 + 1)
#define CONSTS_ALL_ONES  ((repeat_t) - 1)
#define CONSTS_MINUS_ONE ((CONSTS_LIMIT) - 1)
#define CONSTS_NEGATIVE  (-3)
#define CONSTS_CHANNEL   CHANNEL_B
#define CHANNEL_A        CHANNEL_A
#define CONSTS_SQUARE(x) ((x) * (x))
#define CONSTS_TEXT      "popcorn"
#define CONSTS_SIZE      sizeof(uint32_t)
#define CONSTS_EMPTY
#define CONSTS_LOOP_A CONSTS_LOOP_B
#define CONSTS_LOOP_B CONSTS_LOOP_A

extern volatile repeat_t      repeat;
extern volatile channel_t     channel;
extern volatile link_t        link;
extern volatile link_high_t   link_high;
extern volatile route_alias_t route_alias;

void touch_b(void);
